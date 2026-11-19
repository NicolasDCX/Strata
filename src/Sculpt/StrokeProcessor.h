#pragma once

#include "Core/Assert.h"
#include "Core/Timer.h"
#include "Core/Types.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Mesh.h"
#include "Geometry/Ray.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/EditRegion.h"
#include "Sculpt/Brush.h"
#include "Sculpt/PositionStaging.h"
#include "Sculpt/UndoStack.h"

#include <glm/vec3.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace se::sculpt {

/// Counters describing the work done since the stroke began.
struct StrokeStats {
    u32 dabCount = 0;
    u32 lastDabVertexCount = 0;
    u32 maxDabVertexCount = 0;
    // Time spent per stage, summed over the stroke (milliseconds).
    double gatherMs = 0.0;  ///< BVH sphere query, vertex selection, undo recording, average normal
    /// gatherMs by step (dab brushes; Grab gathers once at begin()).
    struct GatherTimings {
        double queryMs = 0.0;  ///< ChunkedBvh::querySphere
        double selectMs = 0.0; ///< de-duplication, distance test and falloff
        double recordMs = 0.0; ///< first touch per stroke: undo record
        double normalMs = 0.0; ///< average normal of the dab's vertices
        /// Wall time of undo record + average normal + page staging. When
        /// they overlap it is less than their sum (stageMs is not in gatherMs).
        double prepareWallMs = 0.0;
        u32 overlappedDabs = 0; ///< dabs whose preparation steps overlapped
        /// selectMs of the parallel gather by step: first corners (atomic
        /// minimum), distance test and falloff per block, join and reset.
        double selectFirstMs = 0.0;
        double selectTestMs = 0.0;
        double selectJoinMs = 0.0;
    } gather;
    double stageMs = 0.0;   ///< copying the vertex pages of the dab to staging pages
    double brushMs = 0.0;   ///< Brush::apply
    double normalsMs = 0.0; ///< edit region and new normals, computed from the proposed positions
    /// normalsMs by step, summed over the dabs.
    struct NormalTimings {
        double regionMs = 0.0;     ///< EditRegion::collect: moved vertices' triangles, then their one-ring (serial)
        double computeMs = 0.0;    ///< new normals of the affected vertices (wall time, parallel)
        double computeCpuMs = 0.0; ///< computeMs as CPU time summed over the tasks
        double dirtyMs = 0.0;      ///< marking the affected vertices dirty (serial)
        u64 affected = 0;          ///< vertices whose normal is computed
        u64 moved = 0;             ///< vertices the brush moved
        u64 triangles = 0;         ///< triangles with a moved corner
        u32 parallelRegions = 0;   ///< dabs whose region was walked in parallel
        u32 overlappedRegions = 0; ///< dabs whose region walk and leaf marking overlapped the preparation
    } normals;
    double publishMs = 0.0; ///< commit: writing the new positions and normals into the mesh
    double bvhMs = 0.0;     ///< BVH refit including the chunks' normal cones: leaf marking (prepare) + the rest (commit)
    double commitWaitMs = 0.0; ///< waiting in the commit-enter hook (the document lock)
    geo::ChunkedBvh::RefitTimings refit; ///< bvhMs split by refit phase, summed over the dabs
    u64 coneTriangles = 0;  ///< triangles visited to recompute normal cones
};

/// Turns a sequence of pointer rays into mesh edits.
///
/// Input is expressed as world-space rays, so this class needs no camera, window
/// or renderer. Every edit (dab) runs in two phases.
///
/// Prepare reads the mesh but does not change it:
///   1. gather the vertices inside the brush sphere (BVH sphere query; the
///      sphere may span any number of chunks and vertex pages)
///   2. record their previous positions for undo (first touch per stroke only)
///   3. copy the vertex pages holding them to staging pages and let the brush
///      deform them there (a staging page table maps all other pages to the mesh)
///   4. find the invalidated normals (moved vertices and their one-ring) and
///      compute them through the staging table, so they see the proposed
///      positions of moved vertices and the current positions of all others;
///      mark them dirty
///   5. queue the BVH leaves of the affected triangles (ChunkedBvh::markTriangles)
///
/// Commit publishes the result as one update, between the commit hooks:
///   6. write the new positions and normals into the mesh
///   7. refit the queued BVH boxes and the normal cones of their chunks, so the
///      next raycast sees the deformed surface
///   8. (in the leave hook) hand the dirty vertices to whoever mirrors the mesh
///
/// A host whose other threads read the mesh takes their lock in the enter hook
/// and releases it in the leave hook: readers then see the mesh either before
/// or after a dab, never in between, and wait only for the commit. This relies
/// on this object's thread being the only writer of the mesh and BVH.
///
/// Every step costs time proportional to the brush footprint, not to the mesh.
///
/// One instance is meant to live as long as its mesh: all scratch buffers are
/// reused, so dabs do not allocate once the buffers have grown to working size.
class StrokeProcessor {
public:
    /// Distance between two dabs as a fraction of the brush radius.
    static constexpr float kDabSpacing = 0.25f;
    /// Upper bound on dabs emitted by a single update() call (guards against
    /// a huge pointer jump with a tiny brush stalling the UI).
    static constexpr u32 kMaxDabsPerUpdate = 256;

