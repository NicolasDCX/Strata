#include "Application/AppController.h"

#include "Core/Log.h"
#include "Core/Timer.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/Brushes/SmoothBrush.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace se::app {

namespace {

/// Triangles re-partitioned per background BVH step. Small enough (well under a
/// millisecond) that a reader or a new stroke never waits noticeably for a step.
constexpr u32 kOptimizeStepTriangles = 4096;
/// Background exact cone rebuild: chunks per step and thread (4 chunks of
/// 4,096 triangles take about 0.25 ms on one thread).
constexpr u32 kConeRebuildChunksPerThread = 4;

geo::ChunkedBvh buildBvh(const geo::Mesh& mesh, u32 chunkTriangleShift, core::JobSystem* jobs, bool normalCones)
{
    geo::ChunkedBvh bvh;
    bvh.build(mesh, chunkTriangleShift, jobs, normalCones);
    return bvh;
}

std::unique_ptr<core::JobSystem> makeJobSystem(int requestedThreads)
{
    const unsigned threads = requestedThreads < 0 ? core::JobSystem::defaultWorkerCount()
                                                  : static_cast<unsigned>(std::min(requestedThreads, 64));
    return threads > 0 ? std::make_unique<core::JobSystem>(threads) : nullptr;
}

} // namespace

AppController::AppController(const AppConfig& config)
    : m_jobs(makeJobSystem(config.jobThreads)),
      m_mesh(geo::makeIcosphere(config.sphereSubdivisions, config.sphereRadius, geo::VertexOrder::Spatial,
                                config.meshLayout)),
      m_bvh(buildBvh(m_mesh, config.chunkTriangleShift, m_jobs.get(), config.normalCones)),
      m_stroke(m_mesh, m_bvh),
      m_dirty(m_mesh.makeDirtySet())
{
    m_stroke.setJobSystem(m_jobs.get());
    m_stroke.setParallelGather(config.parallelGather);
    m_stroke.setParallelMarking(config.parallelMarking);
    m_stroke.setParallelRegion(config.parallelRegion);
    m_stroke.setOverlapPrepare(config.overlapPrepare);
    m_stroke.setOverlapRegion(config.overlapRegion);
    m_historyNormalsBeforeLock = config.historyNormalsBeforeLock;
    m_coalesceMoveUpdates = config.coalesceMoveUpdates;
    m_bvh.setDenseRefit(config.denseBoxRefit);
    m_bvh.setIncrementalCones(config.incrementalCones && config.normalCones, config.coneFallbackPercent);
    m_bvh.setReuseConeNormals(config.reuseConeNormals);
    m_measureConeLoss = config.measureConeLoss && config.normalCones;
    if (m_measureConeLoss) {
        // 6 axis and 8 diagonal directions, 3 radii from the centre.
        const geo::Aabb box = m_bvh.bounds();
        const glm::vec3 center = box.center();
        const float distance = 1.5f * glm::length(box.max - box.min);
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
                for (int z = -1; z <= 1; ++z) {
                    const int nonzero = (x != 0) + (y != 0) + (z != 0);
                    if (nonzero == 1 || nonzero == 3) {
                        m_coneLossEyes.push_back(center + distance * glm::normalize(glm::vec3(x, y, z)));
                    }
                }
            }
        }
    }
    // A dab is prepared without the document lock (this thread is the only
    // writer, so reading the mesh needs no lock) and takes it only to publish
    // its positions, normals, BVH boxes, cones and dirty vertices together.
    m_stroke.setCommitHooks([this] { lockDocument(); },
                            [this] {
                                m_stroke.drainDirty(m_dirty);
                                {
                                    const std::scoped_lock lock(m_publishedMutex);
                                    m_published.strokeCenter = m_stroke.lastCenter();
                                }
                                ++m_commitSeq;
                                m_lastCommitMs = std::chrono::duration<double, std::milli>(
                                                     std::chrono::steady_clock::now().time_since_epoch()).count();
                                unlockDocument(HoldKind::Dab);
                            });

    constexpr double kMiB = 1024.0 * 1024.0;
    const geo::MeshMemory memory = m_mesh.memory();
    core::logInfo("Sculpt mesh: {} triangles, {} vertices in {} vertex pages of {} and {} triangle pages of {}",
                  m_mesh.triangleCount(), m_mesh.vertexCount(), m_mesh.vertexPageCount(),
                  m_mesh.layout().vertexPageCapacity(), m_mesh.trianglePageCount(), m_mesh.layout().trianglePageCapacity());
    core::logInfo("Mesh memory: {:.1f} MiB (positions {:.1f}, normals {:.1f}, triangles {:.1f}, adjacency {:.1f})",
                  static_cast<double>(memory.total()) / kMiB, static_cast<double>(memory.positions) / kMiB,
                  static_cast<double>(memory.normals) / kMiB, static_cast<double>(memory.triangles) / kMiB,
                  static_cast<double>(memory.adjacency) / kMiB);
    core::logInfo("Adjacency: {} layout, neighbours {} {:.1f} MiB ({} vertices on the general path), incident triangles "
                  "{:.1f} MiB, {} rows kept as u32 ids",
                  m_mesh.layout().adjacency == geo::AdjacencyLayout::Compact ? "compact" : "wide",
                  m_mesh.storesNeighbors() ? "stored" : "derived", static_cast<double>(memory.adjacencyNeighbors) / kMiB,
                  m_mesh.generalFanCount(), static_cast<double>(memory.adjacencyIncidentTriangles) / kMiB,
                  m_mesh.wideAdjacencyRows());
    core::logInfo("Acceleration: {} chunks of up to {} triangles, {} nodes, {:.1f} MiB, normal cones {}", m_bvh.chunkCount(),
                  1u << m_bvh.chunkTriangleShift(), m_bvh.nodeCount(), static_cast<double>(m_bvh.memoryBytes()) / kMiB,
                  m_bvh.hasNormalCones() ? "on" : "off");
    core::logInfo("Sculpt thread started with {} helper thread(s), {} gather, {} BVH leaf marking, {} edit region, {} dab preparation, {} Smooth binning, {} Move updates, undo/redo normals {} the lock, {} box refit, {} normal cones{}",
                  jobThreadCount(), m_stroke.parallelGather() ? "parallel" : "serial",
                  m_stroke.parallelMarking() ? "parallel" : "serial", m_stroke.parallelRegion() ? "parallel" : "serial",
                  m_stroke.overlapPrepare() ? "overlapped" : "serial",
                  sculpt::SmoothBrush::parallelBinning() ? "parallel" : "serial",
                  m_coalesceMoveUpdates ? "latest-wins" : "every one of the", m_historyNormalsBeforeLock ? "before" : "under",
                  m_bvh.denseRefit() ? "dense" : "incremental",
                  m_bvh.incrementalCones() ? std::format("incremental ({}% fallback)", config.coneFallbackPercent) : std::string("exact"),
                  m_bvh.reuseConeNormals() ? ", face normals reused in the cones' second pass" : "");

    m_sculptThread = std::thread([this] { sculptThreadMain(); });
}

