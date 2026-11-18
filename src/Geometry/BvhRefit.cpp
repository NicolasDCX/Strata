#include "Geometry/Bvh.h"

#include "Core/Timer.h"

#include <algorithm>

namespace se::geo {

Aabb Bvh::leafBounds(const Node& node, PagedArrayView<const glm::vec3> positions,
                     PagedArrayView<const Mesh::Triangle> triangles) const noexcept
{
    Aabb bounds;
    for (u32 k = 0; k < node.triangleCount; ++k) {
        const Mesh::Triangle& corners = triangles[m_triangles[node.leftOrFirst + k]];
        bounds.expand(positions[corners[0]]);
        bounds.expand(positions[corners[1]]);
        bounds.expand(positions[corners[2]]);
    }
    return bounds;
}

void Bvh::refit(const Mesh& mesh) noexcept
{
    const PagedArrayView<const glm::vec3> positions = mesh.positions();
    const PagedArrayView<const Mesh::Triangle> triangles = mesh.triangles();
    // Children have larger indices than their parent, so a reverse sweep
    // recomputes every child before the parent that depends on it.
    for (std::size_t i = m_nodes.size(); i-- > 0;) {
        Node& node = m_nodes[i];
        if (node.isLeaf()) {
            node.bounds = leafBounds(node, positions, triangles);
        } else {
            Aabb bounds = m_nodes[node.leftOrFirst].bounds;
            bounds.expand(m_nodes[node.leftOrFirst + 1].bounds);
            node.bounds = bounds;
        }
    }
    // Everything may have moved: let optimize() look at the whole tree.
    std::fill(m_touched.begin(), m_touched.end(), u8{1});
}

void Bvh::refit(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem* jobs)
{
    if (m_nodes.empty() || movedVertices.empty()) {
        return;
    }
    beginRefit();
    for (const VertexId vertex : movedVertices) {
        mesh.incidentTriangles(vertex).forEach([this](TriangleId triangle) { markTriangle(triangle); });
    }
    finishRefit(mesh, jobs);
}

void Bvh::beginRefit()
{
    if (++m_refitGeneration == 0) { // wrapped: old stamps could alias the new generation
        std::fill(m_leafStamp.begin(), m_leafStamp.end(), 0u);
        m_refitGeneration = 1;
    }
    for (std::vector<u32>& level : m_levels) {
        level.clear();
    }
}

u32 Bvh::queuedLeafCount() const noexcept
{
    u32 count = 0;
    forEachQueuedLeaf([&count](u32) { ++count; });
    return count;
}

bool Bvh::finishRefit(const Mesh& mesh, core::JobSystem* jobs, bool allowDense)
{
    if (m_nodes.empty()) {
        return false;
    }
    const Aabb rootBefore = m_nodes[0].bounds;
    // Page tables in locals: not reloaded through the mesh for every leaf.
    const PagedArrayView<const glm::vec3> positions = mesh.positions();
    const PagedArrayView<const Mesh::Triangle> triangles = mesh.triangles();
    RefitStats stats;

    if (allowDense && jobs == nullptr) {
        // Only leaves are queued before the first level is processed.
        std::size_t queued = 0;
        for (const std::vector<u32>& level : m_levels) {
            queued += level.size();
        }
        const std::size_t leafCount = (m_nodes.size() + 1) / 2; // full binary tree
        if (queued > 0 && queued * kDenseRefitDenominator >= leafCount * kDenseRefitNumerator) {
            // Leaves first, in index order, then inner nodes from the last
            // index down: children have larger indices than their parent.
            // Unqueued leaves and inner nodes get the box they already have
            // (min/max of the same values), so the result equals the
            // incremental refit's bit for bit.
            core::Stopwatch timer;
            u32 unchangedLeaves = 0;
            for (Node& node : m_nodes) {
                if (node.isLeaf()) {
                    const Aabb updated = leafBounds(node, positions, triangles);
                    unchangedLeaves += updated == node.bounds ? 1u : 0u;
                    node.bounds = updated;
                    ++stats.leaves;
                    stats.leafTriangles += node.triangleCount;
                }
            }
            stats.leafMs = static_cast<float>(timer.elapsedMs());
            timer.reset();
            u32 unchangedInner = 0;
            for (std::size_t i = m_nodes.size(); i-- > 0;) {
                Node& node = m_nodes[i];
                if (!node.isLeaf()) {
                    Aabb updated = m_nodes[node.leftOrFirst].bounds;
                    updated.expand(m_nodes[node.leftOrFirst + 1].bounds);
                    unchangedInner += updated == node.bounds ? 1u : 0u;
                    node.bounds = updated;
                    ++stats.inner;
                }
            }
            stats.innerMs = static_cast<float>(timer.elapsedMs());
            stats.unchangedLeaves = unchangedLeaves;
            stats.unchangedInner = unchangedInner;
            stats.dense = true;
            m_lastRefitStats = stats;
            return !(m_nodes[0].bounds == rootBefore);
        }
    }

    // Deepest level first. All children of a level's nodes are deeper, hence
    // already final, so the nodes of one level are independent of each other
    // and can be recomputed in parallel. A parent is queued only if one of its
    // children actually changed, so the work stops where the edit stops
    // mattering, and every node is recomputed at most once.
    for (std::size_t depth = m_levels.size(); depth-- > 0;) {
        const std::vector<u32>& level = m_levels[depth];
        const std::size_t count = level.size();
        if (count == 0) {
            continue;
        }
        m_changed.resize(count);
        core::Stopwatch levelTimer;
        core::parallelFor(jobs, count, 2048, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                Node& node = m_nodes[level[i]];
                Aabb updated;
                if (node.isLeaf()) {
                    updated = leafBounds(node, positions, triangles);
                } else {
                    updated = m_nodes[node.leftOrFirst].bounds;
                    updated.expand(m_nodes[node.leftOrFirst + 1].bounds);
                }
                m_changed[i] = updated == node.bounds ? u8{0} : u8{1};
                node.bounds = updated;
            }
        });
        {
            const float levelMs = static_cast<float>(levelTimer.elapsedMs());
            u32 leaves = 0, leafTriangles = 0, unchangedLeaves = 0, unchangedInner = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const Node& node = m_nodes[level[i]];
                if (node.isLeaf()) {
                    ++leaves;
                    leafTriangles += node.triangleCount;
                    unchangedLeaves += m_changed[i] == 0 ? 1u : 0u;
                } else {
                    unchangedInner += m_changed[i] == 0 ? 1u : 0u;
                }
            }
            const float leafShare = static_cast<float>(leaves) / static_cast<float>(count);
            stats.leafMs += levelMs * leafShare;
            stats.innerMs += levelMs * (1.0f - leafShare);
            stats.leaves += leaves;
            stats.leafTriangles += leafTriangles;
            stats.unchangedLeaves += unchangedLeaves;
            stats.inner += static_cast<u32>(count) - leaves;
            stats.unchangedInner += unchangedInner;
        }
        if (depth == 0) {
            break; // the root has no parent
        }
        const core::Stopwatch queueTimer;
        std::vector<u32>& parents = m_levels[depth - 1];
        for (std::size_t i = 0; i < count; ++i) {
            if (m_changed[i] == 0) {
                continue;
            }
            const u32 parent = m_parents[level[i]];
            if (m_leafStamp[parent] != m_refitGeneration) {
                m_leafStamp[parent] = m_refitGeneration;
                parents.push_back(parent);
            }
        }
        stats.queueMs += static_cast<float>(queueTimer.elapsedMs());
    }
    m_lastRefitStats = stats;
    return !(m_nodes[0].bounds == rootBefore);
}

} // namespace se::geo
