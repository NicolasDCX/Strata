#pragma once

#include "Core/JobSystem.h"
#include "Core/Types.h"
#include "Geometry/Aabb.h"
#include "Geometry/Bvh.h"
#include "Geometry/Mesh.h"
#include "Geometry/NormalCone.h"
#include "Geometry/Ray.h"

#include <glm/vec3.hpp>

#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace se::geo {

/// Two-level acceleration structure: the mesh's triangles are grouped into
/// spatial chunks, each chunk has its own local BVH, and a small top-level BVH
/// is built over the chunks' bounds.
///
/// What a chunk is
/// - A chunk is a run of 2^chunkTriangleShift consecutive triangle ids. Because
///   triangles are created in spatial (Morton) order, such a run is a compact
///   patch of surface.
/// - Chunk membership never changes: chunkOf(triangle) is a shift. After the
///   surface is deformed a chunk is still a connected patch; it has merely
///   moved, and its bounds follow.
/// - Chunks partition triangles, not vertices. A vertex on the border between
///   two chunks is one vertex, referenced by triangles of both; nothing is
///   duplicated. Chunks have no fixed relation to geometry pages or GPU pages.
///
/// Why two levels
/// - An edit touches a few chunks. Their local trees are refitted independently
///   (and in parallel when there are many); the top level only hears about
///   chunks whose overall box changed.
/// - Tidying up after an edit means rebuilding the local trees of the touched
///   chunks, a chunk at a time, plus the small top-level tree. Unlike a single
///   big tree, no part of the hierarchy is frozen in its initial shape.
/// - Local trees are independent, so the initial build runs in parallel.
/// - A chunk is the natural unit for later work: culling, level of detail,
///   loading and evicting.
///
/// Normal cones (optional, see build())
/// - Each chunk can also keep a cone holding the face normal of every one of
///   its triangles, for back-face culling of whole chunks. A cone is
///   recomputed by every refit that touches the chunk, in the same parallel
///   pass as its local tree, so a chunk's cone and bounds always describe the
///   same state of the mesh: whoever reads bounds() under the document lock
///   reads the matching chunkCone() too.
/// - With incremental cones (setIncrementalCones) a refit may instead widen a
///   chunk's cone, or turn its culling off, in the same pass. The cone still
///   holds every normal of the chunk, only less tightly, until a background
///   rebuild (computeStaleCones/publishStaleCones) makes it exact again.
///
/// The same mesh must be passed to every call. Queries are valid after any
/// completed refit or optimize call, including between optimize steps.
class ChunkedBvh {
public:
    static constexpr u32 kDefaultChunkTriangleShift = 12; // 4,096 triangles per chunk

    struct TopNode {
        Aabb bounds;
        /// Inner node: index of the left child (the right child is `left + 1`).
        u32 left = 0;
        /// Leaf: the chunk it holds. kInvalidIndex for inner nodes.
        u32 chunk = kInvalidIndex;

        [[nodiscard]] bool isLeaf() const noexcept { return chunk != kInvalidIndex; }
    };

    /// Builds chunks, their local trees (in parallel when `jobs` is given) and
    /// the top level. With `normalCones`, every chunk also keeps a normal cone
    /// that every later refit keeps current.
    void build(const Mesh& mesh, u32 chunkTriangleShift = kDefaultChunkTriangleShift, core::JobSystem* jobs = nullptr,
               bool normalCones = false);

    /// Recomputes every box of every chunk and the top level. O(node count).
    void refit(const Mesh& mesh);

    /// Recomputes the boxes affected by moving `movedVertices`. Each affected
    /// chunk is refitted incrementally; chunks are processed in parallel when
    /// there are enough of them. The result does not depend on `jobs`.
    void refit(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem* jobs = nullptr);
    /// The same refit from the triangles around the moved vertices, each once
    /// (EditRegion::triangles()). Bit-identical to refit(mesh, movedVertices)
    /// when the list is in EditRegion's order; avoids a second walk over the
    /// adjacency when the caller already has the list.
    void refitTriangles(const Mesh& mesh, std::span<const TriangleId> triangles, core::JobSystem* jobs = nullptr);