AppController::~AppController()
{
    {
        const std::scoped_lock lock(m_queueMutex);
        m_stopping = true;
    }
    m_queueChanged.notify_all();
    m_sculptThread.join();
}

// --- Document ------------------------------------------------------------------

geo::Aabb AppController::bounds() const
{
    const std::scoped_lock lock(m_documentMutex);
    return m_bvh.bounds();
}

std::optional<geo::RayHit> AppController::pick(const geo::Ray& ray) const
{
    const std::scoped_lock lock(m_documentMutex);
    return m_bvh.raycast(m_mesh, ray);
}

// --- Tools ---------------------------------------------------------------------

void AppController::setActiveBrush(sculpt::BrushId id)
{
    if (m_brushes.contains(id) && !strokeActive()) {
        m_activeBrush = id;
    }
}

std::string_view AppController::activeBrushName() const
{
    return m_brushes.brush(m_activeBrush).name();
}

const sculpt::BrushSettings& AppController::activeSettings() const
{
    return m_brushes.settings(m_activeBrush);
}

void AppController::setRadius(float radius)
{
    m_brushes.settings(m_activeBrush).radius = std::clamp(radius, kMinRadius, kMaxRadius);
}

void AppController::setStrength(float strength)
{
    m_brushes.settings(m_activeBrush).strength = std::clamp(strength, 0.0f, 1.0f);
}

void AppController::setInvert(bool invert)
{
    m_brushes.settings(m_activeBrush).invert = invert;
}

