#pragma once

#include "Core/FairMutex.h"
#include "Core/JobSystem.h"
#include "Core/Timer.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/Mesh.h"
#include "Geometry/Ray.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/NormalUpdater.h"
#include "Sculpt/StrokeProcessor.h"
#include "Sculpt/UndoStack.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace se::app {

struct AppConfig {
    /// Icosphere subdivision level: 20 * 4^n triangles (6 -> 81,920, 7 -> 327,680).
    int sphereSubdivisions = 6;
    float sphereRadius = 1.0f;
    /// Threads helping the sculpt thread with per-vertex loops.
    /// Negative: choose from the hardware. 0: none (dabs run on the sculpt thread only).
    int jobThreads = -1;
    /// Page capacities of the mesh storage.
    geo::MeshLayout meshLayout;
    /// 2^n triangles per spatial chunk of the acceleration structure.
    u32 chunkTriangleShift = geo::ChunkedBvh::kDefaultChunkTriangleShift;
    /// Keep a normal cone per chunk, updated with the chunk's bounds in every
    /// BVH refit on the sculpt thread, so the renderer can skip back-facing chunks.
    bool normalCones = true;
    /// Gather each dab's vertices with the helper threads (bit-identical to the
    /// serial gather; see StrokeProcessor::setParallelGather). On by default:
    /// 14 to 18% shorter dabs at 5.2M triangles and 16% at 21M with 11
    /// helpers; without helpers, or for small dabs, it gathers serially.
    bool parallelGather = true;
    /// Queue each dab's BVH leaves with the helper threads, one chunk per task
    /// (bit-identical; see StrokeProcessor::setParallelMarking). On by default:
    /// marking 4 to 5x faster at 21M triangles with 11 helpers, dabs 15 to 23%
    /// shorter; without helpers, or under 32K triangles, it marks serially.
    bool parallelMarking = true;
    /// Walk the edit region of dabs, undo and redo on the helper threads when
    /// at least 16K vertices moved (geo::EditRegion; same lists, same order).
    /// Default: with 11 helpers the walk is 2.2-2.4x faster at 21M and 84M
    /// (84M Draw dab 33 -> 25.5 ms, undo region 61 -> 24 ms); it keeps a 4-byte
    /// stamp per vertex on the sculpt thread (160 MiB at 84M).
    bool parallelRegion = true;
    /// Overlap each large dab's undo record, average normal and page staging
    /// on the helper threads (StrokeProcessor::setOverlapPrepare; same results).
    bool overlapPrepare = true;
    /// With overlapPrepare: also walk the edit region and queue the BVH leaves
    /// during the preparation (StrokeProcessor::setOverlapRegion; same results).
    bool overlapRegion = false;
    /// Undo and redo compute the new normals before taking the document lock,
    /// from the history entry's positions staged over the mesh (the dab staging
    /// pages), and only write them under the lock. Same results bit for bit.
    bool historyNormalsBeforeLock = false;
    /// Latest-wins Move updates: when the sculpt thread takes a Move (Grab)
    /// update and more updates of the same stroke are already queued right
    /// behind it, it applies only the newest one that would change anything
    /// and drops the others. A Grab update re-applies the whole drag to the
    /// positions captured at the stroke's start, so the final positions,
    /// normals, undo record, dirty pages and (once idle) BVH boxes and cones
    /// are the same bit for bit; only intermediate frames are skipped. Never
    /// crosses a begin, end, undo or redo; Draw and Smooth are never coalesced.
    /// On by default: at 84M triangles, radius 0.25, a 120 Hz Move drained in
    /// 1.3-1.5 s after release and a 240 Hz one in 4.7-5.0 s; with it, 14-26 ms
    /// (README, "Latest-wins Move updates").
    bool coalesceMoveUpdates = true;
    /// BVH refit: a chunk whose refit touches at least half of its leaves
    /// recomputes its whole tree in node order (ChunkedBvh::setDenseRefit).
    /// Same boxes bit for bit; faster for undo/redo and large dabs (README,
    /// "Dense box refit").
    bool denseBoxRefit = true;
    /// Normal cones: a refit that changed at most `coneFallbackPercent` of a
    /// chunk's leaves widens the chunk's cone around its old axis by the new
    /// normals (or turns the chunk's culling off) instead of recomputing it,
    /// and the sculpt thread rebuilds the exact cone in the background
    /// (ChunkedBvh::setIncrementalCones). Culling stays conservative. On by
    /// default: dab lock holds 10 to 40% shorter for small dabs, undo/redo
    /// holds 3 to 9% shorter without helpers (README, "Incremental normal cones").
    bool incrementalCones = true;
    u32 coneFallbackPercent = 50;
    /// Exact cones keep the first pass's face normals in a per-thread scratch
    /// (64 KiB per thread for 4,096-triangle chunks) and reuse them in the
    /// second pass (ChunkedBvh::setReuseConeNormals). Same cones bit for bit.
    /// On by default: exact cones take about a third less CPU, undo/redo holds
    /// 5 to 9% shorter without helpers (README, "Face normals reused in exact cones").
    bool reuseConeNormals = true;
    /// Diagnostics (benchmarks): after every undo/redo and stroke, measure how
    /// many more triangles the current cones let through than exact cones
    /// would, from fixed eyes (InteractionStats::lastConeLoss). Costly.
    bool measureConeLoss = false;
};

