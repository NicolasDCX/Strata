#include "Geometry/ChunkedBvh.h"

#include "Core/Assert.h"
#include "Core/Timer.h"
#include "Geometry/Intersect.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <numeric>
#include <utility>

namespace se::geo {

namespace {

// The top-level tree is balanced, so depth <= 32. 64 is ample.
constexpr std::size_t kStackSize = 64;

/// Chunks refitted per parallel task. A local refit takes a few microseconds to
/// a few tens of microseconds; waking a helper thread costs about as much, so
/// only spread the work when there are many chunks.
constexpr std::size_t kRefitChunkGrain = 8;
/// Parallel leaf marking: triangles per block of the grouping passes (fixed,
/// so the grouping does not depend on the thread count), and the smallest list
/// worth sharing out.
constexpr std::size_t kMarkBlockTriangles = 8192;
constexpr std::size_t kParallelMarkMinTriangles = 32768;

/// Index of `chunk` among `runs`, appended (count 0) when absent. Searched
/// from the back: in a spatially ordered list the chunk is usually the last one.
std::size_t findRun(auto& runs, u32 chunk)
{
    for (std::size_t i = runs.size(); i-- > 0;) {
        if (runs[i].chunk == chunk) {
            return i;
        }
    }
    runs.push_back({.chunk = chunk});
    return runs.size() - 1;
}

/// Bytes of every thread's face-normal scratch (ChunkedBvh::exactCone).
std::atomic<std::size_t> g_coneScratchBytes{0};

} // namespace

std::size_t ChunkedBvh::coneScratchBytes() noexcept
{
    return g_coneScratchBytes.load(std::memory_order_relaxed);
}

NormalCone ChunkedBvh::exactCone(const Mesh& mesh, u32 first, u32 count, NormalConeTimes& times) const
{
    if (!m_reuseConeNormals) {
        return computeNormalConeTimed(mesh, first, count, nullptr, times);
    }
    // One scratch per thread that computes cones (the sculpt thread, the
    // helpers, the thread that builds), kept for the thread's lifetime.
    thread_local std::vector<glm::vec4> faces;
    if (faces.size() < count) {
        const std::size_t before = faces.capacity();
        faces.resize(count);
        g_coneScratchBytes.fetch_add((faces.capacity() - before) * sizeof(glm::vec4), std::memory_order_relaxed);
    }
    return computeNormalConeTimed(mesh, first, count, faces.data(), times);
}

// --- Build -----------------------------------------------------------------------

void ChunkedBvh::build(const Mesh& mesh, u32 chunkTriangleShift, core::JobSystem* jobs, bool normalCones)
{
    SE_ASSERT(chunkTriangleShift >= 2 && chunkTriangleShift < 31);
    m_shift = chunkTriangleShift;
    m_normalCones = normalCones;
    const u32 triangleTotal = mesh.triangleCount();
    const u32 capacity = 1u << m_shift;
    const u32 chunkTotal = (triangleTotal + capacity - 1) >> m_shift;

    m_chunks.clear();
    m_chunks.resize(chunkTotal);
    m_optimizeQueue.clear();
    m_topStale = false;
    m_staleCones.clear();
    m_rebuildChunks.clear();
    m_rebuildCones.clear();

    // Local trees do not share anything: build them in parallel.
    core::parallelFor(jobs, chunkTotal, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            const u32 first = static_cast<u32>(index) << m_shift;
            m_chunks[index].tree.build(mesh, first, std::min(capacity, triangleTotal - first));
            updateCone(mesh, static_cast<u32>(index));
        }
    });
    buildTop();
}

void ChunkedBvh::updateCone(const Mesh& mesh, u32 chunkIndex, NormalConeTimes* times)
{
    if (!m_normalCones) {
        return;
    }
    const u32 first = chunkIndex << m_shift;
    const u32 count = std::min(1u << m_shift, mesh.triangleCount() - first);
    NormalConeTimes unused;
    m_chunks[chunkIndex].cone = exactCone(mesh, first, count, times != nullptr ? *times : unused);
    m_chunks[chunkIndex].coneStale = false; // left in m_staleCones, skipped there
}

