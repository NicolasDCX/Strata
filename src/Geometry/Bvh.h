#pragma once

#include "Core/JobSystem.h"
#include "Core/Types.h"
#include "Geometry/Aabb.h"
#include "Geometry/Mesh.h"
#include "Geometry/Ray.h"

#include <glm/vec3.hpp>

#include <limits>
#include <optional>
#include <utility>
#include <span>
#include <vector>

namespace se::geo {

/// Bounding volume hierarchy over a contiguous range of triangle ids of one mesh.
///
/// Used in two ways: over the whole mesh (a single tree), and as the local tree
/// of one spatial chunk inside ChunkedBvh, which is what the engine uses.
///
/// The three concerns live in separate translation units:
///   - BvhBuilder.cpp    build(), optimize(): top-down median split
///   - BvhRefit.cpp      refit(): recompute boxes after vertices moved
///   - BvhTraversal.cpp  raycast() / querySphere(): read-only queries
///
/// The BVH stores no pointer to the mesh; the same mesh must be passed to every
/// call. After vertex positions change, queries are only correct once a refit
/// (or build) has run.
///
/// Updating after an edit costs time proportional to the edited region:
///   - the incremental refit recomputes only the leaves that contain a moved
///     triangle, then level by level the ancestors whose child changed;
///   - optimize() re-partitions only the blocks of the tree that were refitted
///     since the previous optimize()/build().
///
/// The shape of the tree (which node has which children, how many triangles
/// each subtree holds) depends only on the triangle count, because every split
/// is an exact median. That is what makes rebuilding a subtree in place
/// possible: it needs exactly the node slots it already occupies.
class Bvh {
public:
    struct Node {
        Aabb bounds;
        /// Inner node: index of the left child (the right child is `leftOrFirst + 1`).
        /// Leaf: index of the first entry in the triangle index array.
        u32 leftOrFirst = 0;
        /// 0 for inner nodes, number of triangles for leaves.
        u32 triangleCount = 0;

        [[nodiscard]] bool isLeaf() const noexcept { return triangleCount > 0; }
    };

    static constexpr u32 kMaxLeafTriangles = 4;
    static_assert(kMaxLeafTriangles >= 2, "nodeCountPair assumes n + 1 <= 2 * kMaxLeafTriangles splits into two leaves");
    /// optimize() rebuilds touched subtrees holding at most this many triangles.
    static constexpr u32 kOptimizeBlockTriangles = 2048;

    /// Number of nodes build() makes for `triangleCount` triangles. The build
    /// splits every node at the exact median, so a subtree's shape depends only
    /// on its triangle count: f(n) = 1 for n <= kMaxLeafTriangles, otherwise
    /// 1 + f(floor(n / 2)) + f(ceil(n / 2)). build() reserves exactly this many.
    [[nodiscard]] static constexpr u32 nodeCountFor(u32 triangleCount) noexcept
    {
        return triangleCount == 0 ? 0u : nodeCountPair(triangleCount).first;
    }

    /// Rebuilds the hierarchy over all triangles of the mesh.
    void build(const Mesh& mesh) { build(mesh, 0, mesh.triangleCount()); }
    /// Rebuilds the hierarchy over triangles [firstTriangle, firstTriangle + triangleCount).
    void build(const Mesh& mesh, TriangleId firstTriangle, u32 triangleCount);

    /// Recomputes every node's bounds. O(node count). Does not allocate.
    void refit(const Mesh& mesh) noexcept;

    /// Recomputes the bounds affected by moving `movedVertices`: the leaves
    /// holding their triangles, then level by level every ancestor whose child
    /// changed, each exactly once. All incident triangles must be in this tree's
    /// range. Allocation free once its scratch lists have grown. With `jobs`,
    /// the nodes of a level are processed in parallel; the result is identical.
    void refit(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem* jobs = nullptr);