/// Per-stroke overrides coming from modifier keys.
struct StrokeModifiers {
    bool invert = false; ///< Flip the brush direction for this stroke.
    bool smooth = false; ///< Use the Smooth brush for this stroke, whatever is selected.
};

/// Measurements of the sculpt thread, taken with a monotonic clock.
struct InteractionStats {
    double lastUpdateMs = 0.0;    ///< Time the sculpt thread spent on the last stroke input.
    double worstUpdateMs = 0.0;   ///< Worst stroke input of the current/last stroke.
    double lastStrokeEndMs = 0.0; ///< Time spent finishing the last stroke (undo record).
    u32 lastDabVertexCount = 0;
    u32 strokeDabCount = 0;
    u32 queuedInputs = 0;         ///< Inputs waiting for the sculpt thread when this was read.
    double lastUndoRedoMs = 0.0;  ///< Time the sculpt thread spent on the last undo or redo.
    /// Current/last stroke, summed over its dabs: time in the BVH refit (which
    /// includes the normal cones when they are on), and triangles visited to
    /// recompute cones.
    double strokeBvhMs = 0.0;
    double strokeNormalsMs = 0.0;
    double strokeBrushMs = 0.0;
    u64 strokeConeTriangles = 0;
    geo::ChunkedBvh::RefitTimings strokeRefit; ///< refit phases and cone updates, summed over the dabs of the stroke
    sculpt::StrokeStats strokeStages; ///< every stage timer of the current/last stroke, summed over its dabs
    /// Sculpt-thread time of every input of the current/last stroke (ms).
    std::vector<float> updateSamplesMs;
    /// One per stroke input (begin and updates) of the current/last stroke:
    /// when it was queued and when the sculpt thread took it and finished it
    /// (ms on the steady clock), and how many dabs it applied. Finishing an
    /// input that applied dabs means its last dab is committed and visible to
    /// the next frame.
    struct InputSample {
        double queuedAtMs = 0.0;
        double startedAtMs = 0.0;
        double doneAtMs = 0.0;
        u32 dabs = 0;
        /// With dabs > 0: commitSequence() after this input's last dab was
        /// published, and when (steady_clock ms, just before the lock was released).
        u64 commitSeq = 0;
        double committedAtMs = 0.0;
        /// AppConfig::coalesceMoveUpdates: a newer queued update of the same
        /// Move stroke replaced this one, which was dropped without being
        /// applied (startedAtMs == doneAtMs: when it was dropped). Its result
        /// becomes visible with the next sample that is not coalesced.
        bool coalesced = false;
    };
    std::vector<InputSample> inputSamples;
    /// Inputs of the current/last stroke dropped by AppConfig::coalesceMoveUpdates,
    /// and since startup.
    u32 strokeCoalescedInputs = 0;
    u64 coalescedInputs = 0;
    /// Every hold of the document lock by the sculpt thread during the
    /// current/last stroke (ms): one per dab, the dab's commit. Stroke commands
    /// take the lock for nothing else.
    std::vector<float> lockHoldSamplesMs;
    /// Total time of every dab of the current/last stroke (ms), from the start
    /// of its gather to the end of its publication, lock wait included.
    std::vector<float> dabSamplesMs;
    double lastUndoRedoHoldMs = 0.0;  ///< Document lock hold of the last undo or redo.
    /// Where the last undo or redo spent its time. prepareMs is before the
    /// lock; the other times add up to (about) lastUndoRedoHoldMs.
    struct HistoryProfile {
        double prepareMs = 0.0;    ///< edit region, BVH leaf marking, staged normals when computed before the lock
        double regionMs = 0.0;     ///< prepareMs: the edit region walk (geo::EditRegion::collect)
        bool parallelRegion = false; ///< the region was walked in parallel
        double positionsMs = 0.0;  ///< write the entry's positions into the mesh
        double normalsMs = 0.0;    ///< recompute (or publish) the affected normals
        double normalsCpuMs = 0.0; ///< normalsMs as CPU time summed over the tasks
        double dirtyMs = 0.0;      ///< mark the dirty pages, invalidate page bounds
        double refitMs = 0.0;      ///< local BVH refits and normal cones (wall time)
        double boxCpuMs = 0.0;     ///< refitMs split by stage, CPU time summed over the threads:
        double coneCpuMs = 0.0;    ///< the chunks' box refits and their normal cones
        u32 chunks = 0;            ///< chunks refitted
        u32 coneTriangles = 0;     ///< triangles visited to recompute the cones
        geo::ChunkedBvh::RefitTimings refit; ///< the BVH's own breakdown of the refit
        double topMs = 0.0;        ///< top-level BVH
        u32 vertices = 0;          ///< vertices written
        u32 affected = 0;          ///< vertices whose normal is recomputed
        u32 triangles = 0;         ///< triangles refitted
    } lastHistory;
    double worstOptimizeHoldMs = 0.0; ///< Longest background BVH step since startup (lock hold).
    u64 optimizeSteps = 0;            ///< Background BVH steps since startup.
    /// Background exact rebuild of widened cones (AppConfig::incrementalCones),
    /// since startup: steps, chunks rebuilt, compute time before the lock
    /// (total and worst step, wall), and the longest publication hold.
    u64 coneRebuildSteps = 0;
    u64 coneRebuildChunks = 0;
    double coneRebuildMs = 0.0;
    double worstConeRebuildMs = 0.0;
    double worstConeRebuildHoldMs = 0.0;
    /// AppConfig::measureConeLoss: measured after the last undo/redo or stroke end.
    geo::ChunkedBvh::ConeLoss lastConeLoss;
    std::size_t editScratchBytes = 0; ///< Per-dab scratch kept by the stroke processor, history normals and edit region stamps.
};