    StrokeProcessor(geo::Mesh& mesh, geo::ChunkedBvh& bvh);

    /// Thread pool used for the per-vertex work of each dab (brush loop, normals,
    /// BVH leaf boxes). Null (the default) keeps everything on the calling
    /// thread. Results are identical either way.
    void setJobSystem(core::JobSystem* jobs) noexcept { m_jobs = jobs; }

    /// Gathers each dab's vertices with the job system: the sphere query per
    /// chunk, and vertex selection in fixed blocks whose results are joined in
    /// order. The vertex list and the weights (and so the undo record, which
    /// stays serial) are the same, in the same order, as the serial gather's,
    /// so results stay bit-identical. Dabs under 32K triangles, or a job system
    /// without helpers, gather on the calling thread. Off by default.
    void setParallelGather(bool enabled);
    [[nodiscard]] bool parallelGather() const noexcept { return m_parallelGather; }

    /// Queues each dab's BVH leaves with the job system, one chunk per task
    /// (ChunkedBvh::markTriangles with jobs). Same marks in the same order per
    /// chunk, same chunk order, so the refit is bit-identical. Lists under 32K
    /// triangles, or a job system without helpers, mark serially. Off by default.
    void setParallelMarking(bool enabled) noexcept { m_parallelMarking = enabled; }

    /// The dab staging pages, lent to the host between strokes (undo and redo
    /// stage their positions in it), so the copies are kept only once.
    [[nodiscard]] PositionStaging& staging() noexcept
    {
        SE_ASSERT(!active());
        return m_staging;
    }
    [[nodiscard]] bool parallelMarking() const noexcept { return m_parallelMarking; }
    /// Walks each dab's edit region (geo::EditRegion) on the helper threads when
    /// it moves at least EditRegion::kParallelMinVertices vertices: same lists in
    /// the same order. Off by default.
    void setParallelRegion(bool enabled) noexcept { m_parallelRegion = enabled; }
    /// Runs a dab's three serial preparation steps (undo record, average
    /// normal, page staging) at the same time on the helper threads when it
    /// moves at least kOverlapPrepareMinVertices vertices. Each step keeps its
    /// own loop and order and writes its own state: same results. Off by default
    /// (on in AppConfig).
    void setOverlapPrepare(bool enabled) noexcept { m_overlapPrepare = enabled; }
    [[nodiscard]] bool overlapPrepare() const noexcept { return m_overlapPrepare; }
    static constexpr std::size_t kOverlapPrepareMinVertices = 16384;
    /// When the preparation steps overlap (setOverlapPrepare), also walk the
    /// dab's edit region and queue its BVH leaves as a fourth step at the same
    /// time. Both need only the gathered vertices and the mesh's topology,
    /// which nothing changes before commit, and both produce the same lists in
    /// the same order wherever they run: same results. Off by default.
    void setOverlapRegion(bool enabled) noexcept { m_overlapRegion = enabled; }
    [[nodiscard]] bool overlapRegion() const noexcept { return m_overlapRegion; }
    [[nodiscard]] bool parallelRegion() const noexcept { return m_parallelRegion; }

    /// Called around the commit of every dab: `enter` before the first write to
    /// the mesh, `leave` once positions, normals and BVH are consistent again.
    /// A host that runs strokes on a worker thread takes its document lock in
    /// `enter`, and in `leave` drains the dirty vertices (drainDirty) and
    /// releases the lock. Without hooks, commits run unguarded.
    void setCommitHooks(std::function<void()> enter, std::function<void()> leave)
    {
        m_commitEnter = std::move(enter);
        m_commitLeave = std::move(leave);
    }

    /// Starts a stroke. Returns false (and stays inactive) when the ray misses the mesh.
    /// `brush` must outlive the stroke.
    bool begin(const Brush& brush, const BrushSettings& settings, const geo::Ray& ray);

    /// Feeds the next pointer position. No-op when no stroke is active.
    void update(const geo::Ray& ray);

    /// Finishes the stroke and returns its undo record (nullopt if nothing was changed).
    std::optional<StrokeDelta> end();

    [[nodiscard]] bool active() const noexcept { return m_brush != nullptr; }
    /// A Grab (Move) stroke is active: every update() re-applies the whole drag
    /// to the positions captured at begin(), so the result of a run of updates
    /// depends only on the last one that grabUpdateApplies() accepts.
    [[nodiscard]] bool grabActive() const noexcept;
    /// Whether update(ray) would change anything in the active Grab stroke
    /// (the ray meets the drag plane in front of its origin and the stroke
    /// captured vertices). An update it rejects is a no-op.
    [[nodiscard]] bool grabUpdateApplies(const geo::Ray& ray) const noexcept;
    [[nodiscard]] const StrokeStats& stats() const noexcept { return m_stats; }
    /// Scratch kept between dabs: the edit region, the staging pages and their
    /// page table, the new normals, the parallel gather's per-chunk and
    /// per-block lists, and the BVH's parallel-marking lists.
    [[nodiscard]] std::size_t editScratchBytes() const noexcept;
    /// Working memory lent to the brushes (DabScratch), not in editScratchBytes().
    [[nodiscard]] std::size_t brushScratchBytes() const noexcept { return m_scratch.memoryBytes(); }
    /// The part of editScratchBytes() only the parallel gather uses.
    [[nodiscard]] std::size_t parallelGatherScratchBytes() const noexcept;

