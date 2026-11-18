// Headless measurements of the geometry and sculpt hot paths. No GPU required.
//
//   se_bench [firstLevel] [lastLevel] [helperThreads] [sweep]
//
//   firstLevel..lastLevel  icosphere subdivision levels (default 6 8)
//   helperThreads          helper threads for the parallel runs (default: from the hardware)
//   sweep                  also compare vertex page and chunk sizes at lastLevel
//
//   se_bench adjacency <level> [helperThreads] [wide|compact] [stored|derived] [pgather] [pmark]
//
//   se_bench live <level> [helperThreads] [rounds] [pgather] [pmark]
//   se_bench history <level> [helperThreads] [cycles] [pgather] [pmark] [hstage]
//
//   Draw, Smooth and Move strokes through AppController with inputs every 2 ms
//   and a reader thread taking the document lock every 4 ms, like the renderer.
//   Prints the sculpt thread's lock holds, the reader's waits, total dab and
//   input latency (p50, p95, max), the input backlog and the per-dab scratch.
//
//   One mesh in a fresh process: construction time and peak heap, memory by
//   kind (adjacency split into neighbours and incident triangles), then Draw,
//   Smooth and Move strokes with a hash of the positions after each, so runs
//   can be compared for bit-identical results. helperThreads 0 = one thread.
//
// Every level is measured on one thread and with the job system, then through
// AppController (the asynchronous path the editor uses).
// Numbers are wall-clock and only meaningful for Release builds.

#include "AllocTracker.h"

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Core/Timer.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/EditRegion.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace se;

namespace {

constexpr double kMiB = 1024.0 * 1024.0;
/// "dense" anywhere on the command line: ChunkedBvh::setDenseRefit for every mode.
bool g_denseRefit = false;
/// "icones" anywhere: incremental normal cones (AppConfig::incrementalCones);
/// "cfb=N": their fallback percentage; "closs": measure the extra triangles the
/// widened cones let through (AppConfig::measureConeLoss); "r=R": dab radius of
/// the adjacency and live strokes (default 0.25).
bool g_incrementalCones = false;
/// "creuse": exact cones reuse the first pass's face normals (AppConfig::reuseConeNormals).
bool g_reuseConeNormals = false;
u32 g_coneFallbackPercent = 50;
/// "pregion": walk edit regions on the helper threads (AppConfig::parallelRegion).
bool g_parallelRegion = false;
bool g_measureConeLoss = false;
float g_dabRadius = 0.25f;

void applyConeOptions(app::AppConfig& config)
{
    config.denseBoxRefit = g_denseRefit;
    config.incrementalCones = g_incrementalCones;
    config.coneFallbackPercent = g_coneFallbackPercent;
    config.measureConeLoss = g_measureConeLoss;
    config.reuseConeNormals = g_reuseConeNormals;
    config.parallelRegion = g_parallelRegion;
}

/// Extra triangles drawn per eye, and as a share of the exact cones' count.
void printConeLoss(const char* label, double extraSum, double exactSum, double staleSum, double disabledSum, double n)
{
    std::printf("    %s cone loss: %.0f extra triangles drawn per eye (%+.3f%% of %.0f), %.1f stale chunks (%.1f with culling off) per measure\n",
                label, extraSum / n, exactSum > 0.0 ? 100.0 * extraSum / exactSum : 0.0, exactSum / n, staleSum / n,
                disabledSum / n);
}

geo::Ray orbitRay(float angle, float height)
{
    const glm::vec3 origin(4.0f * std::cos(angle), height, 4.0f * std::sin(angle));
    return geo::Ray{origin, glm::normalize(glm::vec3(0.0f, height * 0.9f, 0.0f) - origin)};
}

struct StrokeResult {
    u32 edits = 0;
    double msPerEdit = 0.0;
    double worstSampleMs = 0.0;
    u32 verticesPerEdit = 0;
    double uploadVerticesPerFrame = 0.0;
    double uploadPagesPerFrame = 0.0;
    double optimizeMs = 0.0;
    u32 optimizedTriangles = 0;
    double undoMiB = 0.0;
    sculpt::StrokeStats stages;
    geo::ChunkedBvh::ConeLoss coneLoss; ///< at the end of the stroke, before the rebuild
    u32 staleCones = 0;
    double coneRebuildMs = 0.0;         ///< exact rebuild of every stale cone, serial
};

/// Fixed eyes around the mesh for cone-loss measurements: 6 axis and 8
/// diagonal directions, 1.5 diagonals from the centre (as AppController).
std::vector<glm::vec3> coneLossEyes(const geo::ChunkedBvh& bvh)
{
    std::vector<glm::vec3> eyes;
    const geo::Aabb box = bvh.bounds();
    const float distance = 1.5f * glm::length(box.max - box.min);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                const int nonzero = (x != 0) + (y != 0) + (z != 0);
                if (nonzero == 1 || nonzero == 3) {
                    eyes.push_back(box.center() + distance * glm::normalize(glm::vec3(x, y, z)));
                }
            }
        }
    }
    return eyes;
}

/// One stroke of 200 pointer samples around the equator.
StrokeResult runStroke(geo::Mesh& mesh, geo::ChunkedBvh& bvh, sculpt::StrokeProcessor& stroke, const sculpt::Brush& brush)
{
    const sculpt::BrushSettings settings{.radius = g_dabRadius, .strength = 0.5f, .invert = false};
    constexpr int kSamples = 200;
    StrokeResult result;
    geo::DirtyVertexSet dirty = mesh.makeDirtySet();
    u64 uploadVertices = 0;
    u64 uploadPages = 0;
    u32 uploads = 0;

    const core::Stopwatch strokeTimer;
    if (!stroke.begin(brush, settings, orbitRay(0.0f, 0.0f))) {
        return result;
    }
    for (int i = 1; i <= kSamples; ++i) {
        const core::Stopwatch sampleTimer;
        stroke.update(orbitRay(0.01f * static_cast<float>(i), 0.0f));
        result.worstSampleMs = std::max(result.worstSampleMs, sampleTimer.elapsedMs());
        // What a renderer would have to copy if it drew a frame after this sample.
        stroke.drainDirty(dirty);
        if (!dirty.empty()) {
            uploadVertices += dirty.dirtyVertexCount();
            uploadPages += dirty.dirtyPageCount();
            ++uploads;
            dirty.clear();
        }
    }
    result.stages = stroke.stats();
    result.edits = result.stages.dabCount;
    result.verticesPerEdit = result.stages.lastDabVertexCount;
    result.msPerEdit = strokeTimer.elapsedMs() / std::max(result.edits, 1u);
    const auto delta = stroke.end();
    result.undoMiB = delta ? static_cast<double>(delta->memoryBytes()) / kMiB : 0.0;
    result.uploadVerticesPerFrame = static_cast<double>(uploadVertices) / std::max(uploads, 1u);
    result.uploadPagesPerFrame = static_cast<double>(uploadPages) / std::max(uploads, 1u);

    if (bvh.incrementalCones()) {
        if (g_measureConeLoss) {
            result.coneLoss = bvh.measureConeLoss(mesh, coneLossEyes(bvh));
        }
        result.staleCones = bvh.staleConeCount();
        const core::Stopwatch rebuildTimer;
        while (bvh.hasStaleCones()) {
            bvh.computeStaleCones(mesh, 1u << 20);
            bvh.publishStaleCones();
        }
        result.coneRebuildMs = rebuildTimer.elapsedMs();
    }
    const core::Stopwatch optimizeTimer;
    result.optimizedTriangles = bvh.optimize(mesh);
    result.optimizeMs = optimizeTimer.elapsedMs();
    return result;
}