    /// The same refit in three steps, for callers that route triangles to several
    /// trees (ChunkedBvh): begin, mark every triangle that has a moved vertex,
    /// finish. finishRefit() returns true if the root box changed.
    void beginRefit();
    /// Inline: called once per triangle around every moved vertex.
    void markTriangle(TriangleId triangle)
    {
        // Queue the triangle's leaf once, at its depth.
        const u32 leaf = m_triangleLeaf[triangle - m_firstTriangle];
        if (m_leafStamp[leaf] == m_refitGeneration) {
            return; // already queued through another vertex or triangle
        }
        m_leafStamp[leaf] = m_refitGeneration;
        m_levels[m_depth[leaf]].push_back(leaf);

        // Remember, up to the root, that this part of the tree was refitted.
        // If a node is already marked, so are all its ancestors.
        for (u32 node = leaf; node != kInvalidIndex && m_touched[node] == 0; node = m_parents[node]) {
            m_touched[node] = 1;
        }
    }
    ///
    /// With `allowDense` and no `jobs`, a refit that queued at least half of
    /// the leaves recomputes the whole tree instead, in node order: every leaf,
    /// then every inner node from the deepest index up. Boxes are minima and
    /// maxima, so the result is bit-identical; the sweep reads the nodes, the
    /// leaf contents and the vertices in memory order instead of in queue order,
    /// and skips the level lists. Nothing else changes (stamps, optimize state).
    bool finishRefit(const Mesh& mesh, core::JobSystem* jobs = nullptr, bool allowDense = false);
    /// Queued leaves needed for the dense sweep: numerator / denominator of all leaves.
    static constexpr u32 kDenseRefitNumerator = 1;
    static constexpr u32 kDenseRefitDenominator = 2;

    /// Where the last finishRefit() spent its time (diagnostics). Levels are
    /// timed as a whole: a level of leaves counts as leaf time, a level of inner
    /// nodes as propagation (a mixed level, only in a partial tree, is split by
    /// node count). `queueMs` is queueing the parents of changed nodes.
    struct RefitStats {
        float leafMs = 0.0f;
        float innerMs = 0.0f;
        float queueMs = 0.0f;
        u32 leaves = 0;          ///< leaves recomputed
        u32 leafTriangles = 0;   ///< triangles in them
        u32 unchangedLeaves = 0; ///< recomputed leaves whose box did not change
        u32 inner = 0;           ///< inner nodes recomputed
        u32 unchangedInner = 0;  ///< recomputed inner nodes whose box did not change
        bool dense = false;      ///< the whole tree was swept in node order
    };
    [[nodiscard]] const RefitStats& lastRefitStats() const noexcept { return m_lastRefitStats; }

    /// Calls f(leafNode) for every leaf queued by the current refit, from its
    /// markTriangle() calls until the next beginRefit() (finishRefit() keeps
    /// them). Inner nodes the refit queued are skipped.
    template <class F>
    void forEachQueuedLeaf(F&& f) const
    {
        for (const std::vector<u32>& level : m_levels) {
            for (const u32 node : level) {
                if (m_nodes[node].isLeaf()) {
                    f(node);
                }
            }
        }
    }
    /// Number of leaves forEachQueuedLeaf() visits.
    [[nodiscard]] u32 queuedLeafCount() const noexcept;
    /// Leaves of the tree (a full binary tree).
    [[nodiscard]] u32 leafCount() const noexcept { return static_cast<u32>((m_nodes.size() + 1) / 2); }

    /// Re-partitions blocks touched by incremental refits since the last build().
    /// Stops once at least `maxTriangles` triangles were re-partitioned, so the
    /// work can be spread over several calls; the tree is valid for queries and
    /// refits between calls. Returns the number of triangles re-partitioned.
    u32 optimize(const Mesh& mesh, u32 maxTriangles = std::numeric_limits<u32>::max());