/// The single entry point the editor uses to drive the engine.
///
/// Owns the document (mesh + acceleration structure), the tools and the undo
/// history. It has no dependency on Qt or Vulkan: input arrives as world-space
/// rays and output leaves as a set of dirty vertices.
///
/// Threading model
/// ---------------
/// All mesh edits happen on one internal *sculpt thread*. The public functions
/// are called from one *client thread* (the UI thread) and never wait for an
/// edit: stroke input, undo and redo are queued and return immediately.
///
///   client thread --(command queue)--> sculpt thread --(job system)--> helpers
///
/// The mesh's vertex data, the BVH and the pending dirty set are protected by
/// documentMutex(). The sculpt thread holds it for one dab at a time and hands
/// it over between dabs (core::FairMutex), so a reader such as the renderer
/// waits for at most one dab, never for a whole stroke. After a stroke, the
/// sculpt thread tidies the BVH in small steps while it has nothing else to do,
/// releasing the lock between steps; a new stroke can start at any time.
///
/// Because edits are asynchronous, state read through this class (undo counts,
/// stats, stroke centre) is the latest *published* state. Call waitUntilIdle()
/// when a deterministic view is needed (tests, scripted runs).
class AppController {
public:
    explicit AppController(const AppConfig& config = {});
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // --- Document -----------------------------------------------------------
    /// Topology (indices, adjacency, counts) never changes and may be read at any
    /// time. Positions and normals may only be read while holding documentMutex()
    /// or after waitUntilIdle() with no further input.
    [[nodiscard]] const geo::Mesh& mesh() const noexcept { return m_mesh; }

    /// Protects the mesh's vertex data, the BVH and pendingVertexUpdates().
    [[nodiscard]] core::FairMutex& documentMutex() const noexcept { return m_documentMutex; }

    /// Vertices whose position or normal changed and that the consumer (the
    /// renderer) has not picked up yet. Read and clear only under documentMutex().
    [[nodiscard]] geo::DirtyVertexSet& pendingVertexUpdates() noexcept { return m_dirty; }

    /// Dabs published so far (latency tracing): incremented with each dab's
    /// commit, under documentMutex(). Read only under documentMutex().
    [[nodiscard]] const u64& commitSequence() const noexcept { return m_commitSeq; }

    /// The spatial chunks and their BVHs. Chunk membership never changes; chunk
    /// bounds do, and may only be read while holding documentMutex().
    [[nodiscard]] const geo::ChunkedBvh& spatialIndex() const noexcept { return m_bvh; }

    /// Current bounds of the model.
    [[nodiscard]] geo::Aabb bounds() const;