struct LevelResult {
    double meshMs = 0.0;
    double bvhBuildMs = 0.0;
    double bvhParallelBuildMs = 0.0;
    double refitMs = 0.0;
    double rayUs = 0.0;
    double meshMiB = 0.0;
    double bvhMiB = 0.0;
    u32 vertexPages = 0;
    u32 chunks = 0;
    StrokeResult draw;
    StrokeResult smooth;
    StrokeResult move;
};

LevelResult measure(int level, const geo::MeshLayout& layout, u32 chunkShift, core::JobSystem* jobs, core::JobSystem* buildJobs,
                    bool verbose)
{
    LevelResult result;
    core::Stopwatch timer;
    geo::Mesh mesh = geo::makeIcosphere(level, 1.0f, geo::VertexOrder::Spatial, layout);
    result.meshMs = timer.elapsedMs();

    geo::ChunkedBvh bvh;
    timer.reset();
    bvh.build(mesh, chunkShift);
    result.bvhBuildMs = timer.elapsedMs();
    if (buildJobs != nullptr) {
        geo::ChunkedBvh parallel;
        timer.reset();
        parallel.build(mesh, chunkShift, buildJobs);
        result.bvhParallelBuildMs = timer.elapsedMs();
    }

    timer.reset();
    constexpr int kRefits = 10;
    for (int i = 0; i < kRefits; ++i) {
        bvh.refit(mesh);
    }
    result.refitMs = timer.elapsedMs() / kRefits;
    // A full refit queues every chunk for optimisation; clear that so the
    // stroke measurements below only see what the strokes themselves touch.
    bvh.optimize(mesh);

    std::mt19937 rng(1);
    std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> height(-0.8f, 0.8f);
    constexpr int kRays = 20000;
    int hits = 0;
    timer.reset();
    for (int i = 0; i < kRays; ++i) {
        hits += bvh.raycast(mesh, orbitRay(angle(rng), height(rng))).has_value() ? 1 : 0;
    }
    result.rayUs = timer.elapsedMs() * 1000.0 / kRays;
    if (hits != kRays) {
        std::printf("  WARNING: only %d of %d rays hit\n", hits, kRays);
    }

    result.vertexPages = mesh.vertexPageCount();
    result.chunks = bvh.chunkCount();

    const sculpt::BrushLibrary brushes;
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(jobs);
    result.draw = runStroke(mesh, bvh, stroke, brushes.brush(sculpt::BrushId::Draw));
    result.smooth = runStroke(mesh, bvh, stroke, brushes.brush(sculpt::BrushId::Smooth));
    result.move = runStroke(mesh, bvh, stroke, brushes.brush(sculpt::BrushId::Move));

    result.meshMiB = static_cast<double>(mesh.memoryBytes()) / kMiB;
    result.bvhMiB = static_cast<double>(bvh.memoryBytes()) / kMiB;

    if (verbose) {
        const geo::MeshMemory memory = mesh.memory();
        std::printf("level %d: %u triangles, %u vertices\n", level, mesh.triangleCount(), mesh.vertexCount());
        std::printf("  layout: %u vertex pages of %u, %u triangle pages of %u, %u chunks of %u triangles\n",
                    mesh.vertexPageCount(), layout.vertexPageCapacity(), mesh.trianglePageCount(),
                    layout.trianglePageCapacity(), bvh.chunkCount(), 1u << chunkShift);
        std::printf("  memory: mesh %.1f MiB (positions %.1f, normals %.1f, triangles %.1f, adjacency %.1f), BVH %.1f MiB\n",
                    result.meshMiB, static_cast<double>(memory.positions) / kMiB, static_cast<double>(memory.normals) / kMiB,
                    static_cast<double>(memory.triangles) / kMiB, static_cast<double>(memory.adjacency) / kMiB,
                    result.bvhMiB);
        std::printf("  construction: mesh (generate + order + page) %.1f ms, BVH %.1f ms on one thread", result.meshMs,
                    result.bvhBuildMs);
        if (buildJobs != nullptr) {
            std::printf(", %.1f ms with %u helpers", result.bvhParallelBuildMs, buildJobs->workerCount());
        }
        std::printf("\n  BVH full refit %.3f ms, raycast %.3f us/ray\n", result.refitMs, result.rayUs);
    }
    return result;
}

void printStroke(const char* name, const StrokeResult& s, u32 vertexCount)
{
    std::printf("    %-6s %4u edits, %7.3f ms/edit, worst sample %7.3f ms, ~%u verts/edit, undo %.2f MiB\n", name, s.edits,
                s.msPerEdit, s.worstSampleMs, s.verticesPerEdit, s.undoMiB);
    const double edits = std::max(s.edits, 1u);
    std::printf("           per edit: gather %.3f, brush %.3f, normals %.3f, publish %.3f, bvh %.3f ms\n", s.stages.gatherMs / edits,
                s.stages.brushMs / edits, s.stages.normalsMs / edits, s.stages.publishMs / edits, s.stages.bvhMs / edits);
    std::printf("           upload %.0f verts/frame in %.1f pages (%.1f%% of mesh), stroke-end optimize %.2f ms (%u triangles)\n",
                s.uploadVerticesPerFrame, s.uploadPagesPerFrame, 100.0 * s.uploadVerticesPerFrame / vertexCount, s.optimizeMs,
                s.optimizedTriangles);
}

void runLevel(int level, core::JobSystem& jobs)
{
    const geo::MeshLayout layout;
    const u32 chunkShift = geo::ChunkedBvh::kDefaultChunkTriangleShift;
    const u32 vertexCount = static_cast<u32>(geo::icosphereVertexCount(level));

    const LevelResult serial = measure(level, layout, chunkShift, nullptr, &jobs, true);
    std::printf("  strokes on one thread:\n");
    printStroke("Draw", serial.draw, vertexCount);
    printStroke("Smooth", serial.smooth, vertexCount);
    printStroke("Move", serial.move, vertexCount);

    const LevelResult parallel = measure(level, layout, chunkShift, &jobs, nullptr, false);
    std::printf("  strokes with %u helper thread(s):\n", jobs.workerCount());
    printStroke("Draw", parallel.draw, vertexCount);
    printStroke("Smooth", parallel.smooth, vertexCount);
    printStroke("Move", parallel.move, vertexCount);
    std::printf("\n");
}

