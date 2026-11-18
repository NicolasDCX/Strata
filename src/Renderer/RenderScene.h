#pragma once

#include "Core/FairMutex.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/Mesh.h"
#include "Renderer/OrbitCamera.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <vector>

namespace se::render {

/// Brush cursor drawn on the surface by the mesh shader.
struct BrushCursor {
    bool visible = false;
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    glm::vec4 color{1.0f, 0.82f, 0.35f, 0.9f}; ///< rgb + opacity
};

enum class CullMode {
    Off,        ///< Submit every chunk.
    /// Submit chunks whose bounds intersect the view frustum and that are not
    /// entirely back-facing. A chunk is back-facing when its normal cone
    /// (geo::ChunkedBvh::chunkCone, published with its bounds) proves that
    /// every triangle faces away from the eye. Chunks without a usable cone, or
    /// whose GPU copy is older than the CPU mesh, are never rejected that way.
    On,
    OnUnmerged, ///< As On, but one draw call per chunk (to measure draw-call overhead).
    Frustum,    ///< Frustum culling only (no back-facing chunks rejected).
};

/// How the renderer draws and how much GPU memory it may use.
struct RenderSettings {
    /// May be changed at any time; takes effect on the next frame.
    CullMode cullMode = CullMode::On;
    /// Measurement only: the scene is drawn this many times per frame, so that
    /// the GPU is busy enough to leave its power-saving clocks. GPU time per
    /// pass is then the frame's GPU time divided by this. 1 for normal use.
    u32 drawRepeat = 1;

    // The following are read when GPU resources are created.

    /// Mesh data always lives in page pools (ResidentMeshBuffers). This is
    /// their GPU memory budget in bytes. 0 (with gpuBudgetFraction 0), the
    /// default: the pools are sized for the whole mesh plus the spare vertex
    /// slots that copy on write needs, so every page stays resident and the mesh
    /// is uploaded completely when resources are created. Any other value
    /// limits the pools to this size: pages are then uploaded on demand and
    /// evicted when the pools are full.
    u64 gpuBudgetBytes = 0;
    /// Alternative way to give a limited budget: as a fraction of what one
    /// complete copy of the mesh needs. Used when > 0 and gpuBudgetBytes == 0.
    double gpuBudgetFraction = 0.0;
    /// Most bytes copied to the GPU in one frame. Bounds the time a frame spends
    /// uploading; pages beyond it are deferred to later frames.
    u64 maxUploadBytesPerFrame = 32ull * 1024 * 1024;
    /// 2^n vertices per GPU vertex page.
    u32 gpuPageShift = 10;
    /// Opt-in coarse render fallback (`--lod`): keep a coarse stand-in of
    /// every group of chunks resident (counted against the budget) and draw it
    /// for visible chunks whose detailed pages are not usable. Off by default.
    /// It is allocated only when a budget is set that cannot hold the whole
    /// mesh (else it would never be drawn), unless coarseAlways.
    bool coarseFallback = false;
    /// Testing (seam checks with whole-mesh pools): allocate the fallback even
    /// without a budget.
    bool coarseAlways = false;
    /// 2^n consecutive chunks are simplified together and drawn coarse as one
    /// unit. Only the group's outer border is locked, so larger groups need
    /// fewer vertices for the same triangles (4 chunks at 1/32: about half the
    /// memory of single chunks at 1/16). Default 1 chunk: coarse topology is
    /// fixed at load time, and a group's few large interior triangles follow
    /// strong deformation much worse than single chunks, whose dense locked
    /// borders keep many exact vertices (see the coarse-fallback notes).
    u32 coarseGroupShift = 0;
    /// Triangles kept per group by the coarse fallback (border-locked; the
    /// border bounds how far it can go: single chunks end near 1/16 anyway).
    float coarseRatio = 1.0f / 16.0f;
    /// Draw all coarse groups with one multi-draw-indirect call per pass (when
    /// the device supports it); false: one draw call per group.
    bool coarseIndirectDraws = true;

    /// Test support, may change at any time: force coarse groups to test seams.
    /// 0 off, 1 every odd group coarse, 2 every group coarse.
    u32 coarseDebug = 0;

    /// True when an explicit budget was given (instead of the whole-mesh default).
    [[nodiscard]] bool limitedBudget() const noexcept { return gpuBudgetBytes > 0 || gpuBudgetFraction > 0.0; }
};

/// Everything the renderer reads to draw a frame. This struct is the whole
/// interface between the editor and the renderer: the renderer never sees
/// brushes, strokes or the application controller.
///
/// Ownership: the editor owns the scene and whatever it points to; both must
/// outlive the renderer.
///
/// Threading: the renderer runs on the GUI thread (QVulkanWindow renders there).
/// When another thread edits the mesh, `meshLock` must be set; the renderer then
/// holds it while it reads `mesh`, `chunks` and `*pendingVertexUpdates`, and for
/// nothing else. Camera, cursor and settings belong to the GUI thread.
struct RenderScene {
    const geo::Mesh* mesh = nullptr;
    const OrbitCamera* camera = nullptr;
    BrushCursor cursor;
    RenderSettings settings;

    /// Spatial chunks of `mesh`: their current bounds drive culling, and they
    /// define the units of index data. Null draws the mesh as a single chunk.
    const geo::ChunkedBvh* chunks = nullptr;