void ChunkedBvh::buildTop()
{
    m_top.clear();
    m_topParents.clear();
    const u32 chunkTotal = chunkCount();
    if (chunkTotal == 0) {
        return;
    }
    m_topOrder.resize(chunkTotal);
    std::iota(m_topOrder.begin(), m_topOrder.end(), 0u);
    m_top.reserve(static_cast<std::size_t>(chunkTotal) * 2);
    m_topParents.reserve(static_cast<std::size_t>(chunkTotal) * 2);
    m_top.emplace_back();
    m_topParents.push_back(kInvalidIndex);
    buildTopNode(0, 0, chunkTotal);
}

void ChunkedBvh::buildTopNode(u32 nodeIndex, u32 first, u32 count)
{
    Aabb bounds;
    Aabb centerBounds;
    for (u32 i = first; i < first + count; ++i) {
        const Aabb chunkBounds = m_chunks[m_topOrder[i]].tree.bounds();
        bounds.expand(chunkBounds);
        centerBounds.expand(chunkBounds.center());
    }
    m_top[nodeIndex].bounds = bounds;

    if (count == 1) {
        const u32 chunkIndex = m_topOrder[first];
        m_top[nodeIndex].chunk = chunkIndex;
        m_chunks[chunkIndex].topLeaf = nodeIndex;
        return;
    }

    const glm::vec3 extent = centerBounds.extent();
    int axis = 0;
    if (extent.y > extent[axis]) {
        axis = 1;
    }
    if (extent.z > extent[axis]) {
        axis = 2;
    }
    const u32 half = count / 2;
    const auto begin = m_topOrder.begin() + first;
    std::nth_element(begin, begin + half, begin + count, [this, axis](u32 a, u32 b) {
        return m_chunks[a].tree.bounds().center()[axis] < m_chunks[b].tree.bounds().center()[axis];
    });

    const u32 left = static_cast<u32>(m_top.size());
    m_top.emplace_back();
    m_top.emplace_back();
    m_topParents.push_back(nodeIndex);
    m_topParents.push_back(nodeIndex);
    m_top[nodeIndex].left = left;
    m_top[nodeIndex].chunk = kInvalidIndex;

    buildTopNode(left, first, half);
    buildTopNode(left + 1, first + half, count - half);
}

// --- Updates ---------------------------------------------------------------------

void ChunkedBvh::updateTopFor(u32 chunkIndex)
{
    u32 node = m_chunks[chunkIndex].topLeaf;
    m_top[node].bounds = m_chunks[chunkIndex].tree.bounds();
    // Every inner box equals the union of its children, so the walk can stop at
    // the first ancestor that does not change.
    for (node = m_topParents[node]; node != kInvalidIndex; node = m_topParents[node]) {
        Aabb merged = m_top[m_top[node].left].bounds;
        merged.expand(m_top[m_top[node].left + 1].bounds);
        if (merged == m_top[node].bounds) {
            break;
        }
        m_top[node].bounds = merged;
    }
}

void ChunkedBvh::queueOptimize(u32 chunkIndex)
{
    Chunk& chunk = m_chunks[chunkIndex];
    if (!chunk.optimizeQueued) {
        chunk.optimizeQueued = true;
        m_optimizeQueue.push_back(chunkIndex);
    }
}

void ChunkedBvh::refit(const Mesh& mesh)
{
    m_lastConeTriangles = 0;
    for (u32 index = 0; index < chunkCount(); ++index) {
        m_chunks[index].tree.refit(mesh);
        updateCone(mesh, index);
        queueOptimize(index);
    }
    m_staleCones.clear();
    if (m_normalCones) {
        m_lastConeTriangles = mesh.triangleCount();
    }
    // All boxes may have changed: recompute the top level bottom-up. Children
    // have larger indices than their parent.
    for (std::size_t i = m_top.size(); i-- > 0;) {
        TopNode& node = m_top[i];
        if (node.isLeaf()) {
            node.bounds = m_chunks[node.chunk].tree.bounds();
        } else {
            node.bounds = m_top[node.left].bounds;
            node.bounds.expand(m_top[node.left + 1].bounds);
        }
    }
    m_topStale = !m_chunks.empty();
}