// --- Client side of strokes and history -----------------------------------------

bool AppController::beginStroke(const geo::Ray& ray, StrokeModifiers modifiers)
{
    if (strokeActive()) {
        return false;
    }
    // The caller needs the answer now (sculpt or orbit?), so the hit test runs
    // here. The sculpt thread repeats it when it starts the stroke.
    const std::optional<geo::RayHit> hit = pick(ray);
    if (!hit) {
        return false;
    }

    Command command;
    command.type = Command::Type::BeginStroke;
    command.ray = ray;
    // Shift-smoothing keeps the active brush's radius so the cursor does not jump.
    command.settings = activeSettings();
    command.brush = m_activeBrush;
    if (modifiers.smooth) {
        command.brush = sculpt::BrushId::Smooth;
        command.settings.strength = m_brushes.settings(sculpt::BrushId::Smooth).strength;
    }
    command.settings.invert = command.settings.invert != modifiers.invert;

    m_strokeOpen = true;
    m_strokeRadius = command.settings.radius;
    {
        const std::scoped_lock lock(m_publishedMutex);
        m_published.strokeCenter = hit->position;
    }
    enqueue(command);
    return true;
}

void AppController::updateStroke(const geo::Ray& ray)
{
    if (!strokeActive()) {
        return;
    }
    Command command;
    command.type = Command::Type::UpdateStroke;
    command.ray = ray;
    enqueue(command);
}

void AppController::endStroke()
{
    if (!strokeActive()) {
        return;
    }
    m_strokeOpen = false;
    Command command;
    command.type = Command::Type::EndStroke;
    enqueue(command);
}

void AppController::undo()
{
    if (strokeActive()) {
        return;
    }
    Command command;
    command.type = Command::Type::Undo;
    enqueue(command);
}

void AppController::redo()
{
    if (strokeActive()) {
        return;
    }
    Command command;
    command.type = Command::Type::Redo;
    enqueue(command);
}

bool AppController::canUndo() const
{
    return !strokeActive() && undoCount() > 0;
}

bool AppController::canRedo() const
{
    return !strokeActive() && redoCount() > 0;
}

std::size_t AppController::undoCount() const
{
    const std::scoped_lock lock(m_publishedMutex);
    return m_published.undoCount;
}

std::size_t AppController::redoCount() const
{
    const std::scoped_lock lock(m_publishedMutex);
    return m_published.redoCount;
}

std::size_t AppController::undoMemoryBytes() const
{
    const std::scoped_lock lock(m_publishedMutex);
    return m_published.undoMemoryBytes;
}

glm::vec3 AppController::strokeCenter() const
{
    const std::scoped_lock lock(m_publishedMutex);
    return m_published.strokeCenter;
}

std::pair<u32, u32> AppController::checkNormalCones() const
{
    const std::scoped_lock lock(m_documentMutex);
    u32 violations = 0;
    u32 usable = 0;
    const u32 capacity = 1u << m_bvh.chunkTriangleShift();
    for (u32 chunk = 0; chunk < m_bvh.chunkCount(); ++chunk) {
        const geo::NormalCone& cone = m_bvh.chunkCone(chunk);
        const u32 first = chunk * capacity;
        const u32 count = std::min(capacity, m_mesh.triangleCount() - first);
        usable += cone.usable() ? 1u : 0u;
        violations += geo::coneContainsNormals(cone, m_mesh, first, count) ? 0u : 1u;
    }
    return {violations, usable};
}

InteractionStats AppController::stats() const
{
    InteractionStats result;
    {
        const std::scoped_lock lock(m_publishedMutex);
        result = m_published.stats;
    }
    const std::scoped_lock lock(m_queueMutex);
    result.queuedInputs = static_cast<u32>(m_queue.size());
    return result;
}

void AppController::drainDirtyVertices(geo::DirtyVertexSet& destination)
{
    const std::scoped_lock lock(m_documentMutex);
    destination.merge(m_dirty);
    m_dirty.clear();
}

std::size_t AppController::accelerationMemoryBytes() const
{
    const std::scoped_lock lock(m_documentMutex);
    return m_bvh.memoryBytes();
}

bool AppController::hasDirtyVertices() const
{
    const std::scoped_lock lock(m_documentMutex);
    return !m_dirty.empty();
}