    /// Closest surface point under a ray (used for the brush cursor).
    [[nodiscard]] std::optional<geo::RayHit> pick(const geo::Ray& ray) const;

    // --- Tools (client thread only) -----------------------------------------
    void setActiveBrush(sculpt::BrushId id);
    [[nodiscard]] sculpt::BrushId activeBrush() const noexcept { return m_activeBrush; }
    [[nodiscard]] std::string_view activeBrushName() const;
    [[nodiscard]] const sculpt::BrushSettings& activeSettings() const;
    [[nodiscard]] const sculpt::BrushLibrary& brushes() const noexcept { return m_brushes; }

    void setRadius(float radius);
    void setStrength(float strength);
    void setInvert(bool invert);

    static constexpr float kMinRadius = 0.01f;
    static constexpr float kMaxRadius = 2.0f;

    // --- Stroke -------------------------------------------------------------
    /// Returns false when the ray misses the mesh (no stroke is started). The hit
    /// test runs on the calling thread; the stroke itself is queued.
    bool beginStroke(const geo::Ray& ray, StrokeModifiers modifiers = {});
    void updateStroke(const geo::Ray& ray);
    void endStroke();
    /// True between a successful beginStroke() and endStroke() on the client side.
    [[nodiscard]] bool strokeActive() const noexcept { return m_strokeOpen; }
    /// Brush centre of the latest edit the sculpt thread has completed.
    [[nodiscard]] glm::vec3 strokeCenter() const;
    /// Radius used by the active stroke.
    [[nodiscard]] float strokeRadius() const noexcept { return m_strokeRadius; }

    // --- History ------------------------------------------------------------
    /// Queue an undo / redo. Ignored while a stroke is open; a request with
    /// nothing to undo or redo does nothing.
    void undo();
    void redo();
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] std::size_t undoCount() const;
    [[nodiscard]] std::size_t redoCount() const;
    [[nodiscard]] std::size_t undoMemoryBytes() const;

    // --- Synchronisation ----------------------------------------------------
    /// Blocks until every queued input is applied and background BVH work is done.
    void waitUntilIdle();
    /// AppConfig::historyNormalsBeforeLock, as configured.
    [[nodiscard]] bool historyNormalsBeforeLock() const noexcept { return m_historyNormalsBeforeLock; }
    /// The undo history. Test support: read only after waitUntilIdle() with no
    /// further input (the sculpt thread owns it).
    [[nodiscard]] const sculpt::UndoStack& history() const noexcept { return m_undo; }
    /// Diagnostics (self-test): waits until idle, then recomputes every vertex
    /// normal from scratch with the whole-mesh reference (one scatter over all
    /// triangles, PagedMesh::recomputeNormals()) under the document lock and
    /// marks every vertex dirty, so the renderer shows the reference normals.
    /// Positions, BVH and history are not touched.
    void recomputeAllNormalsForReference();
    /// AppConfig::coalesceMoveUpdates, as configured.
    [[nodiscard]] bool coalesceMoveUpdates() const noexcept { return m_coalesceMoveUpdates; }
    /// AppConfig::incrementalCones, as configured (and cones on).
    [[nodiscard]] bool incrementalCones() const noexcept { return m_bvh.incrementalCones(); }

    /// `listener` is called from the sculpt thread after it changed the mesh or
    /// the history. It must be cheap and must not call back into this object
    /// (typically it posts an event to the UI thread). Pass an empty function to
    /// remove it; when this returns, the old listener is no longer running.
    void setChangeListener(std::function<void()> listener);

    // --- Presentation -------------------------------------------------------
    /// Moves the pending dirty vertices into `destination` (locks the document).
    /// The renderer uses documentMutex() + pendingVertexUpdates() directly so that
    /// taking the set and copying the vertex data happen under one lock.
    void drainDirtyVertices(geo::DirtyVertexSet& destination);
    [[nodiscard]] bool hasDirtyVertices() const;

    [[nodiscard]] InteractionStats stats() const;
    [[nodiscard]] unsigned jobThreadCount() const noexcept { return m_jobs ? m_jobs->workerCount() : 0u; }
    /// Memory held by the acceleration structure. Locks the document.
    [[nodiscard]] std::size_t accelerationMemoryBytes() const;
    [[nodiscard]] u32 chunkCount() const noexcept { return m_bvh.chunkCount(); }
    /// True when chunks keep normal cones (AppConfig::normalCones).
    [[nodiscard]] bool hasNormalCones() const noexcept { return m_bvh.hasNormalCones(); }
    /// Test support: chunks whose published normal cone does not contain the
    /// current normal of every one of their triangles (must be 0), and the
    /// number of chunks with a usable cone. Locks the document.
    [[nodiscard]] std::pair<u32, u32> checkNormalCones() const;