void ChunkedBvh::markForRefit(TriangleId triangle)
{
    const u32 chunkIndex = triangle >> m_shift;
    Chunk& chunk = m_chunks[chunkIndex];
    if (!chunk.inRefit) {
        chunk.inRefit = true;
        chunk.tree.beginRefit();
        // The cone is left alone: marking changes nothing a reader sees, and
        // step 2 recomputes the cone together with the boxes.
        m_refitChunks.push_back(chunkIndex);
    }
    chunk.tree.markTriangle(triangle);
}

void ChunkedBvh::refit(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem* jobs)
{
    if (m_chunks.empty() || movedVertices.empty()) {
        return;
    }

    // 1. Route every triangle of a moved vertex to its chunk's local refit.
    //    A triangle belongs to exactly one chunk, whatever chunks its
    //    neighbours are in, so an edit across a chunk border needs no special
    //    handling: it simply marks triangles in several chunks.
    m_refitChunks.clear();
    for (const VertexId vertex : movedVertices) {
        mesh.incidentTriangles(vertex).forEach([this](TriangleId triangle) { markForRefit(triangle); });
    }
    finishPartialRefit(mesh, jobs);
}

void ChunkedBvh::refitTriangles(const Mesh& mesh, std::span<const TriangleId> triangles, core::JobSystem* jobs)
{
    if (m_chunks.empty() || triangles.empty()) {
        return;
    }
    markTriangles(triangles);
    finishPartialRefit(mesh, jobs);
}

void ChunkedBvh::markTriangles(std::span<const TriangleId> triangles, core::JobSystem* jobs)
{
    const core::Stopwatch timer;
    m_refitChunks.clear();
    if (!m_chunks.empty()) {
        if (jobs != nullptr && jobs->workerCount() > 0 && triangles.size() >= kParallelMarkMinTriangles) {
            markTrianglesParallel(triangles, *jobs);
        } else {
            for (const TriangleId triangle : triangles) {
                markForRefit(triangle);
            }
        }
    }
    m_lastRefitTimings.markMs = timer.elapsedMs();
}