    /// True while optimize() still has touched blocks to re-partition.
    [[nodiscard]] bool needsOptimize() const noexcept { return !m_touched.empty() && m_touched[0] != 0; }

    /// Closest hit along the ray, or nullopt.
    [[nodiscard]] std::optional<RayHit>
    raycast(const Mesh& mesh, const Ray& ray,
            float maxDistance = std::numeric_limits<float>::max()) const noexcept;

    /// Appends to `outTriangles` every triangle stored in a leaf whose box touches
    /// the sphere. Conservative: callers must still test individual vertices.
    void querySphere(const glm::vec3& center, float radius, std::vector<TriangleId>& outTriangles) const;

    [[nodiscard]] bool empty() const noexcept { return m_nodes.empty(); }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return m_nodes.size(); }
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return m_nodes; }
    [[nodiscard]] const std::vector<TriangleId>& triangleIndices() const noexcept { return m_triangles; }
    [[nodiscard]] TriangleId firstTriangle() const noexcept { return m_firstTriangle; }
    [[nodiscard]] u32 triangleCount() const noexcept { return static_cast<u32>(m_triangles.size()); }

    /// Parent of a node (kInvalidIndex for the root).
    [[nodiscard]] u32 parentOf(u32 node) const noexcept { return m_parents[node]; }
    /// Leaf node that stores a triangle of this tree's range.
    [[nodiscard]] u32 leafOf(TriangleId triangle) const noexcept { return m_triangleLeaf[triangle - m_firstTriangle]; }

    /// Bounds of the whole hierarchy (empty box when there are no triangles).
    [[nodiscard]] Aabb bounds() const noexcept { return m_nodes.empty() ? Aabb{} : m_nodes[0].bounds; }

    [[nodiscard]] std::size_t memoryBytes() const noexcept;

private:
    /// (f(n), f(n + 1)) for n >= 1, with f as in nodeCountFor. At every depth the
    /// subtree sizes are two adjacent values, so this takes O(log n) steps.
    [[nodiscard]] static constexpr std::pair<u32, u32> nodeCountPair(u32 n) noexcept
    {
        if (n < kMaxLeafTriangles) {
            return {1u, 1u};
        }
        if (n == kMaxLeafTriangles) {
            return {1u, 3u}; // n + 1 splits into two leaves
        }
        // n = 2m: children (m, m), n + 1 = 2m + 1: (m, m + 1).
        // n = 2m + 1: children (m, m + 1), n + 1 = 2m + 2: (m + 1, m + 1).
        const auto [a, b] = nodeCountPair(n / 2);
        return n % 2 == 0 ? std::pair<u32, u32>{1u + 2u * a, 1u + a + b} : std::pair<u32, u32>{1u + a + b, 1u + 2u * b};
    }

    [[nodiscard]] Aabb leafBounds(const Node& node, PagedArrayView<const glm::vec3> positions,
                                  PagedArrayView<const Mesh::Triangle> triangles) const noexcept;

    // Children are always allocated after their parent, and the descendants of
    // a node occupy one contiguous index range starting at its left child.
    std::vector<Node> m_nodes;
    std::vector<TriangleId> m_triangles; // leaf contents: triangle ids
    std::vector<u32> m_parents;          // per node
    std::vector<u32> m_triangleLeaf;     // per triangle of the range: the leaf that stores it
    std::vector<u8> m_depth;             // per node: distance from the root
    TriangleId m_firstTriangle = 0;

    // Incremental refit state.
    std::vector<u32> m_leafStamp; // per node: refit generation in which it was queued
    std::vector<u8> m_touched;    // per node: subtree holds a refitted block not yet optimized
    std::vector<std::vector<u32>> m_levels; // scratch: nodes to recompute, by depth
    std::vector<u8> m_changed;              // scratch: per node of the current level, did its box change
    u32 m_refitGeneration = 0;
    RefitStats m_lastRefitStats;
};

} // namespace se::geo
