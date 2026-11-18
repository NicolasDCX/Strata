#include "Geometry/Bvh.h"

#include "Core/Assert.h"

#include <algorithm>
#include <numeric>

namespace se::geo {

namespace {

struct BuildContext {
    const Mesh& mesh;
    PagedArrayView<const glm::vec3> positions;
    std::vector<Bvh::Node>& nodes;
    std::vector<TriangleId>& triangles;
    std::vector<u32>& parents;
    std::vector<u32>& triangleLeaf;
    std::vector<u8>& depth;
    TriangleId firstTriangle;
    /// Index the next pair of children is written to. A full build starts it
    /// after the root and grows the arrays; a subtree rebuild starts it at the
    /// subtree's existing first child and reuses the existing slots.
    u32 nextNode = 1;

    /// Three times the centroid: enough for ordering, and needs no storage.
    [[nodiscard]] glm::vec3 centroidSum(TriangleId triangle) const noexcept
    {
        const Mesh::Triangle& corners = mesh.triangle(triangle);
        return positions[corners[0]] + positions[corners[1]] + positions[corners[2]];
    }
};

/// Writes the subtree for triangles[first, first + count) rooted at `nodeIndex`.
void buildNode(BuildContext& ctx, u32 nodeIndex, u32 first, u32 count, u8 depth)
{
    ctx.depth[nodeIndex] = depth;

    Aabb bounds;
    Aabb centroidBounds;
    for (u32 i = first; i < first + count; ++i) {
        const Mesh::Triangle& corners = ctx.mesh.triangle(ctx.triangles[i]);
        const glm::vec3& p0 = ctx.positions[corners[0]];
        const glm::vec3& p1 = ctx.positions[corners[1]];
        const glm::vec3& p2 = ctx.positions[corners[2]];
        bounds.expand(p0);
        bounds.expand(p1);
        bounds.expand(p2);
        centroidBounds.expand(p0 + p1 + p2);
    }
    ctx.nodes[nodeIndex].bounds = bounds;

    if (count <= Bvh::kMaxLeafTriangles) {
        ctx.nodes[nodeIndex].leftOrFirst = first;
        ctx.nodes[nodeIndex].triangleCount = count;
        for (u32 i = first; i < first + count; ++i) {
            ctx.triangleLeaf[ctx.triangles[i] - ctx.firstTriangle] = nodeIndex;
        }
        return;
    }

    // Exact median split along the widest centroid axis. Because the split
    // position depends only on `count`, so does the shape of the whole subtree.
    const glm::vec3 extent = centroidBounds.extent();
    int axis = 0;
    if (extent.y > extent[axis]) {
        axis = 1;
    }
    if (extent.z > extent[axis]) {
        axis = 2;
    }

    const u32 half = count / 2;
    const auto begin = ctx.triangles.begin() + first;
    std::nth_element(begin, begin + half, begin + count, [&ctx, axis](TriangleId a, TriangleId b) {
        return ctx.centroidSum(a)[axis] < ctx.centroidSum(b)[axis];
    });

    const u32 left = ctx.nextNode;
    ctx.nextNode += 2;
    if (ctx.nodes.size() < ctx.nextNode) {
        ctx.nodes.resize(ctx.nextNode);
        ctx.parents.resize(ctx.nextNode);
        ctx.depth.resize(ctx.nextNode);
    }
    ctx.parents[left] = nodeIndex;
    ctx.parents[left + 1] = nodeIndex;
    ctx.nodes[nodeIndex].leftOrFirst = left;
    ctx.nodes[nodeIndex].triangleCount = 0;

    buildNode(ctx, left, first, half, static_cast<u8>(depth + 1));
    buildNode(ctx, left + 1, first + half, count - half, static_cast<u8>(depth + 1));
}

} // namespace

void Bvh::build(const Mesh& mesh, TriangleId firstTriangle, u32 triangleCount)
{
    m_firstTriangle = firstTriangle;
    m_nodes.clear();
    m_parents.clear();
    m_depth.clear();
    m_levels.clear();
    m_triangles.resize(triangleCount);
    m_triangleLeaf.assign(triangleCount, kInvalidIndex);
    if (triangleCount == 0) {
        m_leafStamp.clear();
        m_touched.clear();
        return;
    }
    std::iota(m_triangles.begin(), m_triangles.end(), firstTriangle);

    // The node count is known in advance (nodeCountFor): one exact
    // allocation per array, no growth during the recursion and no shrinking
    // afterwards. With thousands of chunks built in parallel, the 2n reserve
    // and shrink_to_fit it replaces went through the shared heap and made the
    // build superlinear (README, "Scaling checkpoint: 84M triangles").
    const u32 nodeTotal = nodeCountFor(triangleCount);
    m_nodes.clear();
    m_parents.clear();
    m_depth.clear();
    if (m_nodes.capacity() != nodeTotal) {
        std::vector<Node>().swap(m_nodes);
        std::vector<u32>().swap(m_parents);
        std::vector<u8>().swap(m_depth);
        m_nodes.reserve(nodeTotal);
        m_parents.reserve(nodeTotal);
        m_depth.reserve(nodeTotal);
    }
    m_nodes.emplace_back();
    m_parents.push_back(kInvalidIndex);
    m_depth.push_back(0);

    BuildContext ctx{mesh, mesh.positions(), m_nodes, m_triangles, m_parents, m_triangleLeaf, m_depth, firstTriangle, 1};
    buildNode(ctx, 0, 0, triangleCount, 0);
    SE_ASSERT(m_nodes.size() == nodeTotal);
    m_levels.resize(static_cast<std::size_t>(*std::max_element(m_depth.begin(), m_depth.end())) + 1);

    m_leafStamp.assign(m_nodes.size(), 0);
    m_touched.assign(m_nodes.size(), 0);
    m_refitGeneration = 0;
}

u32 Bvh::optimize(const Mesh& mesh, u32 maxTriangles)
{
    if (m_nodes.empty()) {
        return 0;
    }
    BuildContext ctx{mesh, mesh.positions(), m_nodes, m_triangles, m_parents, m_triangleLeaf, m_depth, m_firstTriangle, 0};
    const u32 triangleTotal = triangleCount();

    u32 rebuiltTriangles = 0;
    while (m_touched[0] != 0 && rebuiltTriangles < maxTriangles) {
        // Follow touched nodes down from the root to one block. The triangle
        // range of a child follows from the median split, so it is tracked on
        // the way instead of being stored per node.
        u32 node = 0;
        u32 first = 0;
        u32 count = triangleTotal;
        while (count > kOptimizeBlockTriangles && !m_nodes[node].isLeaf()) {
            const u32 left = m_nodes[node].leftOrFirst;
            const u32 half = count / 2;
            if (m_touched[left] != 0) {
                node = left;
                count = half;
            } else if (m_touched[left + 1] != 0) {
                node = left + 1;
                first += half;
                count -= half;
            } else {
                break; // marked, but nothing below is: only the mark is stale
            }
        }

        if (count <= kOptimizeBlockTriangles && !m_nodes[node].isLeaf()) {
            // Same triangles, same node slots, new partition. The ancestors'
            // boxes are unions of the same triangles, so they stay valid.
            const u32 left = m_nodes[node].leftOrFirst;
            ctx.nextNode = left;
            buildNode(ctx, node, first, count, m_depth[node]);
            std::fill(m_touched.begin() + left, m_touched.begin() + ctx.nextNode, u8{0});
            rebuiltTriangles += count;
        }
        // (A leaf has nothing to re-partition; its box is already exact.)

        // Clear the mark of this node, then of every ancestor that has no
        // touched child left.
        m_touched[node] = 0;
        for (u32 parent = m_parents[node]; parent != kInvalidIndex; parent = m_parents[parent]) {
            const u32 left = m_nodes[parent].leftOrFirst;
            if (m_touched[left] != 0 || m_touched[left + 1] != 0) {
                break;
            }
            m_touched[parent] = 0;
        }
    }
    return rebuiltTriangles;
}

std::size_t Bvh::memoryBytes() const noexcept
{
    std::size_t bytes = m_nodes.capacity() * sizeof(Node) +
                        (m_triangles.capacity() + m_parents.capacity() + m_triangleLeaf.capacity() + m_leafStamp.capacity()) *
                            sizeof(u32) +
                        m_touched.capacity() + m_depth.capacity() + m_changed.capacity() +
                        m_levels.capacity() * sizeof(std::vector<u32>);
    for (const std::vector<u32>& level : m_levels) {
        bytes += level.capacity() * sizeof(u32);
    }
    return bytes;
}

} // namespace se::geo