void ChunkedBvh::markTrianglesParallel(std::span<const TriangleId> triangles, core::JobSystem& jobs)
{
    const std::size_t count = triangles.size();
    const std::size_t blockCount = (count + kMarkBlockTriangles - 1) / kMarkBlockTriangles;
    if (m_markBlocks.size() < blockCount) {
        m_markBlocks.resize(blockCount);
    }
    if (m_markCursor.size() < m_chunks.size()) {
        m_markCursor.assign(m_chunks.size(), 0u);
    }

    // 1. Per block: the chunks it reaches, in first-occurrence order, and how
    //    many of its triangles each gets.
    core::parallelFor(&jobs, blockCount, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t b = begin; b < end; ++b) {
            std::vector<MarkRun>& runs = m_markBlocks[b];
            runs.clear();
            u32 last = kInvalidIndex;
            std::size_t at = 0;
            const std::size_t stop = std::min(count, (b + 1) * kMarkBlockTriangles);
            for (std::size_t t = b * kMarkBlockTriangles; t < stop; ++t) {
                const u32 chunk = triangles[t] >> m_shift;
                if (chunk != last) {
                    at = findRun(runs, chunk);
                    last = chunk;
                }
                ++runs[at].count;
            }
        }
    });

    // 2. Serial, over the few runs: the chunks in the order the serial loop
    //    first reaches them (blocks in order, runs in order), then each chunk's
    //    range of buckets and each run's place in it.
    for (std::size_t b = 0; b < blockCount; ++b) {
        for (const MarkRun& run : m_markBlocks[b]) {
            Chunk& chunk = m_chunks[run.chunk];
            if (!chunk.inRefit) {
                chunk.inRefit = true;
                m_refitChunks.push_back(run.chunk);
            }
            m_markCursor[run.chunk] += run.count;
        }
    }
    const std::size_t chunkCount = m_refitChunks.size();
    m_markStart.resize(chunkCount + 1);
    u32 start = 0;
    for (std::size_t i = 0; i < chunkCount; ++i) {
        const u32 chunk = m_refitChunks[i];
        m_markStart[i] = start;
        start += m_markCursor[chunk];
        m_markCursor[chunk] = m_markStart[i];
    }
    m_markStart[chunkCount] = start;
    for (std::size_t b = 0; b < blockCount; ++b) {
        for (MarkRun& run : m_markBlocks[b]) {
            run.offset = m_markCursor[run.chunk];
            m_markCursor[run.chunk] += run.count;
        }
    }
    for (const u32 chunk : m_refitChunks) {
        m_markCursor[chunk] = 0;
    }

    // 3. Group the triangles by chunk, keeping their order (a stable counting sort).
    m_markBuckets.resize(count);
    core::parallelFor(&jobs, blockCount, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t b = begin; b < end; ++b) {
            std::vector<MarkRun>& runs = m_markBlocks[b];
            u32 last = kInvalidIndex;
            MarkRun* run = nullptr;
            const std::size_t stop = std::min(count, (b + 1) * kMarkBlockTriangles);
            for (std::size_t t = b * kMarkBlockTriangles; t < stop; ++t) {
                const u32 chunk = triangles[t] >> m_shift;
                if (chunk != last) {
                    run = &runs[findRun(runs, chunk)];
                    last = chunk;
                }
                m_markBuckets[run->offset++] = triangles[t];
            }
        }
    });

    // 4. Mark, one chunk per task: each chunk's tree is its own, and sees its
    //    triangles in the serial order.
    core::parallelFor(&jobs, chunkCount, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            Bvh& tree = m_chunks[m_refitChunks[i]].tree;
            tree.beginRefit();
            for (u32 k = m_markStart[i]; k < m_markStart[i + 1]; ++k) {
                tree.markTriangle(m_markBuckets[k]);
            }
        }
    });
}

void ChunkedBvh::finishMarkedRefit(const Mesh& mesh, core::JobSystem* jobs)
{
    finishPartialRefit(mesh, jobs);
}

