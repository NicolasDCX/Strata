#include "Geometry/EditRegion.h"

#include "Core/Assert.h"
#include "Core/Timer.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace se::geo {

namespace {

/// Sets bit `index` and appends `index` to `out` at `count` when the bit was
/// clear. Branchless: whether an id is new follows no pattern a predictor
/// could learn (every triangle comes up once per moved corner), so the slot is
/// always written and `count` advances by 0 or 1.
inline void claimInto(u64* bits, u32 index, u32* out, std::size_t& count) noexcept
{
    u64& word = bits[index >> 6];
    const u64 bit = u64{1} << (index & 63u);
    const u64 old = word;
    word = old | bit;
    out[count] = index;
    count += (old & bit) == 0 ? 1u : 0u;
}

/// Grows `list` so that `count + extra` slots are writable.
inline void ensureRoom(std::vector<u32>& list, std::size_t count, std::size_t extra)
{
    if (list.size() < count + extra) {
        list.resize(std::max(list.size() * 2, count + extra + 1024));
    }
}

/// Per-vertex order stamps of the parallel walk (see collectParallel). All
/// kNoStamp between calls. One array for the process, sized for the largest
/// mesh walked, used by one walk at a time (g_stampMutex), so a walk may run on
/// any thread (a helper, when it overlaps a dab's preparation) without each
/// thread growing its own copy.
constexpr u32 kNoStamp = 0xFFFFFFFFu;
constexpr u32 kTaken = 0xFFFFFFFEu;    ///< one-ring vertex already listed
constexpr u32 kRingFlag = 0x80000000u; ///< stamps below it: moved vertex indices
std::vector<u32> t_stamps;
std::mutex g_stampMutex;
std::atomic<std::size_t> g_stampBytes{0};
EditRegion::ParallelSteps g_steps;

std::vector<u32>& stampsFor(u32 vertexCount)
{
    if (t_stamps.size() < vertexCount) {
        const std::size_t before = t_stamps.capacity();
        t_stamps.assign(vertexCount, kNoStamp);
        g_stampBytes.fetch_add((t_stamps.capacity() - before) * sizeof(u32), std::memory_order_relaxed);
    }
    return t_stamps;
}

inline u32 loadRelaxed(u32& slot) noexcept
{
    return std::atomic_ref<u32>(slot).load(std::memory_order_relaxed);
}

/// Lowers `slot` to `value` unless it already holds a smaller one.
inline void atomicMin(u32& slot, u32 value) noexcept
{
    std::atomic_ref<u32> ref(slot);
    u32 current = ref.load(std::memory_order_relaxed);
    while (value < current && !ref.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

std::size_t EditRegion::parallelStampBytes() noexcept
{
    return g_stampBytes.load(std::memory_order_relaxed);
}

EditRegion::ParallelSteps EditRegion::parallelSteps() noexcept
{
    return g_steps;
}

void EditRegion::resetParallelSteps() noexcept
{
    g_steps = {};
}

void EditRegion::collect(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem* jobs)
{
    m_lastParallel = jobs != nullptr && jobs->workerCount() > 0 && movedVertices.size() >= kParallelMinVertices;
    if (m_lastParallel) {
        collectParallel(mesh, movedVertices, *jobs);
    } else {
        collectSerial(mesh, movedVertices);
    }
}

void EditRegion::collectParallel(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem& jobs)
{
    // The serial walk's order, rebuilt from first occurrences:
    // - vertices() starts with the moved vertices, each at its first index i
    //   in movedVertices;
    // - a triangle first appears in the row of its earliest moved corner (the
    //   smallest i among its moved corners), at its place in that row; each
    //   row is walked in order, so every triangle is emitted by that row only;
    // - a one-ring vertex first appears at its first corner in the triangle
    //   list: in the earliest row that emits a triangle with it, in the first
    //   such triangle of that row, at its lowest corner index there.
    // One stamp per vertex: i for moved vertices; kRingFlag | i for one-ring
    // vertices, i being the earliest row that emits one of their triangles;
    // kNoStamp otherwise. Rows record their one-ring corners as candidates,
    // in order, and only the earliest row's first candidate of each vertex is
    // kept. Fixed blocks of rows keep their finds in order and are joined by
    // prefix sums, so the triangles are walked once.
    const std::scoped_lock stampLock(g_stampMutex);
    std::vector<u32>& stamps = stampsFor(mesh.vertexCount());
    u32* const stamp = stamps.data();
    const std::size_t movedCount = movedVertices.size();
    SE_ASSERT(movedCount < kRingFlag);

    core::Stopwatch step;
    // 1. First index of every moved vertex.
    core::parallelFor(&jobs, movedCount, 4096, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            atomicMin(stamp[movedVertices[i]], static_cast<u32>(i));
        }
    });
    g_steps.ms[0] += step.elapsedMs();
    step.reset();

    // 2. Per block of rows: the moved vertices at their first index, the
    //    triangles whose earliest moved corner they are, in row order, and the
    //    one-ring corners of those triangles, in order (candidates).
    const std::size_t vertexBlocks = (movedCount + kParallelVertexBlock - 1) / kParallelVertexBlock;
    if (m_blocks.size() < vertexBlocks) {
        m_blocks.resize(vertexBlocks);
    }
    const PagedArrayView<const Mesh::Triangle> corners = mesh.triangles();
    core::parallelFor(&jobs, vertexBlocks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t b = first; b < last; ++b) {
            Block& block = m_blocks[b];
            block.moved.clear();
            block.candidates.clear();
            std::size_t count = 0;
            const std::size_t end = std::min(movedCount, (b + 1) * kParallelVertexBlock);
            for (std::size_t i = b * kParallelVertexBlock; i < end; ++i) {
                const VertexId vertex = movedVertices[i];
                const u32 self = static_cast<u32>(i);
                if (stamp[vertex] != self) {
                    continue; // a repeat: its row adds nothing the first did not
                }
                block.moved.push_back(vertex);
                const AdjacencyRow row = mesh.incidentTriangles(vertex);
                ensureRoom(block.triangles, count, row.size());
                u32* const out = block.triangles.data();
                row.forEach([&](TriangleId triangle) {
                    // The row owns the triangle when no other moved corner
                    // comes earlier. The corner that is `vertex` holds `self`,
                    // distinct moved vertices hold distinct indices and every
                    // other stamp is kRingFlag or more, so it is enough that
                    // the two other corners hold `self` or more (a corner equal
                    // to `vertex` in a degenerate triangle holds `self` too).
                    // Other rows lower one-ring stamps meanwhile: atomic loads.
                    const Mesh::Triangle& c = corners[triangle];
                    const VertexId a = c[0] == vertex ? c[1] : c[0]; // a comes before d in the triangle
                    const VertexId d = c[2] == vertex ? c[1] : c[2];
                    const u32 sa = loadRelaxed(stamp[a]);
                    const u32 sd = loadRelaxed(stamp[d]);
                    const bool owned = (sa >= self) & (sd >= self);
                    // Branchless append: ownership follows no pattern.
                    out[count] = triangle;
                    count += owned ? 1u : 0u;
                    // One-ring corners are rare (the border of the region).
                    if (owned & ((sa >= kRingFlag) | (sd >= kRingFlag))) {
                        if (sa >= kRingFlag) {
                            atomicMin(stamp[a], kRingFlag | self);
                            block.candidates.push_back(a);
                            block.candidates.push_back(self);
                        }
                        if (sd >= kRingFlag) {
                            atomicMin(stamp[d], kRingFlag | self);
                            block.candidates.push_back(d);
                            block.candidates.push_back(self);
                        }
                    }
                });
            }
            block.triangleCount = count;
        }
    });
    g_steps.ms[1] += step.elapsedMs();
    step.reset();

    // 3. Join the triangles. Keep each block's candidates whose row holds the
    //    minimum, the first one of each vertex: only the block of that row can
    //    match, and it marks the vertex taken.
    m_offsets.resize(vertexBlocks + 1);
    m_offsets[0] = 0;
    std::size_t movedUnique = 0;
    for (std::size_t b = 0; b < vertexBlocks; ++b) {
        m_offsets[b + 1] = m_offsets[b] + m_blocks[b].triangleCount;
        movedUnique += m_blocks[b].moved.size();
    }
    const std::size_t triangleCount = m_offsets[vertexBlocks];
    if (m_triangles.size() < triangleCount) {
        m_triangles.resize(triangleCount + triangleCount / 4);
    }
    core::parallelFor(&jobs, vertexBlocks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t b = first; b < last; ++b) {
            Block& block = m_blocks[b];
            std::copy_n(block.triangles.begin(), block.triangleCount, m_triangles.begin() + static_cast<std::ptrdiff_t>(m_offsets[b]));
            block.ring.clear();
            const std::vector<u32>& candidates = block.candidates;
            for (std::size_t c = 0; c < candidates.size(); c += 2) {
                const VertexId vertex = candidates[c];
                const std::atomic_ref<u32> slot(stamp[vertex]);
                if (slot.load(std::memory_order_relaxed) == (kRingFlag | candidates[c + 1])) {
                    slot.store(kTaken, std::memory_order_relaxed);
                    block.ring.push_back(vertex);
                }
            }
        }
    });
    g_steps.ms[2] += step.elapsedMs();
    step.reset();

    // 4. Vertex list: the moved vertices, then the one-ring, block by block.
    m_movedAt.resize(vertexBlocks + 1);
    m_ringAt.resize(vertexBlocks + 1);
    m_movedAt[0] = 0;
    m_ringAt[0] = movedUnique;
    for (std::size_t b = 0; b < vertexBlocks; ++b) {
        m_movedAt[b + 1] = m_movedAt[b] + m_blocks[b].moved.size();
        m_ringAt[b + 1] = m_ringAt[b] + m_blocks[b].ring.size();
    }
    const std::size_t vertexCount = m_ringAt[vertexBlocks];
    if (m_vertices.size() < vertexCount) {
        m_vertices.resize(vertexCount + vertexCount / 4);
    }
    core::parallelFor(&jobs, vertexBlocks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t b = first; b < last; ++b) {
            const Block& block = m_blocks[b];
            std::copy(block.moved.begin(), block.moved.end(), m_vertices.begin() + static_cast<std::ptrdiff_t>(m_movedAt[b]));
            std::copy(block.ring.begin(), block.ring.end(), m_vertices.begin() + static_cast<std::ptrdiff_t>(m_ringAt[b]));
        }
    });
    g_steps.ms[3] += step.elapsedMs();
    step.reset();

    // 5. Every stamped vertex is in the list once: reset them for the next call.
    const u32* const vertices = m_vertices.data();
    core::parallelFor(&jobs, vertexCount, 8192, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            stamp[vertices[i]] = kNoStamp;
        }
    });
    g_steps.ms[4] += step.elapsedMs();
    ++g_steps.calls;
    m_triangleCount = triangleCount;
    m_vertexCount = vertexCount;
}