    /// refitTriangles() in two halves, for a single writer that publishes its
    /// edits under a lock. markTriangles() queues the triangles' leaves and
    /// chunks; it changes nothing that queries, bounds(), chunk() or
    /// chunkCone() read, so other threads may keep reading the tree meanwhile.
    /// finishMarkedRefit() then recomputes the queued boxes, the cones of the
    /// same chunks and the top level from the mesh's current positions. It must
    /// follow before the next refit or optimize call. Together they equal
    /// refitTriangles() bit for bit.
    ///
    /// With `jobs` (and helper threads, and at least 32K triangles) the marking
    /// runs per chunk on the helpers: the triangles are first grouped by chunk,
    /// keeping their order, and the chunks keep the order in which the list
    /// first reaches them. Every chunk then sees the same marks in the same
    /// order as in the serial loop, and the refit queue has the same order.
    void markTriangles(std::span<const TriangleId> triangles, core::JobSystem* jobs = nullptr);
    void finishMarkedRefit(const Mesh& mesh, core::JobSystem* jobs = nullptr);

    /// Re-partitions the local trees of chunks refitted since the last
    /// optimize()/build(), then rebuilds the top level once no chunk is pending.
    /// Stops after about `maxTriangles` triangles so the work can be spread over
    /// several calls. Returns the number of triangles re-partitioned.
    u32 optimize(const Mesh& mesh, u32 maxTriangles = std::numeric_limits<u32>::max());

    /// True while optimize() still has work.
    [[nodiscard]] bool needsOptimize() const noexcept { return !m_optimizeQueue.empty() || m_topStale; }

    /// Closest hit along the ray, or nullopt.
    [[nodiscard]] std::optional<RayHit>
    raycast(const Mesh& mesh, const Ray& ray,
            float maxDistance = std::numeric_limits<float>::max()) const noexcept;

    /// Appends to `outTriangles` every triangle stored in a leaf whose box touches
    /// the sphere. Conservative: callers must still test individual vertices.
    void querySphere(const glm::vec3& center, float radius, std::vector<TriangleId>& outTriangles) const;
    /// Appends the chunks whose local tree querySphere() would search, in the
    /// order it searches them. Querying each with chunk(c).querySphere() and
    /// concatenating the results in this order gives querySphere()'s list.
    void querySphereChunks(const glm::vec3& center, float radius, std::vector<u32>& outChunks) const;

    [[nodiscard]] bool empty() const noexcept { return m_chunks.empty(); }
    /// Bounds of the whole mesh (empty box when there are no triangles).
    [[nodiscard]] Aabb bounds() const noexcept { return m_top.empty() ? Aabb{} : m_top[0].bounds; }

    // --- Chunks ------------------------------------------------------------------
    [[nodiscard]] u32 chunkCount() const noexcept { return static_cast<u32>(m_chunks.size()); }
    [[nodiscard]] u32 chunkTriangleShift() const noexcept { return m_shift; }
    [[nodiscard]] u32 chunkOf(TriangleId triangle) const noexcept { return triangle >> m_shift; }
    /// Local tree of a chunk; its bounds() are the chunk's bounds.
    [[nodiscard]] const Bvh& chunk(u32 index) const noexcept { return m_chunks[index].tree; }
    [[nodiscard]] const std::vector<TopNode>& topNodes() const noexcept { return m_top; }
    /// True when chunks keep normal cones (see build()).
    [[nodiscard]] bool hasNormalCones() const noexcept { return m_normalCones; }
    /// Normal cone of a chunk, matching its current bounds. Unusable (it can
    /// reject nothing) when cones are off or the chunk's normals spread too widely.
    [[nodiscard]] const NormalCone& chunkCone(u32 index) const noexcept { return m_chunks[index].cone; }
    /// Triangles visited to recompute cones by the last refit call (diagnostics).
    [[nodiscard]] u32 lastConeTriangles() const noexcept { return m_lastConeTriangles; }

    /// Where the last partial refit spent its time (diagnostics, milliseconds).
    struct RefitTimings {
        double markMs = 0.0;  ///< queue each triangle's leaf and its chunk (markTriangles)
        double localMs = 0.0; ///< parallel over chunks: local refits and normal cones
        double topMs = 0.0;   ///< serial: chunk boxes into the top level
        /// localMs split by stage, summed over the threads (CPU time): the
        /// chunks' box refits and their normal-cone recomputations. Without
        /// helpers they add up to about localMs; with helpers to more.
        double boxCpuMs = 0.0;
        double coneCpuMs = 0.0;
        u32 chunks = 0; ///< chunks refitted
        /// boxCpuMs split (Bvh::RefitStats summed over the chunks): leaf boxes,
        /// propagation through inner nodes, queueing parents.
        double leafCpuMs = 0.0;
        double innerCpuMs = 0.0;
        double queueCpuMs = 0.0;
        u64 leaves = 0, leafTriangles = 0, unchangedLeaves = 0, inner = 0, unchangedInner = 0;
        /// Box refit of the slowest chunk, and the mean over chunks (load balance).
        double boxChunkMaxMs = 0.0;
        double boxChunkMeanMs = 0.0;
        u32 denseChunks = 0; ///< chunks refitted with the dense sweep
        /// Cones of the refitted chunks (see setIncrementalCones): recomputed
        /// exactly, widened, or turned off until their rebuild; and the
        /// changed triangles the widened ones visited.
        u32 conesExact = 0;
        u32 conesWidened = 0;
        u32 conesDisabled = 0;
        u64 widenTriangles = 0;
        /// Exact cones (coneCpuMs part), CPU time summed over the threads:
        /// the first pass (face normals, axis) and the second (half-angle),
        /// with the triangles the second pass visited before stopping.
        double conePass1CpuMs = 0.0;
        double conePass2CpuMs = 0.0;
        u64 conePass2Triangles = 0;
    };