void ChunkedBvh::finishPartialRefit(const Mesh& mesh, core::JobSystem* jobs)
{
    // 2. Finish the local refits and recompute the normal cones of the same
    //    chunks. Chunks are independent of each other. A cone costs a pass
    //    over the chunk's triangles, much more than a local refit, so with
    //    cones the work is spread over threads more eagerly.
    core::Stopwatch timer;
    const std::size_t count = m_refitChunks.size();
    m_rootChanged.resize(count);
    m_refitBoxMs.resize(count);
    m_refitConeMs.resize(count);
    m_coneUpdate.assign(count, static_cast<u8>(ConeUpdate::None));
    m_coneVisited.assign(count, 0u);
    m_refitConeTimes.assign(count, NormalConeTimes{});
    core::parallelFor(jobs, count, m_normalCones ? 1 : kRefitChunkGrain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const core::Stopwatch stage;
            m_rootChanged[i] = m_chunks[m_refitChunks[i]].tree.finishRefit(mesh, nullptr, m_denseRefit) ? u8{1} : u8{0};
            const double boxMs = stage.elapsedMs();
            updateChunkCone(mesh, i);
            m_refitBoxMs[i] = static_cast<float>(boxMs);
            m_refitConeMs[i] = static_cast<float>(stage.elapsedMs() - boxMs);
        }
    });
    m_lastRefitTimings.boxCpuMs = 0.0;
    m_lastRefitTimings.coneCpuMs = 0.0;
    m_lastRefitTimings.leafCpuMs = 0.0;
    m_lastRefitTimings.innerCpuMs = 0.0;
    m_lastRefitTimings.queueCpuMs = 0.0;
    m_lastRefitTimings.leaves = m_lastRefitTimings.leafTriangles = m_lastRefitTimings.unchangedLeaves = 0;
    m_lastRefitTimings.inner = m_lastRefitTimings.unchangedInner = 0;
    m_lastRefitTimings.boxChunkMaxMs = 0.0;
    m_lastRefitTimings.denseChunks = 0;
    for (std::size_t i = 0; i < count; ++i) {
        m_lastRefitTimings.boxCpuMs += m_refitBoxMs[i];
        m_lastRefitTimings.coneCpuMs += m_refitConeMs[i];
        m_lastRefitTimings.boxChunkMaxMs = std::max<double>(m_lastRefitTimings.boxChunkMaxMs, m_refitBoxMs[i]);
        const Bvh::RefitStats& stats = m_chunks[m_refitChunks[i]].tree.lastRefitStats();
        m_lastRefitTimings.leafCpuMs += stats.leafMs;
        m_lastRefitTimings.innerCpuMs += stats.innerMs;
        m_lastRefitTimings.queueCpuMs += stats.queueMs;
        m_lastRefitTimings.leaves += stats.leaves;
        m_lastRefitTimings.leafTriangles += stats.leafTriangles;
        m_lastRefitTimings.unchangedLeaves += stats.unchangedLeaves;
        m_lastRefitTimings.inner += stats.inner;
        m_lastRefitTimings.unchangedInner += stats.unchangedInner;
        m_lastRefitTimings.denseChunks += stats.dense ? 1u : 0u;
    }
    m_lastRefitTimings.boxChunkMeanMs = count > 0 ? m_lastRefitTimings.boxCpuMs / static_cast<double>(count) : 0.0;
    m_lastRefitTimings.chunks = static_cast<u32>(count);
    m_lastRefitTimings.conesExact = m_lastRefitTimings.conesWidened = m_lastRefitTimings.conesDisabled = 0;
    m_lastRefitTimings.widenTriangles = 0;
    m_lastRefitTimings.conePass1CpuMs = m_lastRefitTimings.conePass2CpuMs = 0.0;
    m_lastRefitTimings.conePass2Triangles = 0;
    m_lastConeTriangles = 0;
    for (std::size_t i = 0; i < count; ++i) {
        m_lastRefitTimings.conePass1CpuMs += m_refitConeTimes[i].firstPassMs;
        m_lastRefitTimings.conePass2CpuMs += m_refitConeTimes[i].secondPassMs;
        m_lastRefitTimings.conePass2Triangles += m_refitConeTimes[i].secondPassTriangles;
        m_lastConeTriangles += m_coneVisited[i];
        const auto update = static_cast<ConeUpdate>(m_coneUpdate[i]);
        if (update == ConeUpdate::Exact) {
            ++m_lastRefitTimings.conesExact;
        } else if (update == ConeUpdate::Widened || update == ConeUpdate::Disabled) {
            ++(update == ConeUpdate::Widened ? m_lastRefitTimings.conesWidened : m_lastRefitTimings.conesDisabled);
            m_lastRefitTimings.widenTriangles += m_coneVisited[i];
            Chunk& chunk = m_chunks[m_refitChunks[i]];
            if (!chunk.coneStale) {
                chunk.coneStale = true;
                m_staleCones.push_back(m_refitChunks[i]);
            }
        }
    }

    m_lastRefitTimings.localMs = timer.elapsedMs();
    timer.reset();

    // 3. Tell the top level about chunks whose overall box changed.
    for (std::size_t i = 0; i < count; ++i) {
        const u32 chunkIndex = m_refitChunks[i];
        m_chunks[chunkIndex].inRefit = false;
        queueOptimize(chunkIndex);
        if (m_rootChanged[i] != 0) {
            updateTopFor(chunkIndex);
            m_topStale = true;
        }
    }
    m_lastRefitTimings.topMs = timer.elapsedMs();
}