/// The asynchronous path as the editor uses it: one thread plays the UI
/// (queues a long stroke, then takes the mesh lock once per "frame" like the
/// renderer), while the controller's sculpt thread does the work.
void runResponsiveness(int level, int helperThreads)
{
    app::AppConfig config;
    config.sphereSubdivisions = level;
    config.jobThreads = helperThreads;
    const core::Stopwatch startup;
    app::AppController controller(config);
    const double startupMs = startup.elapsedMs();
    controller.setRadius(0.25f);

    core::Stopwatch timer;
    constexpr int kSamples = 600;
    if (!controller.beginStroke(orbitRay(0.0f, 0.0f))) {
        return;
    }
    for (int i = 1; i <= kSamples; ++i) {
        controller.updateStroke(orbitRay(0.005f * static_cast<float>(i), 0.0f));
    }
    controller.endStroke();
    const double queueMs = timer.elapsedMs();

    // "Frames" every 4 ms until the sculpt thread has finished the stroke.
    geo::DirtyVertexSet frameDirty = controller.mesh().makeDirtySet();
    double worstWaitMs = 0.0;
    double totalWaitMs = 0.0;
    int frames = 0;
    u32 maxQueued = 0;
    while (controller.stats().queuedInputs > 0 || controller.undoCount() == 0) {
        maxQueued = std::max(maxQueued, controller.stats().queuedInputs);
        const core::Stopwatch waitTimer;
        {
            const std::scoped_lock lock(controller.documentMutex());
            const double waited = waitTimer.elapsedMs();
            worstWaitMs = std::max(worstWaitMs, waited);
            totalWaitMs += waited;
            frameDirty.merge(controller.pendingVertexUpdates());
            controller.pendingVertexUpdates().clear();
        }
        frameDirty.clear();
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const double strokeMs = timer.elapsedMs();
    timer.reset();
    controller.waitUntilIdle();
    const double tidyMs = timer.elapsedMs();
    const app::InteractionStats stats = controller.stats();

    std::printf("level %d through AppController, %u helper thread(s):\n", level, controller.jobThreadCount());
    std::printf("  startup (mesh + acceleration structure) %.1f ms\n", startupMs);
    std::printf("  queuing %d inputs took %.3f ms on the caller (%.4f ms each)\n", kSamples + 2, queueMs,
                queueMs / (kSamples + 2));
    std::printf("  sculpt thread: %u dabs in %.1f ms, worst input %.3f ms, up to %u inputs queued\n", stats.strokeDabCount,
                strokeMs, stats.worstUpdateMs, maxQueued);
    std::printf("  reader: %d lock acquisitions, mean wait %.4f ms, worst wait %.3f ms\n", frames,
                totalWaitMs / std::max(frames, 1), worstWaitMs);
    std::printf("  background BVH tidy after the stroke: %.1f ms (does not block input or readers)\n\n", tidyMs);
}

/// Sleeps until `deadline` with sub-millisecond accuracy (the default Windows
/// sleep granularity would turn 2 ms intervals into 15 ms ones).
class Pacer {
public:
    Pacer()
    {
#ifdef _WIN32
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    }
    ~Pacer()
    {
#ifdef _WIN32
        if (m_timer != nullptr) {
            CloseHandle(m_timer);
        }
#endif
    }
    Pacer(const Pacer&) = delete;
    Pacer& operator=(const Pacer&) = delete;

    void sleepUntil(std::chrono::steady_clock::time_point deadline)
    {
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now) {
            return;
        }
#ifdef _WIN32
        if (m_timer != nullptr) {
            const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() / 100;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(std::max<long long>(ticks, 1));
            SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(m_timer, INFINITE);
            return;
        }
#endif
        std::this_thread::sleep_until(deadline);
    }

private:
#ifdef _WIN32
    HANDLE m_timer = nullptr;
#endif
};

struct Spread {
    float p50 = 0.0f;
    float p95 = 0.0f;
    float max = 0.0f;
    std::size_t count = 0;
};

Spread spreadOf(std::vector<float> samples)
{
    Spread result;
    result.count = samples.size();
    if (samples.empty()) {
        return result;
    }
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) { return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1) + 0.5)]; };
    result.p50 = at(0.5);
    result.p95 = at(0.95);
    result.max = samples.back();
    return result;
}

void printSpread(const char* label, const Spread& s)
{
    std::printf("    %-34s p50 %8.3f  p95 %8.3f  max %8.3f ms  (n %zu)\n", label, s.p50, s.p95, s.max, s.count);
}