    /// Exact cones reuse the first pass's face normals in the second, from a
    /// per-thread scratch of 16 bytes per chunk triangle (computeNormalConeTimed).
    /// Same cones bit for bit.
    void setReuseConeNormals(bool enabled) noexcept { m_reuseConeNormals = enabled; }
    [[nodiscard]] bool reuseConeNormals() const noexcept { return m_reuseConeNormals; }
    /// Bytes held by the per-thread face-normal scratch of every thread that
    /// has computed a cone with reuse (process-wide).
    [[nodiscard]] static std::size_t coneScratchBytes() noexcept;

    /// Incremental normal cones. A refitted chunk with at most
    /// `fallbackPercent` of its leaves queued keeps its cone's axis and only
    /// widens the cone to hold the new normals of its changed triangles, a pass
    /// over those triangles instead of two over the whole chunk. A cone that
    /// was unusable, or would become too wide, turns culling off for the chunk
    /// (an unusable cone). Either way the cone holds every normal of the chunk,
    /// so culling stays conservative, and the chunk is queued for an exact
    /// rebuild (computeStaleCones/publishStaleCones). Chunks with more leaves
    /// queued recompute their cone exactly, as without this option.
    void setIncrementalCones(bool enabled, u32 fallbackPercent = 50) noexcept
    {
        m_incrementalCones = enabled;
        m_coneFallbackPercent = fallbackPercent;
    }
    [[nodiscard]] bool incrementalCones() const noexcept { return m_incrementalCones; }

    /// True while some chunk's cone is not the exact one (widened or turned off).
    [[nodiscard]] bool hasStaleCones() const noexcept { return !m_staleCones.empty(); }
    /// Chunks whose cone is not exact.
    [[nodiscard]] u32 staleConeCount() const noexcept;
    /// True when the chunk's cone is widened or turned off, not exact.
    [[nodiscard]] bool chunkConeStale(u32 index) const noexcept { return m_chunks[index].coneStale; }
    /// Exact cone rebuild, step 1: computes the exact cones of up to
    /// `maxChunks` stale chunks from the mesh into scratch (in parallel with
    /// `jobs`). Reads the mesh and changes nothing that queries or chunkCone()
    /// read, so for a single writer it can run without the document lock while
    /// other threads read. The mesh must not change before publishStaleCones().
    /// Returns the number of chunks computed.
    u32 computeStaleCones(const Mesh& mesh, u32 maxChunks, core::JobSystem* jobs = nullptr);
    /// Step 2, under the lock: publishes the cones computed by step 1. Must
    /// follow it with no refit in between. Returns the cones published.
    u32 publishStaleCones() noexcept;
    /// Cones the last publishStaleCones() published.
    [[nodiscard]] u32 lastPublishedCones() const noexcept { return m_lastPublishedCones; }

    /// Diagnostics for incremental cones. For each eye, the triangles of the
    /// chunks the cones let through (coneFacesAway false) with the current
    /// cones, and with exact cones (computed here for the stale chunks).
    struct ConeLoss {
        u64 drawnTriangles = 0;      ///< summed over the eyes
        u64 drawnTrianglesExact = 0; ///< summed over the eyes
        u32 eyes = 0;
        u32 staleChunks = 0;
        u32 disabledChunks = 0;      ///< stale chunks with culling off
    };
    [[nodiscard]] ConeLoss measureConeLoss(const Mesh& mesh, std::span<const glm::vec3> eyes) const;

    /// Lets a chunk whose refit queued at least half of its leaves sweep its
    /// whole tree in node order (Bvh::finishRefit). Same boxes bit for bit.
    void setDenseRefit(bool enabled) noexcept { m_denseRefit = enabled; }
    [[nodiscard]] bool denseRefit() const noexcept { return m_denseRefit; }
    [[nodiscard]] const RefitTimings& lastRefitTimings() const noexcept { return m_lastRefitTimings; }