void ChunkedBvh::updateChunkCone(const Mesh& mesh, std::size_t refitIndex)
{
    if (!m_normalCones) {
        return;
    }
    const u32 chunkIndex = m_refitChunks[refitIndex];
    Chunk& chunk = m_chunks[chunkIndex];
    const Bvh& tree = chunk.tree;
    // Exact cones, or most of the chunk changed: recompute (two passes over the chunk).
    if (!m_incrementalCones ||
        static_cast<u64>(tree.queuedLeafCount()) * 100u > static_cast<u64>(tree.leafCount()) * m_coneFallbackPercent) {
        updateCone(mesh, chunkIndex, &m_refitConeTimes[refitIndex]);
        m_coneUpdate[refitIndex] = static_cast<u8>(ConeUpdate::Exact);
        m_coneVisited[refitIndex] = tree.triangleCount();
        return;
    }
    // Widen around the old axis by the normals of the queued leaves' triangles.
    // Every triangle with a moved vertex is in one of them; the normals of the
    // others did not change, so the old cone still holds them.
    u32 visited = 0;
    const std::vector<TriangleId>& ids = tree.triangleIndices();
    const std::vector<Bvh::Node>& nodes = tree.nodes();
    chunk.cone = widenNormalCone(chunk.cone, mesh, [&](auto&& visit) {
        tree.forEachQueuedLeaf([&](u32 leaf) {
            const Bvh::Node& node = nodes[leaf];
            for (u32 k = 0; k < node.triangleCount; ++k) {
                visit(ids[node.leftOrFirst + k]);
            }
            visited += node.triangleCount;
        });
    });
    m_coneUpdate[refitIndex] = static_cast<u8>(chunk.cone.usable() ? ConeUpdate::Widened : ConeUpdate::Disabled);
    m_coneVisited[refitIndex] = visited;
}

u32 ChunkedBvh::staleConeCount() const noexcept
{
    u32 count = 0;
    for (const u32 chunk : m_staleCones) {
        count += m_chunks[chunk].coneStale ? 1u : 0u;
    }
    return count;
}

u32 ChunkedBvh::computeStaleCones(const Mesh& mesh, u32 maxChunks, core::JobSystem* jobs)
{
    m_rebuildChunks.clear();
    while (!m_staleCones.empty() && m_rebuildChunks.size() < maxChunks) {
        const u32 chunk = m_staleCones.back();
        m_staleCones.pop_back();
        // A chunk recomputed exactly and widened again since is listed twice.
        if (m_chunks[chunk].coneStale &&
            std::find(m_rebuildChunks.begin(), m_rebuildChunks.end(), chunk) == m_rebuildChunks.end()) {
            m_rebuildChunks.push_back(chunk);
        }
    }
    m_rebuildCones.resize(m_rebuildChunks.size());
    core::parallelFor(jobs, m_rebuildChunks.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const u32 first = m_rebuildChunks[i] << m_shift;
            NormalConeTimes times;
            m_rebuildCones[i] = exactCone(mesh, first, std::min(1u << m_shift, mesh.triangleCount() - first), times);
        }
    });
    return static_cast<u32>(m_rebuildChunks.size());
}

u32 ChunkedBvh::publishStaleCones() noexcept
{
    for (std::size_t i = 0; i < m_rebuildChunks.size(); ++i) {
        Chunk& chunk = m_chunks[m_rebuildChunks[i]];
        chunk.cone = m_rebuildCones[i];
        chunk.coneStale = false;
    }
    const auto published = static_cast<u32>(m_rebuildChunks.size());
    m_lastPublishedCones = published;
    m_rebuildChunks.clear();
    return published;
}