private:
    struct Command {
        enum class Type { BeginStroke, UpdateStroke, EndStroke, Undo, Redo };
        Type type = Type::UpdateStroke;
        geo::Ray ray;
        sculpt::BrushId brush = sculpt::BrushId::Draw;
        sculpt::BrushSettings settings;
        double queuedAtMs = 0.0; ///< steady clock, for InteractionStats::inputSamples
    };

    /// State published by the sculpt thread for the client thread.
    struct Published {
        InteractionStats stats;
        glm::vec3 strokeCenter{0.0f};
        std::size_t undoCount = 0;
        std::size_t redoCount = 0;
        std::size_t undoMemoryBytes = 0;
    };

    void enqueue(const Command& command);
    void sculptThreadMain();
    void execute(const Command& command);
    /// Sculpt thread, under m_queueMutex, after taking a Move update: takes the
    /// updates queued right behind it and returns the one to apply (see
    /// AppConfig::coalesceMoveUpdates); the dropped ones go to m_coalesced*.
    Command coalesceMoveUpdates(Command command);
    void publish(const Command& command, double elapsedMs);
    void notifyChanged();

    // Sculpt thread: document lock with hold-time accounting.
    enum class HoldKind { Dab, History, Optimize, ConeRebuild };
    void lockDocument();
    void unlockDocument(HoldKind kind);

    // Declared first: the acceleration structure is built with it, and it must
    // outlive everything that may still be running jobs.
    std::unique_ptr<core::JobSystem> m_jobs;

    // --- Document: guarded by m_documentMutex --------------------------------
    mutable core::FairMutex m_documentMutex;
    geo::Mesh m_mesh;
    geo::ChunkedBvh m_bvh;
    sculpt::StrokeProcessor m_stroke;
    sculpt::UndoStack m_undo;
    sculpt::NormalUpdater m_normalUpdater;
    geo::EditRegion m_region; // undo/redo: affected triangles and vertices
    geo::DirtyVertexSet m_dirty;
    u64 m_commitSeq = 0;        ///< see commitSequence()
    double m_lastCommitMs = 0.0; ///< steady_clock ms of the last dab commit

    // --- Client thread only ----------------------------------------------------
    sculpt::BrushLibrary m_brushes; // brush objects are immutable; settings are client-side
    sculpt::BrushId m_activeBrush = sculpt::BrushId::Draw;
    bool m_strokeOpen = false;
    float m_strokeRadius = 0.0f;

    // --- Published state: guarded by m_publishedMutex --------------------------
    mutable std::mutex m_publishedMutex;
    Published m_published;

    // --- Command queue: guarded by m_queueMutex --------------------------------
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::condition_variable m_becameIdle;
    std::deque<Command> m_queue;
    bool m_working = false; // sculpt thread is executing a command or optimizing
    bool m_stopping = false;

    std::mutex m_listenerMutex;
    std::function<void()> m_listener;

    // --- Sculpt thread only ----------------------------------------------------
    bool m_documentLocked = false;       // the sculpt thread holds m_documentMutex
    core::Stopwatch m_holdTimer;         // since the document lock was taken
    std::vector<float> m_pendingHoldsMs; // stroke holds not yet published
    std::vector<float> m_pendingDabsMs;  // dab latencies not yet published
    std::vector<InteractionStats::InputSample> m_pendingInputs; // input timings not yet published
    double m_lastHistoryHoldMs = 0.0;
    InteractionStats::HistoryProfile m_historyProfile; // sculpt thread: the last undo or redo
    bool m_historyNormalsBeforeLock = false;
    bool m_coalesceMoveUpdates = false;
    // Dropped updates of the command being executed (queue times), in queue
    // order: those before the applied one and those after it.
    std::vector<double> m_coalescedBefore;
    std::vector<double> m_coalescedAfter;
    u32 m_coalescedTotal = 0; // dropped updates not yet published
    std::vector<glm::vec3> m_historyNormals; // undo/redo: new normals, parallel to m_region.vertices()
    bool m_optimizePending = false;
    bool m_measureConeLoss = false;
    double m_lastConeRebuildMs = 0.0; // compute time of the current cone rebuild step
    std::vector<glm::vec3> m_coneLossEyes;
    void measureConeLoss();

    std::thread m_sculptThread; // last member: started when everything else exists
};

} // namespace se::app