    /// Brush centre of the most recent edit (valid while a stroke is active).
    [[nodiscard]] glm::vec3 lastCenter() const noexcept { return m_lastCenter; }

    /// Appends the total time of every dab since the previous call (ms, gather
    /// to the end of the commit-leave hook) to `destination`, then forgets them.
    void takeDabSamples(std::vector<float>& destination);

    /// Adds the vertices changed since the previous call to `destination`, then
    /// forgets them. Does not allocate once `destination` is sized.
    void drainDirty(geo::DirtyVertexSet& destination);

private:
    void gather(const glm::vec3& center, float radius);
    void gatherParallel(const glm::vec3& center, float radius);
    void recordForUndo();
    void selectFirstCornersSerial(const glm::vec3& center, float radius);
    [[nodiscard]] glm::vec3 averageNormal(const glm::vec3& fallback) const noexcept;
    void applyDab(const geo::RayHit& hit);
    void applyGrab(const geo::Ray& ray);
    /// Distance along `ray` to the drag plane, or nullopt when applyGrab()
    /// does nothing for it. Shared by applyGrab() and grabUpdateApplies().
    [[nodiscard]] std::optional<float> grabPlaneDistance(const geo::Ray& ray) const noexcept;
    /// Copies the vertex pages holding m_vertices to staging pages and points
    /// the staging table at them.
    void stagePages();
    /// Points the staging table back at the mesh's pages.
    void releasePages();
    [[nodiscard]] geo::PagedArrayView<glm::vec3> stagedPositions() noexcept;
    /// Prepare steps 4 (normals) and the commit.
    void collectRegion();
    void finishEdit();
    void commit();

    geo::Mesh& m_mesh;
    geo::ChunkedBvh& m_bvh;

    // Stroke state
    const Brush* m_brush = nullptr;
    BrushSettings m_settings;
    StrokeDelta m_delta;
    StrokeStats m_stats;
    geo::DirtyVertexSet m_dirty;
    glm::vec3 m_lastCenter{0.0f};

    // Dab spacing state
    geo::Ray m_lastRay;
    glm::vec3 m_lastHitPosition{0.0f};
    bool m_hasLastHit = false;
    float m_distanceSinceDab = 0.0f;

    // Grab state
    std::vector<glm::vec3> m_grabOriginals;
    glm::vec3 m_grabOrigin{0.0f};
    glm::vec3 m_grabPlaneNormal{0.0f, 0.0f, 1.0f};
    glm::vec3 m_grabSurfaceNormal{0.0f, 1.0f, 0.0f};

    // Reused scratch storage
    std::vector<u32> m_triangles;      // BVH query result
    std::vector<u32> m_vertices;       // vertices inside the brush
    std::vector<float> m_weights;      // falloff, parallel to m_vertices
    DabScratch m_scratch;              // lent to brushes
    // Per vertex. Serial gather: the last gather that visited it. Parallel
    // gather: the index of its first corner in this gather's triangle list, or
    // kNoCorner (reset after every gather by walking the selected vertices).
    std::vector<u32> m_gatherStamp;
    std::vector<u32> m_recordStamp;    // per vertex: last stroke that recorded it
    u32 m_gatherGeneration = 0;
    u32 m_strokeGeneration = 0;
    bool m_parallelGather = false;
    bool m_parallelMarking = false;
    bool m_parallelRegion = false;
    bool m_overlapPrepare = false;
    bool m_overlapRegion = false;
    bool m_regionReady = false; // this dab's region is walked and its leaves marked
    // Parallel gather scratch, reused.
    std::vector<u32> m_queryChunks;                 // chunks searched, in query order
    std::vector<std::vector<u32>> m_chunkTriangles; // per searched chunk: its triangles
    std::vector<std::size_t> m_offsets;             // prefix sums of per-chunk or per-block counts
    struct GatherBlock {
        std::vector<u32> inside;   // first occurrences inside the sphere, in order
        std::vector<float> weights;
        std::vector<u32> outside;  // first occurrences outside (only to reset their slot)
    };
    std::vector<GatherBlock> m_blocks;
    geo::EditRegion m_region; // per dab: affected triangles and vertices, shared by normals and BVH
    PositionStaging m_staging;             // per dab: the vertex pages of the dab, copied
    std::vector<glm::vec3> m_newNormals;   // per dab, parallel to m_region.vertices()
    core::JobSystem* m_jobs = nullptr;
    std::function<void()> m_commitEnter;
    std::function<void()> m_commitLeave;
    core::Stopwatch m_dabTimer;      // since the current dab began
    std::vector<float> m_dabSamples; // total dab times not yet taken
};

} // namespace se::sculpt