ChunkedBvh::ConeLoss ChunkedBvh::measureConeLoss(const Mesh& mesh, std::span<const glm::vec3> eyes) const
{
    ConeLoss loss;
    loss.eyes = static_cast<u32>(eyes.size());
    std::vector<NormalCone> exact(m_chunks.size());
    for (u32 index = 0; index < chunkCount(); ++index) {
        exact[index] = m_chunks[index].cone;
        if (m_chunks[index].coneStale) {
            const u32 first = index << m_shift;
            exact[index] = computeNormalCone(mesh, first, std::min(1u << m_shift, mesh.triangleCount() - first));
            ++loss.staleChunks;
            loss.disabledChunks += m_chunks[index].cone.usable() ? 0u : 1u;
        }
    }
    for (const glm::vec3& eye : eyes) {
        for (u32 index = 0; index < chunkCount(); ++index) {
            const Aabb box = m_chunks[index].tree.bounds();
            const u64 triangles = m_chunks[index].tree.triangleCount();
            loss.drawnTriangles += coneFacesAway(m_chunks[index].cone, box, eye) ? 0u : triangles;
            loss.drawnTrianglesExact += coneFacesAway(exact[index], box, eye) ? 0u : triangles;
        }
    }
    return loss;
}

u32 ChunkedBvh::optimize(const Mesh& mesh, u32 maxTriangles)
{
    u32 rebuilt = 0;
    while (!m_optimizeQueue.empty() && rebuilt < maxTriangles) {
        const u32 chunkIndex = m_optimizeQueue.back();
        Chunk& chunk = m_chunks[chunkIndex];
        rebuilt += chunk.tree.optimize(mesh, maxTriangles - rebuilt);
        if (!chunk.tree.needsOptimize()) {
            chunk.optimizeQueued = false;
            m_optimizeQueue.pop_back();
        }
    }
    if (m_optimizeQueue.empty() && m_topStale) {
        // Chunks moved: re-partition the top level so that sibling boxes
        // overlap as little as possible again. Cheap: one leaf per chunk.
        buildTop();
        m_topStale = false;
    }
    return rebuilt;
}

// --- Queries ---------------------------------------------------------------------

std::optional<RayHit> ChunkedBvh::raycast(const Mesh& mesh, const Ray& ray, float maxDistance) const noexcept
{
    if (m_top.empty()) {
        return std::nullopt;
    }
    const glm::vec3 invDirection = 1.0f / ray.direction;
    float closest = maxDistance;
    std::optional<RayHit> best;

    std::array<u32, kStackSize> stack{};
    std::size_t stackSize = 0;
    float rootNear = 0.0f;
    if (intersectRayAabb(ray.origin, invDirection, m_top[0].bounds, closest, rootNear)) {
        stack[stackSize++] = 0;
    }

    while (stackSize > 0) {
        const TopNode& node = m_top[stack[--stackSize]];
        if (node.isLeaf()) {
            // The box was tested when the node was pushed, but `closest` may have
            // shrunk since; the local traversal re-tests its root against it.
            if (std::optional<RayHit> hit = m_chunks[node.chunk].tree.raycast(mesh, ray, closest)) {
                closest = hit->distance;
                best = hit;
            }
            continue;
        }
        // Visit the nearer child first so the farther one can often be culled.
        u32 nearChild = node.left;
        u32 farChild = node.left + 1;
        float nearDistance = 0.0f;
        float farDistance = 0.0f;
        const bool hitNear = intersectRayAabb(ray.origin, invDirection, m_top[nearChild].bounds, closest, nearDistance);
        const bool hitFar = intersectRayAabb(ray.origin, invDirection, m_top[farChild].bounds, closest, farDistance);
        if (hitNear && hitFar && farDistance < nearDistance) {
            std::swap(nearChild, farChild);
        }
        if (hitFar) {
            stack[stackSize++] = farChild;
        }
        if (hitNear) {
            stack[stackSize++] = nearChild;
        }
    }
    return best;
}