// --- Synchronisation -----------------------------------------------------------------

namespace {

double steadyNowMs() noexcept
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void AppController::enqueue(const Command& command)
{
    {
        const std::scoped_lock lock(m_queueMutex);
        m_queue.push_back(command);
        m_queue.back().queuedAtMs = steadyNowMs();
    }
    m_queueChanged.notify_one();
}

void AppController::recomputeAllNormalsForReference()
{
    waitUntilIdle();
    const std::scoped_lock lock(m_documentMutex);
    m_mesh.recomputeNormals();
    m_dirty.markAll();
}

void AppController::waitUntilIdle()
{
    std::unique_lock lock(m_queueMutex);
    m_becameIdle.wait(lock, [this] { return m_queue.empty() && !m_working; });
}

void AppController::setChangeListener(std::function<void()> listener)
{
    // The sculpt thread holds this mutex while calling the listener, so once we
    // have it the previous listener is not running any more.
    const std::scoped_lock lock(m_listenerMutex);
    m_listener = std::move(listener);
}

void AppController::notifyChanged()
{
    const std::scoped_lock lock(m_listenerMutex);
    if (m_listener) {
        m_listener();
    }
}

// --- Sculpt thread ---------------------------------------------------------------------

void AppController::sculptThreadMain()
{
    for (;;) {
        Command command;
        bool haveCommand = false;
        {
            std::unique_lock lock(m_queueMutex);
            for (;;) {
                if (m_stopping) {
                    return;
                }
                if (!m_queue.empty()) {
                    command = m_queue.front();
                    m_queue.pop_front();
                    if (m_coalesceMoveUpdates && command.type == Command::Type::UpdateStroke && m_stroke.grabActive()) {
                        command = coalesceMoveUpdates(command);
                    }
                    haveCommand = true;
                    break;
                }
                if (m_optimizePending) {
                    break; // nothing queued: use the time to tidy the BVH
                }
                m_working = false;
                m_becameIdle.notify_all();
                m_queueChanged.wait(lock);
            }
            m_working = true;
        }

        if (haveCommand) {
            execute(command);
            notifyChanged();
        } else if (m_bvh.hasStaleCones()) {
            // Exact cones first: they restore culling. Computed without the lock
            // (this thread is the only writer), published under it.
            const core::Stopwatch rebuild;
            const u32 helpers = static_cast<u32>(jobThreadCount());
            m_bvh.computeStaleCones(m_mesh, kConeRebuildChunksPerThread * (helpers + 1), m_jobs.get());
            m_lastConeRebuildMs = rebuild.elapsedMs();
            lockDocument();
            m_bvh.publishStaleCones();
            m_optimizePending = m_bvh.hasStaleCones() || (!m_stroke.active() && m_bvh.needsOptimize());
            unlockDocument(HoldKind::ConeRebuild);
        } else {
            // One small step, then back to the queue: input always comes first.
            lockDocument();
            m_bvh.optimize(m_mesh, kOptimizeStepTriangles);
            m_optimizePending = m_bvh.needsOptimize() || m_bvh.hasStaleCones();
            unlockDocument(HoldKind::Optimize);
        }
    }
}

AppController::Command AppController::coalesceMoveUpdates(Command command)
{
    // Every queued command up to the next one that is not an update belongs to
    // the active stroke: a stroke ends with EndStroke, and undo, redo and the
    // next stroke are queued after it. Settings are not queued commands (a
    // stroke keeps the ones it began with). So only the run of updates right
    // behind this one is taken, and the stroke's end is still applied after it.
    m_coalescedBefore.clear();
    m_coalescedAfter.clear();
    if (m_queue.empty() || m_queue.front().type != Command::Type::UpdateStroke) {
        return command;
    }
    // Apply the newest update that changes anything. Updates the Grab rejects
    // (ray parallel to, or pointing away from, the drag plane) are no-ops, so
    // dropping those after it changes nothing either. If none applies, none
    // is applied: the last one is executed and does nothing, as each would.
    std::vector<Command> run;
    run.push_back(command);
    while (!m_queue.empty() && m_queue.front().type == Command::Type::UpdateStroke) {
        run.push_back(m_queue.front());
        m_queue.pop_front();
    }
    std::size_t chosen = run.size() - 1;
    for (std::size_t i = run.size(); i-- > 0;) {
        if (m_stroke.grabUpdateApplies(run[i].ray)) {
            chosen = i;
            break;
        }
    }
    for (std::size_t i = 0; i < run.size(); ++i) {
        if (i < chosen) {
            m_coalescedBefore.push_back(run[i].queuedAtMs);
        } else if (i > chosen) {
            m_coalescedAfter.push_back(run[i].queuedAtMs);
        }
    }
    return run[chosen];
}

void AppController::execute(const Command& command)
{
    const core::Stopwatch timer;
    const double startedAtMs = steadyNowMs();
    const u32 dabsBefore = m_stroke.active() ? m_stroke.stats().dabCount : 0;
    // Stroke commands take no lock here: every dab locks the document only for
    // its commit (see the commit hooks). History prepares its refit before taking the lock.
    switch (command.type) {
    case Command::Type::BeginStroke:
        if (!m_stroke.active()) {
            m_stroke.begin(m_brushes.brush(command.brush), command.settings, command.ray);
        }
        break;
    case Command::Type::UpdateStroke:
        m_stroke.update(command.ray);
        break;
    case Command::Type::EndStroke:
        if (std::optional<sculpt::StrokeDelta> delta = m_stroke.end()) {
            m_undo.push(std::move(*delta));
        }
        break;
    case Command::Type::Undo:
    case Command::Type::Redo:
        if (!m_stroke.active()) {
            const bool undoing = command.type == Command::Type::Undo;
            const auto vertices = undoing ? m_undo.undoVertices() : m_undo.redoVertices();
            if (vertices.empty()) {
                m_lastHistoryHoldMs = 0.0;
                break;
            }

            // The history cursor and refit scratch belong to the sculpt thread.
            core::Stopwatch stage;
            InteractionStats::HistoryProfile& profile = m_historyProfile;
            m_region.collect(m_mesh, vertices, m_stroke.parallelRegion() ? m_jobs.get() : nullptr);
            profile.regionMs = stage.elapsedMs();
            profile.parallelRegion = m_region.lastWasParallel();
            m_bvh.markTriangles(m_region.triangles(), m_stroke.parallelMarking() ? m_jobs.get() : nullptr);
            const std::span<const u32> affected = m_region.vertices();
            if (m_historyNormalsBeforeLock) {
                // The entry's positions over the mesh, in the dab staging pages:
                // moved vertices read the entry, all others the mesh.
                sculpt::PositionStaging& staging = m_stroke.staging();
                staging.stage(vertices);
                const std::span<const glm::vec3> values = undoing ? m_undo.undoPositions() : m_undo.redoPositions();
                const geo::PagedArrayView<glm::vec3> staged = staging.view();
                for (std::size_t i = 0; i < vertices.size(); ++i) {
                    staged[vertices[i]] = values[i];
                }
                m_historyNormals.resize(affected.size());
                const std::span<glm::vec3> normals(m_historyNormals);
                const geo::PagedArrayView<const glm::vec3> view = staged;
                core::parallelFor(m_jobs.get(), affected.size(), 4096, [&](std::size_t begin, std::size_t end) {
                    m_mesh.computeNormals(affected.subspan(begin, end - begin), view, normals.subspan(begin, end - begin));
                });
                staging.release();
            }
            profile.prepareMs = stage.elapsedMs();
            profile.vertices = static_cast<u32>(vertices.size());
            profile.affected = static_cast<u32>(affected.size());
            profile.triangles = static_cast<u32>(m_region.triangles().size());

            lockDocument();
            stage.reset();
            if (undoing) {
                m_undo.undo(m_mesh);
            } else {
                m_undo.redo(m_mesh);
            }
            profile.positionsMs = stage.elapsedMs();
            stage.reset();
            for (const u32 vertex : affected) {
                m_dirty.mark(vertex);
            }
            m_mesh.invalidatePageBounds(m_dirty);
            profile.dirtyMs = stage.elapsedMs();
            stage.reset();
            if (m_historyNormalsBeforeLock) {
                const auto meshNormals = m_mesh.mutableNormals();
                core::parallelFor(m_jobs.get(), affected.size(), 4096, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        meshNormals[affected[i]] = m_historyNormals[i];
                    }
                });
            } else {
                // NormalUpdater::updateVertices without its dirty marking (above).
                std::atomic<double> cpuMs{0.0};
                core::parallelFor(m_jobs.get(), affected.size(), 4096, [this, affected, &cpuMs](std::size_t begin, std::size_t end) {
                    const core::Stopwatch task;
                    m_mesh.recomputeNormals(affected.subspan(begin, end - begin));
                    cpuMs.fetch_add(task.elapsedMs(), std::memory_order_relaxed);
                });
                profile.normalsCpuMs = cpuMs.load(std::memory_order_relaxed);
            }
            profile.normalsMs = stage.elapsedMs();
            m_bvh.finishMarkedRefit(m_mesh, m_jobs.get());
            profile.refitMs = m_bvh.lastRefitTimings().localMs;
            profile.boxCpuMs = m_bvh.lastRefitTimings().boxCpuMs;
            profile.coneCpuMs = m_bvh.lastRefitTimings().coneCpuMs;
            profile.chunks = m_bvh.lastRefitTimings().chunks;
            profile.coneTriangles = m_bvh.lastConeTriangles();
            profile.refit = m_bvh.lastRefitTimings();
            profile.topMs = m_bvh.lastRefitTimings().topMs;
            unlockDocument(HoldKind::History);
        }
        break;
    }

    // The BVH was only refitted. Its touched blocks are re-partitioned in the
    // background once the stroke is over (see sculptThreadMain).
    m_optimizePending = (!m_stroke.active() && m_bvh.needsOptimize()) || m_bvh.hasStaleCones();
    if (command.type == Command::Type::BeginStroke || command.type == Command::Type::UpdateStroke) {
        // Dropped updates (coalesceMoveUpdates) keep their place in queue
        // order: those before the applied one were dropped when it started,
        // those after it (no-ops) when it finished.
        const auto dropped = [this](double queuedAtMs, double atMs) {
            InteractionStats::InputSample sample;
            sample.queuedAtMs = queuedAtMs;
            sample.startedAtMs = atMs;
            sample.doneAtMs = atMs;
            sample.coalesced = true;
            m_pendingInputs.push_back(sample);
        };
        for (const double queuedAtMs : m_coalescedBefore) {
            dropped(queuedAtMs, startedAtMs);
        }
        InteractionStats::InputSample sample;
        sample.queuedAtMs = command.queuedAtMs;
        sample.startedAtMs = startedAtMs;
        sample.dabs = m_stroke.active() ? m_stroke.stats().dabCount - (command.type == Command::Type::BeginStroke ? 0 : dabsBefore) : 0;
        sample.doneAtMs = steadyNowMs();
        if (sample.dabs > 0) {
            sample.commitSeq = m_commitSeq; // written only on this thread
            sample.committedAtMs = m_lastCommitMs;
        }
        m_pendingInputs.push_back(sample);
        for (const double queuedAtMs : m_coalescedAfter) {
            dropped(queuedAtMs, sample.doneAtMs);
        }
        m_coalescedTotal += static_cast<u32>(m_coalescedBefore.size() + m_coalescedAfter.size());
        m_coalescedBefore.clear();
        m_coalescedAfter.clear();
    }
    publish(command, timer.elapsedMs());
    if (command.type != Command::Type::BeginStroke && command.type != Command::Type::UpdateStroke) {
        measureConeLoss(); // after the command's timing; before any background cone rebuild
    }
}