    /// Protects the vertex data of `mesh`, the bounds in `chunks` and
    /// `*pendingVertexUpdates`. Null when the mesh is only ever touched on the GUI thread.
    core::FairMutex* meshLock = nullptr;

    /// Vertices whose position/normal changed on the CPU and are not yet on the
    /// GPU. Filled by whoever edits the mesh; the renderer consumes and clears it.
    geo::DirtyVertexSet* pendingVertexUpdates = nullptr;
};

/// Measurements made by the renderer, for the status bar and the self-test.
struct RenderStats {
    // --- Last frame ------------------------------------------------------------
    double lastFrameCpuMs = 0.0;      ///< CPU time spent preparing and recording the frame.
    double lastGpuMs = 0.0;           ///< GPU time of the most recent frame whose result is available.
    double lastUploadMs = 0.0;        ///< CPU time of the last vertex upload.
    u32 lastUploadVertexCount = 0;
    u64 lastUploadBytes = 0;          ///< bytes copied to the GPU (vertex + index data)
    double lastMeshLockWaitMs = 0.0;  ///< Time the last frame waited for `meshLock`.
    double worstMeshLockWaitMs = 0.0; ///< Longest such wait so far.
    u64 frameCount = 0;

    u32 chunkCount = 0;
    u32 chunksVisible = 0;      ///< chunks that passed culling (all, when culling is off)
    u32 chunksDrawn = 0;        ///< of those, chunks whose data was on the GPU
    u32 trianglesSubmitted = 0;
    u32 drawCalls = 0;
    u32 chunksCoarse = 0;          ///< coarse fallback: visible chunks drawn coarse
    u32 coarseGroups = 0;          ///< coarse fallback: groups drawn coarse
    /// Coarse fallback: visible chunks that were ready to draw detailed but
    /// were drawn coarse because another chunk of their group was missing.
    u32 coarseReplacedDetailed = 0;
    bool coarseIndirectDraws = false; ///< coarse groups are drawn with one indirect call per pass
    u32 chunksMissing = 0;         ///< visible chunks drawn neither detailed nor coarse
    u32 coarseTriangles = 0;
    u64 coarseBytes = 0;           ///< GPU bytes of the coarse fallback (included in gpuBytesAllocated)
    u64 lastCoarseUploadBytes = 0; ///< included in lastUploadBytes
    /// With coarseDebug >= 2 only: drawn coarse vertices whose GPU copy differs
    /// from the CPU mesh (read back each frame; 0 means the proxies are current).
    u32 coarseMismatchedVertices = 0;
    u32 backFacingChunks = 0;      ///< frustum-visible chunks skipped as entirely back-facing
    /// Frustum-visible chunks whose cone would have rejected them but whose
    /// GPU copy is stale (an edit not uploaded yet): drawn to stay exact.
    u32 backFacingKeptStale = 0;
    double selectMs = 0.0;         ///< CPU time of chunk selection (frustum + back-facing tests, ordering)

    // --- GPU memory ----------------------------------------------------------------
    bool limitedBudget = false;       ///< an explicit budget was given (else: pools sized for the whole mesh)
    bool gpuTimingAvailable = false;
    u64 gpuBytesAllocated = 0;        ///< all mesh buffers created on the device
    u64 gpuBudgetBytes = 0;           ///< the configured budget; 0 for the whole-mesh default
    u32 gpuPageVertices = 0;          ///< size of a GPU vertex page, in vertices
    u32 gpuPageCount = 0;             ///< GPU vertex pages of the mesh
    u32 vertexSlots = 0;              ///< vertex pages the pool can hold
    u32 vertexPagesResident = 0;
    u32 indexSlots = 0;               ///< chunks whose indices the pool can hold
    u32 indexChunksResident = 0;
    u32 lastEvictions = 0;
    u32 vertexReserveSlots = 0;       ///< vertex slots kept for copy on write
    u32 chunksSkipped = 0;            ///< visible chunks left out because a pool was full
    u32 visibleVertexPages = 0;       ///< distinct vertex pages the visible chunks need
    u32 lastStalePages = 0;           ///< pages drawn with content older than the CPU's
    /// More uploads are waiting for the per-frame limit; the renderer keeps
    /// requesting frames until this is false.
    bool pendingWork = false;
    u64 totalUploadBytes = 0;
    u64 totalEvictions = 0;

    /// When set, every frame appends to the sample lists below (used by the
    /// self-test to report distributions; off by default so nothing grows).
    bool recordSamples = false;
    std::vector<float> frameCpuSamplesMs;      ///< CPU time spent on each frame
    std::vector<float> frameGpuSamplesMs;      ///< GPU time of frames as their results arrive
    std::vector<float> frameIntervalSamplesMs; ///< time between the starts of consecutive frames
    std::vector<float> lockWaitSamplesMs;      ///< time each frame waited for `meshLock`
    std::vector<u32> uploadVertexSamples;      ///< vertices copied to the GPU by each frame
    std::vector<u32> uploadKiBSamples;         ///< KiB copied to the GPU by each frame
    std::vector<u32> chunksDrawnSamples;
    std::vector<u32> trianglesSubmittedSamples; ///< triangles submitted by each frame
    std::vector<u32> backFacingSamples;         ///< chunks each frame skipped as back-facing
    std::vector<u32> chunksCoarseSamples;
    std::vector<u32> chunksMissingSamples;
    std::vector<u32> evictionSamples;
};

} // namespace se::render