void ChunkedBvh::querySphere(const glm::vec3& center, float radius, std::vector<TriangleId>& outTriangles) const
{
    if (m_top.empty()) {
        return;
    }
    const float radiusSquared = radius * radius;
    std::array<u32, kStackSize> stack{};
    std::size_t stackSize = 0;
    stack[stackSize++] = 0;

    while (stackSize > 0) {
        const TopNode& node = m_top[stack[--stackSize]];
        if (node.bounds.distanceSquaredTo(center) > radiusSquared) {
            continue;
        }
        if (node.isLeaf()) {
            m_chunks[node.chunk].tree.querySphere(center, radius, outTriangles);
        } else {
            stack[stackSize++] = node.left;
            stack[stackSize++] = node.left + 1;
        }
    }
}

void ChunkedBvh::querySphereChunks(const glm::vec3& center, float radius, std::vector<u32>& outChunks) const
{
    if (m_top.empty()) {
        return;
    }
    // The same traversal as querySphere(), recording leaves instead of searching them.
    const float radiusSquared = radius * radius;
    std::array<u32, kStackSize> stack{};
    std::size_t stackSize = 0;
    stack[stackSize++] = 0;
    while (stackSize > 0) {
        const TopNode& node = m_top[stack[--stackSize]];
        if (node.bounds.distanceSquaredTo(center) > radiusSquared) {
            continue;
        }
        if (node.isLeaf()) {
            outChunks.push_back(node.chunk);
        } else {
            stack[stackSize++] = node.left;
            stack[stackSize++] = node.left + 1;
        }
    }
}

// --- Diagnostics -------------------------------------------------------------------

std::size_t ChunkedBvh::nodeCount() const noexcept
{
    std::size_t count = m_top.size();
    for (const Chunk& chunk : m_chunks) {
        count += chunk.tree.nodeCount();
    }
    return count;
}

std::size_t ChunkedBvh::markScratchBytes() const noexcept
{
    std::size_t bytes = m_markBlocks.capacity() * sizeof(std::vector<MarkRun>) +
                        (m_markCursor.capacity() + m_markStart.capacity() + m_markBuckets.capacity()) * sizeof(u32);
    for (const std::vector<MarkRun>& runs : m_markBlocks) {
        bytes += runs.capacity() * sizeof(MarkRun);
    }
    return bytes;
}

std::size_t ChunkedBvh::memoryBytes() const noexcept
{
    std::size_t bytes = m_chunks.capacity() * sizeof(Chunk) + m_top.capacity() * sizeof(TopNode) +
                        (m_topParents.capacity() + m_topOrder.capacity() + m_optimizeQueue.capacity() +
                         m_refitChunks.capacity()) *
                            sizeof(u32) +
                        m_rootChanged.capacity() + (m_refitBoxMs.capacity() + m_refitConeMs.capacity()) * sizeof(float) +
                        m_coneUpdate.capacity() +
                        (m_coneVisited.capacity() + m_staleCones.capacity() + m_rebuildChunks.capacity()) * sizeof(u32) +
                        m_rebuildCones.capacity() * sizeof(NormalCone) + m_refitConeTimes.capacity() * sizeof(NormalConeTimes);
    for (const Chunk& chunk : m_chunks) {
        bytes += chunk.tree.memoryBytes();
    }
    bytes += markScratchBytes();
    return bytes;
}

bool ChunkedBvh::sameBounds(const ChunkedBvh& other, bool compareCones) const noexcept
{
    if (chunkCount() != other.chunkCount() || m_shift != other.m_shift || !(bounds() == other.bounds())) {
        return false;
    }
    for (u32 index = 0; index < chunkCount(); ++index) {
        if (compareCones && !(m_chunks[index].cone == other.m_chunks[index].cone)) {
            return false;
        }
        const std::vector<Bvh::Node>& mine = m_chunks[index].tree.nodes();
        const std::vector<Bvh::Node>& theirs = other.m_chunks[index].tree.nodes();
        if (mine.size() != theirs.size()) {
            return false;
        }
        for (std::size_t node = 0; node < mine.size(); ++node) {
            if (!(mine[node].bounds == theirs[node].bounds)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace se::geo