    /// Total node count (local trees + top level), for diagnostics.
    [[nodiscard]] std::size_t nodeCount() const noexcept;
    [[nodiscard]] std::size_t memoryBytes() const noexcept;
    /// The part of memoryBytes() that only the parallel markTriangles() uses.
    [[nodiscard]] std::size_t markScratchBytes() const noexcept;

    /// True when `other` has the same chunks and bit-identical boxes (and cones) everywhere.
    /// Used to check that threaded and single-threaded updates agree.
    [[nodiscard]] bool sameBounds(const ChunkedBvh& other, bool compareCones = true) const noexcept;

private:
    struct Chunk {
        Bvh tree;
        NormalCone cone;            // unusable unless m_normalCones
        u32 topLeaf = 0;            // index of this chunk's leaf in m_top
        bool inRefit = false;       // queued in m_refitChunks during a refit call
        bool optimizeQueued = false;
        bool coneStale = false;     // cone widened or turned off; listed in m_staleCones
    };

    void buildTop();
    void buildTopNode(u32 nodeIndex, u32 first, u32 count);
    /// Copies the chunk's box into its top-level leaf and updates the ancestors.
    void updateTopFor(u32 chunkIndex);
    void queueOptimize(u32 chunkIndex);
    /// Exact cone of a chunk (and no longer stale).
    void updateCone(const Mesh& mesh, u32 chunkIndex, NormalConeTimes* times = nullptr);
    /// Cone of the refitIndex-th chunk of the current refit, exact or widened.
    void updateChunkCone(const Mesh& mesh, std::size_t refitIndex);
    /// Queues `triangle`'s chunk for this refit (first time) and marks the triangle in it.
    void markForRefit(TriangleId triangle);
    void markTrianglesParallel(std::span<const TriangleId> triangles, core::JobSystem& jobs);
    /// Steps 2 and 3 of a partial refit: the marked chunks' local refits, cones and the top level.
    void finishPartialRefit(const Mesh& mesh, core::JobSystem* jobs);

    std::vector<Chunk> m_chunks;
    u32 m_shift = kDefaultChunkTriangleShift;
    bool m_normalCones = false;
    bool m_denseRefit = false;
    bool m_incrementalCones = false;
    bool m_reuseConeNormals = false;
    std::vector<NormalConeTimes> m_refitConeTimes; // per refitted chunk, last finishPartialRefit
    /// Exact cone of triangles [first, first + count), timed into `times`.
    [[nodiscard]] NormalCone exactCone(const Mesh& mesh, u32 first, u32 count, NormalConeTimes& times) const;
    u32 m_coneFallbackPercent = 50;
    u32 m_lastConeTriangles = 0;
    // Incremental cones.
    enum class ConeUpdate : u8 { None, Exact, Widened, Disabled };
    std::vector<u8> m_coneUpdate;          // per refitted chunk, last finishPartialRefit: a ConeUpdate
    std::vector<u32> m_coneVisited;        // per refitted chunk: triangles visited for its cone
    std::vector<u32> m_staleCones;         // chunks with coneStale (may also hold chunks no longer stale)
    std::vector<u32> m_rebuildChunks;      // step 1 -> step 2 of the exact rebuild
    std::vector<NormalCone> m_rebuildCones;
    u32 m_lastPublishedCones = 0;
    RefitTimings m_lastRefitTimings;
    std::vector<float> m_refitBoxMs;  // per refitted chunk, last finishPartialRefit (diagnostics)
    std::vector<float> m_refitConeMs;

    // Top level: a balanced tree with one chunk per leaf.
    std::vector<TopNode> m_top;
    std::vector<u32> m_topParents;
    std::vector<u32> m_topOrder; // scratch: chunk indices being partitioned

    std::vector<u32> m_optimizeQueue; // chunks whose local tree should be re-partitioned
    bool m_topStale = false;          // chunk boxes changed since the top level was built

    // Scratch for refit().
    std::vector<u32> m_refitChunks;
    std::vector<u8> m_rootChanged;

    // Scratch for the parallel markTriangles().
    struct MarkRun {
        u32 chunk = 0;
        u32 count = 0;  // triangles of this block in the chunk
        u32 offset = 0; // where they go in m_markBuckets
    };
    std::vector<std::vector<MarkRun>> m_markBlocks; // per block: its chunks, in first-occurrence order
    std::vector<u32> m_markCursor;                  // per chunk id: count, then bucket cursor; 0 between calls
    std::vector<u32> m_markStart;                   // per refit chunk: first bucket entry, plus the end
    std::vector<TriangleId> m_markBuckets;          // the triangles grouped by chunk, in list order
};

} // namespace se::geo