void EditRegion::collectSerial(const Mesh& mesh, std::span<const VertexId> movedVertices)
{
    const std::size_t triangleWords = (static_cast<std::size_t>(mesh.triangleCount()) + 63) / 64;
    const std::size_t vertexWords = (static_cast<std::size_t>(mesh.vertexCount()) + 63) / 64;
    if (m_triangleBits.size() != triangleWords || m_vertexBits.size() != vertexWords) {
        m_triangleBits.assign(triangleWords, 0);
        m_vertexBits.assign(vertexWords, 0);
    }
    u64* const triangleBits = m_triangleBits.data();
    u64* const vertexBits = m_vertexBits.data();
    std::size_t triangleCount = 0;
    std::size_t vertexCount = 0;

    // One walk over the moved vertices' incident-triangle rows.
    ensureRoom(m_vertices, 0, movedVertices.size());
    for (const VertexId vertex : movedVertices) {
        claimInto(vertexBits, vertex, m_vertices.data(), vertexCount);
        const AdjacencyRow row = mesh.incidentTriangles(vertex);
        ensureRoom(m_triangles, triangleCount, row.size());
        u32* const out = m_triangles.data();
        row.forEach([&](TriangleId triangle) { claimInto(triangleBits, triangle, out, triangleCount); });
    }

    // The one-ring: the other corners of those triangles.
    const PagedArrayView<const Mesh::Triangle> corners = mesh.triangles();
    // Each triangle has a moved corner, already claimed: at most two new.
    ensureRoom(m_vertices, vertexCount, triangleCount * 2);
    u32* const vertexOut = m_vertices.data();
    for (std::size_t i = 0; i < triangleCount; ++i) {
        for (const VertexId corner : corners[m_triangles[i]]) {
            // Most corners are moved vertices, already claimed: here the
            // branch predicts well and beats the branchless claim.
            u64& word = vertexBits[corner >> 6];
            const u64 bit = u64{1} << (corner & 63u);
            if ((word & bit) == 0) {
                word |= bit;
                vertexOut[vertexCount++] = corner;
            }
        }
    }

    // Leave the bitsets all clear for the next call.
    for (std::size_t i = 0; i < triangleCount; ++i) {
        triangleBits[m_triangles[i] >> 6] = 0;
    }
    for (std::size_t i = 0; i < vertexCount; ++i) {
        vertexBits[m_vertices[i] >> 6] = 0;
    }
    m_triangleCount = triangleCount;
    m_vertexCount = vertexCount;
}

std::size_t EditRegion::memoryBytes() const noexcept
{
    std::size_t bytes = (m_triangleBits.capacity() + m_vertexBits.capacity()) * sizeof(u64) +
                        m_triangles.capacity() * sizeof(TriangleId) + m_vertices.capacity() * sizeof(VertexId) +
                        m_blocks.capacity() * sizeof(Block) +
                        (m_offsets.capacity() + m_movedAt.capacity() + m_ringAt.capacity()) * sizeof(std::size_t);
    for (const Block& block : m_blocks) {
        bytes += (block.moved.capacity() + block.triangles.capacity() + block.ring.capacity() + block.candidates.capacity()) * sizeof(u32);
    }
    return bytes;
}

} // namespace se::geo