void AppController::measureConeLoss()
{
    if (!m_measureConeLoss) {
        return;
    }
    // This thread is the only writer: reading needs no lock.
    const geo::ChunkedBvh::ConeLoss loss = m_bvh.measureConeLoss(m_mesh, m_coneLossEyes);
    const std::scoped_lock lock(m_publishedMutex);
    m_published.stats.lastConeLoss = loss;
}

void AppController::lockDocument()
{
    m_documentMutex.lock();
    m_documentLocked = true;
    m_holdTimer.reset();
}

void AppController::unlockDocument(HoldKind kind)
{
    const double heldMs = m_holdTimer.elapsedMs();
    m_documentLocked = false;
    m_documentMutex.unlock();
    switch (kind) {
    case HoldKind::Dab:
        if (m_pendingHoldsMs.size() < 100000) {
            m_pendingHoldsMs.push_back(static_cast<float>(heldMs));
        }
        break;
    case HoldKind::History:
        m_lastHistoryHoldMs = heldMs;
        break;
    case HoldKind::Optimize: {
        const std::scoped_lock lock(m_publishedMutex);
        m_published.stats.worstOptimizeHoldMs = std::max(m_published.stats.worstOptimizeHoldMs, heldMs);
        ++m_published.stats.optimizeSteps;
        break;
    }
    case HoldKind::ConeRebuild: {
        const std::scoped_lock lock(m_publishedMutex);
        InteractionStats& stats = m_published.stats;
        stats.worstConeRebuildHoldMs = std::max(stats.worstConeRebuildHoldMs, heldMs);
        stats.worstConeRebuildMs = std::max(stats.worstConeRebuildMs, m_lastConeRebuildMs);
        stats.coneRebuildMs += m_lastConeRebuildMs;
        ++stats.coneRebuildSteps;
        stats.coneRebuildChunks += m_bvh.lastPublishedCones();
        break;
    }
    }
}

