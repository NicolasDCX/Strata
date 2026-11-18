#include "Geometry/EditRegion.h"

#include "Core/Assert.h"

#include <algorithm>
#include <atomic>

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
/// kNoStamp between calls. One array per thread, sized for the largest mesh
/// walked on it.
constexpr u32 kNoStamp = 0xFFFFFFFFu;
constexpr u32 kMovedFlag = 0x80000000u;
thread_local std::vector<u32> t_stamps;
std::atomic<std::size_t> g_stampBytes{0};

std::vector<u32>& stampsFor(u32 vertexCount)
{
    if (t_stamps.size() < vertexCount) {
        const std::size_t before = t_stamps.capacity();
        t_stamps.assign(vertexCount, kNoStamp);
        g_stampBytes.fetch_add((t_stamps.capacity() - before) * sizeof(u32), std::memory_order_relaxed);
    }
    return t_stamps;
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
    // - a one-ring vertex first appears at its first corner 3p + k in the
    //   triangle list.
    // One stamp per vertex holds kMovedFlag | i for moved vertices, the first
    // corner for one-ring vertices, kNoStamp otherwise. Fixed blocks keep
    // their finds in order and are joined by prefix sums.
    std::vector<u32>& stamps = stampsFor(mesh.vertexCount());
    u32* const stamp = stamps.data();
    const std::size_t movedCount = movedVertices.size();
    SE_ASSERT(movedCount < kMovedFlag);

    // 1. First index of every moved vertex.
    core::parallelFor(&jobs, movedCount, 4096, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            atomicMin(stamp[movedVertices[i]], kMovedFlag | static_cast<u32>(i));
        }
    });

    // 2. Per block of moved vertices: the vertices at their first index, and
    //    the triangles whose earliest moved corner they are, in row order.
    const std::size_t vertexBlocks = (movedCount + kParallelVertexBlock - 1) / kParallelVertexBlock;
    if (m_blocks.size() < vertexBlocks) {
        m_blocks.resize(vertexBlocks);
    }
    const PagedArrayView<const Mesh::Triangle> corners = mesh.triangles();
    core::parallelFor(&jobs, vertexBlocks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t b = first; b < last; ++b) {
            Block& block = m_blocks[b];
            block.moved.clear();
            block.triangles.clear();
            const std::size_t end = std::min(movedCount, (b + 1) * kParallelVertexBlock);
            for (std::size_t i = b * kParallelVertexBlock; i < end; ++i) {
                const VertexId vertex = movedVertices[i];
                const u32 self = kMovedFlag | static_cast<u32>(i);
                if (stamp[vertex] != self) {
                    continue; // a repeat: its row adds nothing the first did not
                }
                block.moved.push_back(vertex);
                mesh.incidentTriangles(vertex).forEach([&](TriangleId triangle) {
                    // Stamps of moved vertices are kMovedFlag | index, smaller
                    // than kNoStamp: the earliest moved corner has the minimum.
                    const Mesh::Triangle& c = corners[triangle];
                    const u32 owner = std::min({stamp[c[0]], stamp[c[1]], stamp[c[2]]});
                    if (owner == self) {
                        block.triangles.push_back(triangle);
                    }
                });
            }
        }
    });

    // Join: moved vertices first, then the triangles.
    m_offsets.resize(vertexBlocks + 1);
    m_offsets[0] = 0;
    for (std::size_t b = 0; b < vertexBlocks; ++b) {
        m_offsets[b + 1] = m_offsets[b] + m_blocks[b].triangles.size();
    }
    const std::size_t triangleCount = m_offsets[vertexBlocks];
    std::size_t movedUnique = 0;
    for (std::size_t b = 0; b < vertexBlocks; ++b) {
        movedUnique += m_blocks[b].moved.size();
    }
    if (m_triangles.size() < triangleCount) {
        m_triangles.resize(triangleCount + triangleCount / 4);
    }
    // At most two new corners per triangle.
    if (m_vertices.size() < movedUnique + triangleCount * 2) {
        m_vertices.resize(movedUnique + triangleCount * 2 + 1024);
    }
    std::vector<std::size_t> movedAt(vertexBlocks + 1);
    movedAt[0] = 0;
    for (std::size_t b = 0; b < vertexBlocks; ++b) {
        movedAt[b + 1] = movedAt[b] + m_blocks[b].moved.size();
    }
    core::parallelFor(&jobs, vertexBlocks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t b = first; b < last; ++b) {
            const Block& block = m_blocks[b];
            std::copy(block.triangles.begin(), block.triangles.end(), m_triangles.begin() + static_cast<std::ptrdiff_t>(m_offsets[b]));
            std::copy(block.moved.begin(), block.moved.end(), m_vertices.begin() + static_cast<std::ptrdiff_t>(movedAt[b]));
        }
    });

    // 3. First corner (3p + k) of every vertex that did not move.
    SE_ASSERT(triangleCount * 3 < kMovedFlag);
    const u32* const triangles = m_triangles.data();
    core::parallelFor(&jobs, triangleCount, kParallelTriangleBlock, [&](std::size_t begin, std::size_t end) {
        for (std::size_t p = begin; p < end; ++p) {
            const Mesh::Triangle& c = corners[triangles[p]];
            for (u32 k = 0; k < 3; ++k) {
                u32& slot = stamp[c[k]];
                // Moved stamps are stable now; reading one racing with another
                // corner's atomicMin on a one-ring vertex is through atomic_ref.
                const u32 current = std::atomic_ref<u32>(slot).load(std::memory_order_relaxed);
                if (current != kNoStamp && (current & kMovedFlag) != 0) {
                    continue; // moved
                }
                atomicMin(slot, static_cast<u32>(3 * p + k));
            }
        }
    });

    // 4. Per block of triangles: the one-ring vertices first seen there.
    const std::size_t triangleBlocks = (triangleCount + kParallelTriangleBlock - 1) / kParallelTriangleBlock;
    if (m_blocks.size() < triangleBlocks) {
        m_blocks.resize(triangleBlocks);
    }
    core::parallelFor(&jobs, triangleBlocks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t b = first; b < last; ++b) {
            Block& block = m_blocks[b];
            block.ring.clear();
            const std::size_t end = std::min(triangleCount, (b + 1) * kParallelTriangleBlock);
            for (std::size_t p = b * kParallelTriangleBlock; p < end; ++p) {
                const Mesh::Triangle& c = corners[triangles[p]];
                for (u32 k = 0; k < 3; ++k) {
                    if (stamp[c[k]] == static_cast<u32>(3 * p + k)) {
                        block.ring.push_back(c[k]);
                    }
                }
            }
        }
    });
    m_offsets.resize(triangleBlocks + 1);
    m_offsets[0] = movedUnique;
    for (std::size_t b = 0; b < triangleBlocks; ++b) {
        m_offsets[b + 1] = m_offsets[b] + m_blocks[b].ring.size();
    }
    const std::size_t vertexCount = m_offsets[triangleBlocks];
    core::parallelFor(&jobs, triangleBlocks, 1, [&](std::size_t first, std::size_t last) {
        for (std::size_t b = first; b < last; ++b) {
            const Block& block = m_blocks[b];
            std::copy(block.ring.begin(), block.ring.end(), m_vertices.begin() + static_cast<std::ptrdiff_t>(m_offsets[b]));
        }
    });

    // 5. Every stamped vertex is in the list once: reset them for the next call.
    const u32* const vertices = m_vertices.data();
    core::parallelFor(&jobs, vertexCount, 8192, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            stamp[vertices[i]] = kNoStamp;
        }
    });
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
                        m_blocks.capacity() * sizeof(Block) + m_offsets.capacity() * sizeof(std::size_t);
    for (const Block& block : m_blocks) {
        bytes += (block.moved.capacity() + block.triangles.capacity() + block.ring.capacity()) * sizeof(u32);
    }
    return bytes;
}

} // namespace se::geo