/// One stroke through AppController the way the editor drives it: inputs
/// arrive every `inputIntervalMs` on this thread while a second thread plays
/// the renderer, taking the document lock every `frameIntervalMs` to collect
/// the dirty vertices. Reports what the sculpt thread held the lock for, what
/// the reader waited, the total latency of each dab and of each input, and the
/// input backlog.
void runLiveStroke(app::AppController& controller, sculpt::BrushId brush, int inputs, float anglePerInput,
                   double inputIntervalMs, double frameIntervalMs)
{
    using Clock = std::chrono::steady_clock;
    controller.waitUntilIdle();
    controller.setActiveBrush(brush);
    controller.setRadius(g_dabRadius);
    const app::InteractionStats before = controller.stats();

    std::atomic<bool> stop{false};
    std::vector<float> waits;
    waits.reserve(1u << 16);
    std::thread reader([&] {
        Pacer pacer;
        geo::DirtyVertexSet frameDirty = controller.mesh().makeDirtySet();
        auto next = Clock::now();
        while (!stop.load(std::memory_order_relaxed)) {
            const core::Stopwatch waitTimer;
            {
                const std::scoped_lock lock(controller.documentMutex());
                waits.push_back(static_cast<float>(waitTimer.elapsedMs()));
                frameDirty.merge(controller.pendingVertexUpdates());
                controller.pendingVertexUpdates().clear();
            }
            frameDirty.clear();
            next += std::chrono::microseconds(static_cast<long long>(frameIntervalMs * 1000.0));
            pacer.sleepUntil(next);
        }
    });

    Pacer pacer;
    u32 maxQueued = 0;
    const core::Stopwatch strokeTimer;
    auto next = Clock::now();
    const bool grab = brush == sculpt::BrushId::Move;
    if (!controller.beginStroke(orbitRay(0.0f, 0.0f))) {
        stop = true;
        reader.join();
        std::printf("    stroke missed the mesh\n");
        return;
    }
    for (int i = 1; i <= inputs; ++i) {
        next += std::chrono::microseconds(static_cast<long long>(inputIntervalMs * 1000.0));
        pacer.sleepUntil(next);
        // Move drags a little further every input; dab brushes travel along the equator.
        controller.updateStroke(grab ? orbitRay(0.0005f * static_cast<float>(i), 0.0f)
                                     : orbitRay(anglePerInput * static_cast<float>(i), 0.0f));
        maxQueued = std::max(maxQueued, controller.stats().queuedInputs);
    }
    const u32 queuedAtRelease = controller.stats().queuedInputs;
    const double feedMs = strokeTimer.elapsedMs();
    controller.endStroke();
    while (controller.undoCount() == 0 || controller.stats().queuedInputs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double strokeMs = strokeTimer.elapsedMs();
    stop = true;
    reader.join();
    controller.waitUntilIdle();
    const app::InteractionStats stats = controller.stats();

    std::printf("  %s: %d inputs every %.1f ms (fed in %.0f ms), %u dabs, stroke done after %.0f ms; inputs queued max %u, at release %u; scratch %.1f MiB\n",
                std::string(controller.activeBrushName()).c_str(), inputs, inputIntervalMs, feedMs, stats.strokeDabCount,
                strokeMs, maxQueued, queuedAtRelease, static_cast<double>(stats.editScratchBytes) / kMiB);
    printSpread("lock hold per dab", spreadOf(stats.lockHoldSamplesMs));
    printSpread("reader wait (every frame)", spreadOf(waits));
    printSpread("dab latency (gather to publish)", spreadOf(stats.dabSamplesMs));
    printSpread("input latency (sculpt thread)", spreadOf(stats.updateSamplesMs));
    std::printf("    background BVH steps %llu, longest hold %.3f ms\n", static_cast<unsigned long long>(stats.optimizeSteps),
                stats.worstOptimizeHoldMs);
    std::printf("    cones per dab: CPU %.4f ms (boxes %.4f), %.2f chunks: %.2f exact, %.2f widened, %.2f culling off\n",
                stats.strokeRefit.coneCpuMs / std::max(stats.strokeDabCount, 1u),
                stats.strokeRefit.boxCpuMs / std::max(stats.strokeDabCount, 1u),
                static_cast<double>(stats.strokeRefit.chunks) / std::max(stats.strokeDabCount, 1u),
                static_cast<double>(stats.strokeRefit.conesExact) / std::max(stats.strokeDabCount, 1u),
                static_cast<double>(stats.strokeRefit.conesWidened) / std::max(stats.strokeDabCount, 1u),
                static_cast<double>(stats.strokeRefit.conesDisabled) / std::max(stats.strokeDabCount, 1u));
    std::printf("    cone rebuild this stroke: %llu steps, %llu chunks, %.3f ms compute; worst step %.3f ms, worst publish hold %.4f ms (since startup)\n",
                static_cast<unsigned long long>(stats.coneRebuildSteps - before.coneRebuildSteps),
                static_cast<unsigned long long>(stats.coneRebuildChunks - before.coneRebuildChunks),
                stats.coneRebuildMs - before.coneRebuildMs, stats.worstConeRebuildMs, stats.worstConeRebuildHoldMs);
    if (stats.lastConeLoss.eyes > 0) {
        const auto& loss = stats.lastConeLoss;
        printConeLoss("stroke end", (static_cast<double>(loss.drawnTriangles) - static_cast<double>(loss.drawnTrianglesExact)),
                      static_cast<double>(loss.drawnTrianglesExact), static_cast<double>(loss.staleChunks) * loss.eyes,
                      static_cast<double>(loss.disabledChunks) * loss.eyes, loss.eyes);
    }
}


/// Undo and redo through AppController while a reader thread takes the
/// document lock every `frameIntervalMs` like the renderer: `cycles` times
/// undo of the last `steps` strokes, then redo of them. Reports lock hold,
/// command latency (sculpt thread) and reader wait, and the hold by stage.
u64 hashMesh(const geo::Mesh& mesh);

void runHistoryCycles(app::AppController& controller, const char* label, int steps, int cycles, double frameIntervalMs)
{
    using Clock = std::chrono::steady_clock;
    controller.waitUntilIdle();
    std::atomic<bool> stop{false};
    std::vector<float> waits;
    waits.reserve(1u << 16);
    std::thread reader([&] {
        Pacer pacer;
        geo::DirtyVertexSet frameDirty = controller.mesh().makeDirtySet();
        auto next = Clock::now();
        while (!stop.load(std::memory_order_relaxed)) {
            const core::Stopwatch waitTimer;
            {
                const std::scoped_lock lock(controller.documentMutex());
                waits.push_back(static_cast<float>(waitTimer.elapsedMs()));
                frameDirty.merge(controller.pendingVertexUpdates());
                controller.pendingVertexUpdates().clear();
            }
            frameDirty.clear();
            next += std::chrono::microseconds(static_cast<long long>(frameIntervalMs * 1000.0));
            pacer.sleepUntil(next);
        }
    });

    struct Samples {
        std::vector<float> hold, total, prepare, positions, normals, dirty, refit, top, boxCpu, coneCpu, leafCpu, innerCpu,
            queueCpu, chunkMax, chunkMean;
        double vertices = 0.0, affected = 0.0, triangles = 0.0, chunks = 0.0, coneTriangles = 0.0, leaves = 0.0,
               leafTriangles = 0.0, unchangedLeaves = 0.0, inner = 0.0, unchangedInner = 0.0, denseChunks = 0.0;
        double conesExact = 0.0, conesWidened = 0.0, conesDisabled = 0.0, widenTriangles = 0.0, pass2Triangles = 0.0;
        std::vector<float> pass1, pass2, normalsCpu, region;
        double parallelRegions = 0.0;
        double lossExtra = 0.0, lossExact = 0.0, lossStale = 0.0, lossDisabled = 0.0, lossEyes = 0.0;
    } undo, redo;
    const app::InteractionStats before = controller.stats();
    const auto record = [&](Samples& s) {
        controller.waitUntilIdle();
        const app::InteractionStats stats = controller.stats();
        const auto& h = stats.lastHistory;
        s.hold.push_back(static_cast<float>(stats.lastUndoRedoHoldMs));
        s.total.push_back(static_cast<float>(stats.lastUndoRedoMs));
        s.prepare.push_back(static_cast<float>(h.prepareMs));
        s.positions.push_back(static_cast<float>(h.positionsMs));
        s.normals.push_back(static_cast<float>(h.normalsMs));
        s.normalsCpu.push_back(static_cast<float>(h.normalsCpuMs));
        s.region.push_back(static_cast<float>(h.regionMs));
        s.parallelRegions += h.parallelRegion ? 1.0 : 0.0;
        s.dirty.push_back(static_cast<float>(h.dirtyMs));
        s.refit.push_back(static_cast<float>(h.refitMs));
        s.top.push_back(static_cast<float>(h.topMs));
        s.boxCpu.push_back(static_cast<float>(h.boxCpuMs));
        s.coneCpu.push_back(static_cast<float>(h.coneCpuMs));
        s.chunks += h.chunks;
        s.coneTriangles += h.coneTriangles;
        s.leafCpu.push_back(static_cast<float>(h.refit.leafCpuMs));
        s.innerCpu.push_back(static_cast<float>(h.refit.innerCpuMs));
        s.queueCpu.push_back(static_cast<float>(h.refit.queueCpuMs));
        s.chunkMax.push_back(static_cast<float>(h.refit.boxChunkMaxMs));
        s.chunkMean.push_back(static_cast<float>(h.refit.boxChunkMeanMs));
        s.leaves += static_cast<double>(h.refit.leaves);
        s.leafTriangles += static_cast<double>(h.refit.leafTriangles);
        s.unchangedLeaves += static_cast<double>(h.refit.unchangedLeaves);
        s.inner += static_cast<double>(h.refit.inner);
        s.unchangedInner += static_cast<double>(h.refit.unchangedInner);
        s.denseChunks += h.refit.denseChunks;
        s.conesExact += h.refit.conesExact;
        s.conesWidened += h.refit.conesWidened;
        s.conesDisabled += h.refit.conesDisabled;
        s.widenTriangles += static_cast<double>(h.refit.widenTriangles);
        s.pass1.push_back(static_cast<float>(h.refit.conePass1CpuMs));
        s.pass2.push_back(static_cast<float>(h.refit.conePass2CpuMs));
        s.pass2Triangles += static_cast<double>(h.refit.conePass2Triangles);
        const geo::ChunkedBvh::ConeLoss& loss = stats.lastConeLoss;
        if (loss.eyes > 0) {
            s.lossExtra += (static_cast<double>(loss.drawnTriangles) - static_cast<double>(loss.drawnTrianglesExact));
            s.lossExact += static_cast<double>(loss.drawnTrianglesExact);
            s.lossStale += loss.staleChunks;
            s.lossDisabled += loss.disabledChunks;
            s.lossEyes += loss.eyes;
        }
        s.vertices += h.vertices;
        s.affected += h.affected;
        s.triangles += h.triangles;
        // Let the reader run a frame between commands, as a user would.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    };
    for (int c = 0; c < cycles; ++c) {
        for (int i = 0; i < steps; ++i) {
            controller.undo();
            record(undo);
        }
        for (int i = 0; i < steps; ++i) {
            controller.redo();
            record(redo);
        }
    }
    stop = true;
    reader.join();
    const app::InteractionStats stats = controller.stats();
    std::printf("  %s: %d strokes, %d cycles of undo all + redo all; scratch %.1f MiB\n", label, steps, cycles,
                static_cast<double>(stats.editScratchBytes) / kMiB);
    for (const auto& [name, s] : {std::pair<const char*, const Samples*>{"undo", &undo}, {"redo", &redo}}) {
        const double n = static_cast<double>(s->hold.size());
        std::printf("   %s: ~%.0f vertices, %.0f affected, %.0f triangles, %.0f chunks, %.0f cone triangles per step\n", name,
                    s->vertices / n, s->affected / n, s->triangles / n, s->chunks / n, s->coneTriangles / n);
        const std::string prefix(name);
        printSpread((prefix + " lock hold").c_str(), spreadOf(s->hold));
        printSpread((prefix + " total (sculpt thread)").c_str(), spreadOf(s->total));
        printSpread((prefix + "  before the lock").c_str(), spreadOf(s->prepare));
        printSpread((prefix + "  hold: positions").c_str(), spreadOf(s->positions));
        printSpread((prefix + "  hold: normals").c_str(), spreadOf(s->normals));
        printSpread((prefix + "  hold: normals (CPU sum)").c_str(), spreadOf(s->normalsCpu));
        printSpread((prefix + "  before: edit region").c_str(), spreadOf(s->region));
        std::printf("    %s edit region: %.0f of %.0f steps parallel\n", name, s->parallelRegions, n);
        printSpread((prefix + "  hold: dirty pages").c_str(), spreadOf(s->dirty));
        printSpread((prefix + "  hold: refit + cones").c_str(), spreadOf(s->refit));
        printSpread((prefix + "  hold: top level").c_str(), spreadOf(s->top));
        printSpread((prefix + "   refit: boxes (CPU sum)").c_str(), spreadOf(s->boxCpu));
        printSpread((prefix + "   refit: cones (CPU sum)").c_str(), spreadOf(s->coneCpu));
        printSpread((prefix + "    boxes: leaves (CPU sum)").c_str(), spreadOf(s->leafCpu));
        printSpread((prefix + "    boxes: inner nodes (CPU sum)").c_str(), spreadOf(s->innerCpu));
        printSpread((prefix + "    boxes: queue parents (CPU sum)").c_str(), spreadOf(s->queueCpu));
        printSpread((prefix + "    boxes: slowest chunk").c_str(), spreadOf(s->chunkMax));
        printSpread((prefix + "    boxes: mean chunk").c_str(), spreadOf(s->chunkMean));
        std::printf("    %s boxes: %.0f leaves (%.0f triangles, %.0f unchanged), %.0f inner nodes (%.0f unchanged), %.0f dense chunks per step\n",
                    name, s->leaves / n, s->leafTriangles / n, s->unchangedLeaves / n, s->inner / n, s->unchangedInner / n,
                    s->denseChunks / n);
        std::printf("    %s cones: %.1f exact, %.1f widened, %.1f culling off, %.0f widen triangles per step\n", name,
                    s->conesExact / n, s->conesWidened / n, s->conesDisabled / n, s->widenTriangles / n);
        printSpread((prefix + "    cones: pass 1 (CPU sum)").c_str(), spreadOf(s->pass1));
        printSpread((prefix + "    cones: pass 2 (CPU sum)").c_str(), spreadOf(s->pass2));
        std::printf("    %s cones: %.0f second-pass triangles per step\n", name, s->pass2Triangles / n);
        if (s->lossEyes > 0.0) {
            printConeLoss(name, s->lossExtra, s->lossExact, s->lossStale * s->lossEyes / n, s->lossDisabled * s->lossEyes / n,
                          s->lossEyes);
        }
    }
    {
        const double rebuildSteps = static_cast<double>(stats.coneRebuildSteps - before.coneRebuildSteps);
        const double chunks = static_cast<double>(stats.coneRebuildChunks - before.coneRebuildChunks);
        const double commands = static_cast<double>(undo.hold.size() + redo.hold.size());
        std::printf("    cone rebuild: %.1f steps, %.1f chunks, %.3f ms compute (wall) per command; worst step %.3f ms, worst publish hold %.4f ms (since startup)\n",
                    rebuildSteps / commands, chunks / commands, (stats.coneRebuildMs - before.coneRebuildMs) / commands,
                    stats.worstConeRebuildMs, stats.worstConeRebuildHoldMs);
    }
    std::printf("    edit region stamps %.1f MiB (all threads)\n", static_cast<double>(geo::EditRegion::parallelStampBytes()) / kMiB);
    std::printf("    cone face-normal scratch %.1f KiB (all threads)\n",
                static_cast<double>(geo::ChunkedBvh::coneScratchBytes()) / 1024.0);
    printSpread("reader wait (every frame)", spreadOf(waits));
    // After the last redo: compare runs for bit-identical results.
    std::printf("   hash after the cycles %016llx\n", static_cast<unsigned long long>(hashMesh(controller.mesh())));
}

/// One stroke straight through the controller, as fast as it takes inputs.
void runQuickStroke(app::AppController& controller, sculpt::BrushId brush, float radius, float strength, float fromAngle,
                    float toAngle, float height, int samples)
{
    controller.waitUntilIdle();
    controller.setActiveBrush(brush);
    controller.setRadius(radius);
    controller.setStrength(strength);
    const bool grab = brush == sculpt::BrushId::Move;
    if (!controller.beginStroke(orbitRay(fromAngle, height))) {
        std::printf("    stroke missed the mesh\n");
        return;
    }
    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        controller.updateStroke(grab ? orbitRay(fromAngle + 0.3f * t, height + 0.6f * t)
                                     : orbitRay(fromAngle + (toAngle - fromAngle) * t, height));
    }
    controller.endStroke();
    controller.waitUntilIdle();
}

int runHistoryBench(int level, int helperThreads, int cycles, bool parallelGather, bool parallelMarking,
                    bool normalsBeforeLock)
{
    app::AppConfig config;
    config.sphereSubdivisions = level;
    config.jobThreads = helperThreads;
    config.parallelGather = parallelGather;
    config.parallelMarking = parallelMarking;
    applyConeOptions(config);
    config.historyNormalsBeforeLock = normalsBeforeLock;
    app::AppController controller(config);
    std::printf("undo/redo at level %d (%u triangles), %u helper thread(s), normals %s\n", level,
                controller.mesh().triangleCount(), controller.jobThreadCount(),
                normalsBeforeLock ? "before the lock" : "under the lock");
    // Typical edits: the live bench's strokes (radius 0.25, 400 inputs).
    for (const sculpt::BrushId brush : {sculpt::BrushId::Draw, sculpt::BrushId::Smooth, sculpt::BrushId::Move}) {
        runQuickStroke(controller, brush, 0.25f, brush == sculpt::BrushId::Move ? 1.0f : 0.5f, 0.0f, 1.2f, 0.0f, 120);
    }
    runHistoryCycles(controller, "typical strokes (radius 0.25)", 3, cycles, 4.0);
    // Strong strokes like the self-test's: radius 0.5 and 0.3, strength 1, long paths.
    runQuickStroke(controller, sculpt::BrushId::Draw, 0.5f, 1.0f, -0.6f, 0.6f, 0.0f, 40);
    runQuickStroke(controller, sculpt::BrushId::Draw, 0.5f, 1.0f, 0.6f, -0.6f, 0.1f, 40);
    runQuickStroke(controller, sculpt::BrushId::Draw, 0.3f, 1.0f, -0.4f, 0.4f, -0.3f, 40);
    runQuickStroke(controller, sculpt::BrushId::Move, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 30);
    runQuickStroke(controller, sculpt::BrushId::Move, 0.45f, 1.0f, 2.5f, 2.5f, 0.0f, 30);
    runHistoryCycles(controller, "strong strokes (radius 0.3 to 0.5, strength 1)", 5, cycles, 4.0);
    return 0;
}

int runLiveBench(int level, int helperThreads, int rounds, bool parallelGather, bool parallelMarking)
{
    app::AppConfig config;
    config.sphereSubdivisions = level;
    config.jobThreads = helperThreads;
    config.parallelGather = parallelGather;
    config.parallelMarking = parallelMarking;
    applyConeOptions(config);
    const core::Stopwatch startup;
    app::AppController controller(config);
    std::printf("live strokes at level %d (%u triangles), %u helper thread(s), %s gather, startup %.0f ms\n", level,
                controller.mesh().triangleCount(), controller.jobThreadCount(),
                parallelGather ? (parallelMarking ? "parallel, parallel marking" : "parallel") : (parallelMarking ? "serial, parallel marking" : "serial"),
                startup.elapsedMs());
    for (int round = 0; round < rounds; ++round) {
        std::printf(" round %d\n", round + 1);
        for (const sculpt::BrushId brush : {sculpt::BrushId::Draw, sculpt::BrushId::Smooth, sculpt::BrushId::Move}) {
            runLiveStroke(controller, brush, 400, 0.01f, 2.0, 4.0);
        }
        // History through the same lock.
        controller.undo();
        controller.waitUntilIdle();
        const double undoHold = controller.stats().lastUndoRedoHoldMs;
        const double undoMs = controller.stats().lastUndoRedoMs;
        controller.redo();
        controller.waitUntilIdle();
        std::printf("  undo %.2f ms (lock hold %.2f), redo %.2f ms (lock hold %.2f)\n", undoMs, undoHold,
                    controller.stats().lastUndoRedoMs, controller.stats().lastUndoRedoHoldMs);
    }
    return 0;
}

/// Same mesh, same strokes, different vertex page and chunk sizes.
void runSweep(int level, core::JobSystem& jobs)
{
    const u32 vertexCount = static_cast<u32>(geo::icosphereVertexCount(level));
    std::printf("layout sweep at level %d (%u vertices), Draw stroke, one thread | %u helpers\n", level, vertexCount,
                jobs.workerCount());
    std::printf("  %-12s %-10s | %-9s %-9s | %-12s %-9s | %-9s %-9s | %-9s %-8s\n", "vertex page", "chunk", "edit ms", "edit ms*",
                "upload v/f", "% mesh", "tidy ms", "ray us", "mesh MiB", "BVH MiB");

    const auto row = [&](u32 vertexShift, u32 chunkShift) {
        const geo::MeshLayout layout{.vertexPageShift = vertexShift, .trianglePageShift = 12};
        // Minimum of three runs: these are sub-millisecond figures.
        LevelResult best = measure(level, layout, chunkShift, nullptr, nullptr, false);
        LevelResult bestParallel = measure(level, layout, chunkShift, &jobs, nullptr, false);
        for (int repeat = 0; repeat < 2; ++repeat) {
            const LevelResult again = measure(level, layout, chunkShift, nullptr, nullptr, false);
            if (again.draw.msPerEdit < best.draw.msPerEdit) {
                best = again;
            }
            const LevelResult againParallel = measure(level, layout, chunkShift, &jobs, nullptr, false);
            if (againParallel.draw.msPerEdit < bestParallel.draw.msPerEdit) {
                bestParallel = againParallel;
            }
        }
        std::printf("  %-12u %-10u | %-9.3f %-9.3f | %-12.0f %-9.1f | %-9.2f %-9.3f | %-9.1f %-8.1f\n", 1u << vertexShift,
                    1u << chunkShift, best.draw.msPerEdit, bestParallel.draw.msPerEdit, best.draw.uploadVerticesPerFrame,
                    100.0 * best.draw.uploadVerticesPerFrame / vertexCount, best.draw.optimizeMs, best.rayUs, best.meshMiB,
                    best.bvhMiB);
    };

    std::printf("  -- vertex page size (chunk = 4096) --\n");
    for (const u32 vertexShift : {8u, 10u, 12u, 14u, 16u}) {
        row(vertexShift, 12);
    }
    std::printf("  -- chunk size (vertex page = 1024) --\n");
    for (const u32 chunkShift : {9u, 10u, 11u, 12u, 13u, 14u, 16u}) {
        row(10, chunkShift);
    }
    std::printf("  (* with helper threads)\n\n");
}

u64 hashMesh(const geo::Mesh& mesh)
{
    // FNV-1a over the bits of every position and normal.
    u64 hash = 1469598103934665603ull;
    const auto mix = [&hash](const glm::vec3& v) {
        u32 words[3];
        std::memcpy(words, &v, sizeof(words));
        for (const u32 word : words) {
            hash = (hash ^ word) * 1099511628211ull;
        }
    };
    for (const glm::vec3& p : mesh.positions()) {
        mix(p);
    }
    for (const glm::vec3& n : mesh.normals()) {
        mix(n);
    }
    return hash;
}

int runAdjacencyBench(int level, unsigned threads, const geo::MeshLayout& layout, bool parallelGather,
                      bool parallelMarking)
{
    const geo::AdjacencyLayout adjacency = layout.adjacency;
    const auto mib = [](std::size_t bytes) { return static_cast<double>(bytes) / kMiB; };

    // 1. The editor's startup path: generate + order + page, then the BVH.
    const std::size_t heapBefore = bench::heapBytesInUse();
    bench::resetHeapPeak();
    core::Stopwatch timer;
    geo::Mesh generated = geo::makeIcosphere(level, 1.0f, geo::VertexOrder::Spatial, layout);
    const double icosphereMs = timer.elapsedMs();
    const std::size_t icospherePeak = bench::heapPeakBytes() - heapBefore;

    // 2. PagedMesh construction alone, from flat arrays (what an importer hands over).
    std::vector<glm::vec3> flatPositions(generated.vertexCount());
    std::vector<u32> flatIndices(static_cast<std::size_t>(generated.triangleCount()) * 3);
    generated.copyPositions(0, generated.vertexCount(), flatPositions.data());
    generated.copyIndices(0, generated.triangleCount(), flatIndices.data());
    generated = geo::Mesh();
    const std::size_t flatBytes = bench::heapBytesInUse() - heapBefore;
    bench::resetHeapPeak();
    timer.reset();
    geo::Mesh mesh(std::move(flatPositions), std::move(flatIndices), layout);
    const double constructMs = timer.elapsedMs();
    const std::size_t constructPeak = bench::heapPeakBytes() - heapBefore;

    std::unique_ptr<core::JobSystem> jobs = threads > 0 ? std::make_unique<core::JobSystem>(threads) : nullptr;
    geo::ChunkedBvh bvh;
    timer.reset();
    bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, jobs.get(), true);
    bvh.setDenseRefit(g_denseRefit);
    bvh.setIncrementalCones(g_incrementalCones, g_coneFallbackPercent);
    bvh.setReuseConeNormals(g_reuseConeNormals);
    const double bvhMs = timer.elapsedMs();

    const geo::MeshMemory memory = mesh.memory();
    std::printf("level %d: %u triangles, %u vertices, %u helper thread(s), %s adjacency, %s neighbours (%u general fans), %zu u32 rows\n", level,
                mesh.triangleCount(), mesh.vertexCount(), threads,
                adjacency == geo::AdjacencyLayout::Compact ? "compact" : "wide",
                mesh.storesNeighbors() ? "stored" : "derived", mesh.generalFanCount(), mesh.wideAdjacencyRows());
    std::printf("  mesh MiB: total %.2f | positions %.2f normals %.2f triangles %.2f adjacency %.2f (neighbours %.2f, incident "
                "triangles %.2f) bookkeeping %.2f\n",
                mib(memory.total()), mib(memory.positions), mib(memory.normals), mib(memory.triangles), mib(memory.adjacency),
                mib(memory.adjacencyNeighbors), mib(memory.adjacencyIncidentTriangles), mib(memory.bookkeeping));
    std::printf("  startup: makeIcosphere %.1f ms (peak heap %.1f MiB), BVH %.1f ms, total %.1f ms\n", icosphereMs,
                mib(icospherePeak), bvhMs, icosphereMs + bvhMs);
    std::printf("  PagedMesh from flat arrays: %.1f ms, peak heap %.1f MiB (flat input %.1f MiB, final mesh %.1f MiB)\n",
                constructMs, mib(constructPeak), mib(flatBytes), mib(memory.total()));
    std::printf("  initial hash %016llx\n", static_cast<unsigned long long>(hashMesh(mesh)));

    // Raw adjacency reads over a brush-sized region (the vertices of 64
    // consecutive pages, visited in a scrambled order like a BVH gather), one
    // thread. Run after the strokes: the partial normal recompute may change
    // the last bits of normals, which would change the stroke hashes.
    const auto adjacencyReads = [&] {
        const u32 regionFirst = mesh.vertexCount() / 2;
        const u32 regionCount = std::min(mesh.vertexCount() - regionFirst, 64u << layout.vertexPageShift);
        std::vector<geo::VertexId> region(regionCount);
        for (u32 i = 0; i < regionCount; ++i) {
            region[i] = regionFirst + static_cast<u32>((static_cast<u64>(i) * 2654435761u) % regionCount);
        }
        u64 checksum = 0;
        constexpr int kRepeats = 20;
        const auto time = [&](auto&& body) {
            const core::Stopwatch watch;
            for (int r = 0; r < kRepeats; ++r) {
                body();
            }
            return watch.elapsedMs() * 1e6 / (static_cast<double>(kRepeats) * regionCount); // ns per vertex
        };
        const double lookups = time([&] {
            for (const geo::VertexId v : region) {
                checksum += mesh.forEachNeighbor(v, [](u32) {});
            }
        });
        const double neighborsFor = time([&] {
            for (const geo::VertexId v : region) {
                for (const u32 n : mesh.neighbors(v)) {
                    checksum += n;
                }
            }
        });
        const double neighborsEach = time([&] {
            for (const geo::VertexId v : region) {
                mesh.forEachNeighbor(v, [&](u32 n) { checksum += n; });
            }
        });
        const double trianglesEach = time([&] {
            for (const geo::VertexId v : region) {
                mesh.incidentTriangles(v).forEach([&](u32 t) { checksum += t; });
            }
        });
        const double normals = time([&] { mesh.recomputeNormals(region); });
        std::printf("  adjacency reads, ns/vertex: row lookup %.2f, neighbours range-for %.2f,neighbours forEach %.2f, triangles forEach %.2f, "
                    "normals %.2f (checksum %llu)\n",
                    lookups, neighborsFor, neighborsEach, trianglesEach, normals, static_cast<unsigned long long>(checksum % 997));
    };

    const sculpt::BrushLibrary brushes;
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(jobs.get());
    stroke.setParallelGather(parallelGather);
    stroke.setParallelMarking(parallelMarking);
    stroke.setParallelRegion(g_parallelRegion);
    std::printf("  gather: %s, BVH leaf marking: %s\n", parallelGather ? "parallel" : "serial",
                parallelMarking ? "parallel" : "serial");
    const auto one = [&](const char* name, sculpt::BrushId id) {
        const StrokeResult r = runStroke(mesh, bvh, stroke, brushes.brush(id));
        const double edits = std::max(r.edits, 1u);
        std::printf("  %-6s %4u edits, %7.4f ms/edit (brush %.4f normals %.4f bvh %.4f gather %.4f publish %.4f stage %.4f), worst sample %.3f ms, ~%u "
                    "verts/edit, hash %016llx\n",
                    name, r.edits, r.msPerEdit, r.stages.brushMs / edits, r.stages.normalsMs / edits, r.stages.bvhMs / edits,
                    r.stages.gatherMs / edits, r.stages.publishMs / edits, r.stages.stageMs / edits, r.worstSampleMs, r.verticesPerEdit,
                    static_cast<unsigned long long>(hashMesh(mesh)));
        std::printf("         BVH per edit %.4f: leaf marking %.4f (prepare); commit %.4f = publish %.4f + local refits and cones %.4f + top %.4f ms; %.0f cone triangles\n",
                    r.stages.bvhMs / edits, r.stages.refit.markMs / edits,
                    (r.stages.publishMs + r.stages.refit.localMs + r.stages.refit.topMs) / edits, r.stages.publishMs / edits,
                    r.stages.refit.localMs / edits, r.stages.refit.topMs / edits,
                    static_cast<double>(r.stages.coneTriangles) / edits);
        {
            const auto& n = r.stages.normals;
            const double affected = std::max<double>(1.0, static_cast<double>(n.affected));
            std::printf("         normals per edit %.4f: region %.4f + compute %.4f (CPU %.4f, %.1f ns per vertex) + dirty %.4f ms; "
                        "%.0f moved, %.0f affected vertices, %.0f triangles; %u of %u regions parallel, stamps %.1f MiB\n",
                        r.stages.normalsMs / edits, n.regionMs / edits, n.computeMs / edits, n.computeCpuMs / edits,
                        n.computeCpuMs * 1e6 / affected, n.dirtyMs / edits, static_cast<double>(n.moved) / edits,
                        static_cast<double>(n.affected) / edits, static_cast<double>(n.triangles) / edits, n.parallelRegions,
                        r.edits, static_cast<double>(geo::EditRegion::parallelStampBytes()) / kMiB);
        }
        std::printf("         gather per edit %.4f: query %.4f + select %.4f + undo record %.4f + other (average normal) %.4f ms\n",
                    r.stages.gatherMs / edits, r.stages.gather.queryMs / edits, r.stages.gather.selectMs / edits,
                    r.stages.gather.recordMs / edits,
                    (r.stages.gatherMs - r.stages.gather.queryMs - r.stages.gather.selectMs - r.stages.gather.recordMs) / edits);
        std::printf("         cones per edit: CPU %.4f ms (boxes %.4f), %.2f chunks: %.2f exact, %.2f widened, %.2f culling off, %.0f widen triangles\n",
                    r.stages.refit.coneCpuMs / edits, r.stages.refit.boxCpuMs / edits, r.stages.refit.chunks / edits,
                    r.stages.refit.conesExact / edits, r.stages.refit.conesWidened / edits, r.stages.refit.conesDisabled / edits,
                    static_cast<double>(r.stages.refit.widenTriangles) / edits);
        std::printf("         exact cones per edit: pass 1 %.4f ms, pass 2 %.4f ms (CPU), %.0f second-pass triangles; scratch %.1f KiB\n",
                    r.stages.refit.conePass1CpuMs / edits, r.stages.refit.conePass2CpuMs / edits,
                    static_cast<double>(r.stages.refit.conePass2Triangles) / edits,
                    static_cast<double>(geo::ChunkedBvh::coneScratchBytes()) / 1024.0);
        if (bvh.incrementalCones()) {
            std::printf("         after the stroke: %u stale cones, exact rebuild %.3f ms (one thread)\n", r.staleCones, r.coneRebuildMs);
            if (r.coneLoss.eyes > 0) {
                printConeLoss("stroke end", (static_cast<double>(r.coneLoss.drawnTriangles) - static_cast<double>(r.coneLoss.drawnTrianglesExact)),
                              static_cast<double>(r.coneLoss.drawnTrianglesExact), static_cast<double>(r.coneLoss.staleChunks) * r.coneLoss.eyes,
                              static_cast<double>(r.coneLoss.disabledChunks) * r.coneLoss.eyes,
                              r.coneLoss.eyes);
            }
        }
    };
    one("Draw", sculpt::BrushId::Draw);
    one("Smooth", sculpt::BrushId::Smooth);
    one("Move", sculpt::BrushId::Move);
    std::printf("  edit scratch (edit region lists and bitsets, staging pages and table, new normals, parallel gather and BVH marking lists): %.2f MiB, of which parallel gather %.2f MiB, parallel marking %.2f MiB\n",
                static_cast<double>(stroke.editScratchBytes()) / (1024.0 * 1024.0),
                static_cast<double>(stroke.parallelGatherScratchBytes()) / (1024.0 * 1024.0),
                static_cast<double>(bvh.markScratchBytes()) / (1024.0 * 1024.0));
    adjacencyReads();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        g_denseRefit = g_denseRefit || std::strcmp(argv[i], "dense") == 0;
        g_incrementalCones = g_incrementalCones || std::strcmp(argv[i], "icones") == 0;
        g_measureConeLoss = g_measureConeLoss || std::strcmp(argv[i], "closs") == 0;
        g_reuseConeNormals = g_reuseConeNormals || std::strcmp(argv[i], "creuse") == 0;
        g_parallelRegion = g_parallelRegion || std::strcmp(argv[i], "pregion") == 0;
        if (std::strncmp(argv[i], "cfb=", 4) == 0) {
            g_coneFallbackPercent = static_cast<u32>(std::atoi(argv[i] + 4));
        }
        if (std::strncmp(argv[i], "r=", 2) == 0) {
            g_dabRadius = static_cast<float>(std::atof(argv[i] + 2));
        }
    }
    if (g_incrementalCones) {
        std::printf("incremental normal cones, fallback above %u%% of a chunk's leaves%s\n", g_coneFallbackPercent,
                    g_measureConeLoss ? ", measuring cone loss" : "");
    }
    if (g_parallelRegion) {
        std::printf("parallel edit region\n");
    }
    if (g_reuseConeNormals) {
        std::printf("cone face normals reused in the second pass\n");
    }
    if (g_dabRadius != 0.25f) {
        std::printf("dab radius %.3f\n", static_cast<double>(g_dabRadius));
    }
    if (g_denseRefit) {
        std::printf("dense box refit\n");
    }
    if (argc > 1 && std::strcmp(argv[1], "live") == 0) {
        const int level = argc > 2 ? std::atoi(argv[2]) : 9;
        const int helpers = argc > 3 ? std::atoi(argv[3]) : -1;
        const int rounds = argc > 4 ? std::max(1, std::atoi(argv[4])) : 3;
        bool parallelGather = false;
        bool parallelMarking = false;
        for (int i = 5; i < argc; ++i) {
            parallelGather = parallelGather || std::strcmp(argv[i], "pgather") == 0;
            parallelMarking = parallelMarking || std::strcmp(argv[i], "pmark") == 0;
        }
        return runLiveBench(level, helpers, rounds, parallelGather, parallelMarking);
    }
    if (argc > 1 && std::strcmp(argv[1], "history") == 0) {
        const int level = argc > 2 ? std::atoi(argv[2]) : 9;
        const int helpers = argc > 3 ? std::atoi(argv[3]) : -1;
        const int cycles = argc > 4 ? std::max(1, std::atoi(argv[4])) : 4;
        bool parallelGather = false;
        bool parallelMarking = false;
        bool normalsBeforeLock = false;
        for (int i = 5; i < argc; ++i) {
            parallelGather = parallelGather || std::strcmp(argv[i], "pgather") == 0;
            parallelMarking = parallelMarking || std::strcmp(argv[i], "pmark") == 0;
            normalsBeforeLock = normalsBeforeLock || std::strcmp(argv[i], "hstage") == 0;
        }
        return runHistoryBench(level, helpers, cycles, parallelGather, parallelMarking, normalsBeforeLock);
    }
    if (argc > 1 && std::strcmp(argv[1], "adjacency") == 0) {
        const int level = argc > 2 ? std::atoi(argv[2]) : 8;
        const unsigned helpers = argc > 3 ? static_cast<unsigned>(std::max(0, std::atoi(argv[3])))
                                          : core::JobSystem::defaultWorkerCount();
        // Options in any order after the helper count: wide|compact, stored|derived, pgather, pmark.
        bool wide = false;
        bool derived = false;
        bool parallelGather = false;
        bool parallelMarking = false;
        for (int i = 4; i < argc; ++i) {
            wide = wide || std::strcmp(argv[i], "wide") == 0;
            derived = derived || std::strcmp(argv[i], "derived") == 0;
            parallelGather = parallelGather || std::strcmp(argv[i], "pgather") == 0;
            parallelMarking = parallelMarking || std::strcmp(argv[i], "pmark") == 0;
        }
        return runAdjacencyBench(level, helpers,
                                 {.adjacency = wide ? geo::AdjacencyLayout::Wide : geo::AdjacencyLayout::Compact,
                                  .neighbors = derived ? geo::NeighborStorage::Derived : geo::NeighborStorage::Stored},
                                 parallelGather, parallelMarking);
    }
    const int first = argc > 1 ? std::atoi(argv[1]) : 6;
    const int last = argc > 2 ? std::atoi(argv[2]) : 8;
    const unsigned threads = argc > 3 ? static_cast<unsigned>(std::max(0, std::atoi(argv[3])))
                                      : core::JobSystem::defaultWorkerCount();
    const bool sweep = argc > 4 && std::strcmp(argv[4], "sweep") == 0;

    core::JobSystem jobs(threads);
    for (int level = first; level <= last; ++level) {
        runLevel(level, jobs);
    }
    for (int level = first; level <= last; ++level) {
        runResponsiveness(level, static_cast<int>(threads));
    }
    if (sweep) {
        runSweep(last, jobs);
    }
    return 0;
}