void AppController::publish(const Command& command, double elapsedMs)
{
    const std::scoped_lock lock(m_publishedMutex);
    InteractionStats& stats = m_published.stats;
    switch (command.type) {
    case Command::Type::BeginStroke:
        stats.lastUpdateMs = elapsedMs;
        stats.worstUpdateMs = elapsedMs;
        stats.updateSamplesMs.clear();
        stats.updateSamplesMs.push_back(static_cast<float>(elapsedMs));
        stats.lockHoldSamplesMs.clear();
        stats.dabSamplesMs.clear();
        stats.inputSamples.clear();
        stats.strokeCoalescedInputs = 0;
        break;
    case Command::Type::UpdateStroke:
        stats.lastUpdateMs = elapsedMs;
        stats.worstUpdateMs = std::max(stats.worstUpdateMs, elapsedMs);
        if (stats.updateSamplesMs.size() < 100000) {
            stats.updateSamplesMs.push_back(static_cast<float>(elapsedMs));
        }
        break;
    case Command::Type::EndStroke:
        stats.lastStrokeEndMs = elapsedMs;
        break;
    case Command::Type::Undo:
    case Command::Type::Redo:
        stats.lastUndoRedoMs = elapsedMs;
        stats.lastUndoRedoHoldMs = m_lastHistoryHoldMs;
        stats.lastHistory = m_historyProfile;
        break;
    }
    m_stroke.takeDabSamples(m_pendingDabsMs);
    const auto append = [](std::vector<float>& to, std::vector<float>& from) {
        const std::size_t room = to.size() < 100000 ? 100000 - to.size() : 0;
        to.insert(to.end(), from.begin(), from.begin() + static_cast<std::ptrdiff_t>(std::min(room, from.size())));
        from.clear();
    };
    append(stats.lockHoldSamplesMs, m_pendingHoldsMs);
    append(stats.dabSamplesMs, m_pendingDabsMs);
    if (stats.inputSamples.size() < 100000) {
        stats.inputSamples.insert(stats.inputSamples.end(), m_pendingInputs.begin(), m_pendingInputs.end());
    }
    m_pendingInputs.clear();
    stats.strokeCoalescedInputs += m_coalescedTotal;
    stats.coalescedInputs += m_coalescedTotal;
    m_coalescedTotal = 0;
    stats.editScratchBytes = m_stroke.editScratchBytes() + m_historyNormals.capacity() * sizeof(glm::vec3) +
                             geo::EditRegion::parallelStampBytes();
    if (m_stroke.active()) {
        stats.lastDabVertexCount = m_stroke.stats().lastDabVertexCount;
        stats.strokeDabCount = m_stroke.stats().dabCount;
        stats.strokeBvhMs = m_stroke.stats().bvhMs;
        stats.strokeNormalsMs = m_stroke.stats().normalsMs;
        stats.strokeBrushMs = m_stroke.stats().brushMs;
        stats.strokeConeTriangles = m_stroke.stats().coneTriangles;
        stats.strokeRefit = m_stroke.stats().refit;
        stats.strokeStages = m_stroke.stats();
        m_published.strokeCenter = m_stroke.lastCenter();
    }
    m_published.undoCount = m_undo.undoCount();
    m_published.redoCount = m_undo.redoCount();
    m_published.undoMemoryBytes = m_undo.memoryBytes();
}

} // namespace se::app
