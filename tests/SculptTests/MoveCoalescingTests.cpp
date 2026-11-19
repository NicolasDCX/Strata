// Latest-wins Move updates (AppConfig::coalesceMoveUpdates): when Move updates
// pile up behind the one being applied, only the newest that changes anything
// is applied. A Grab update re-applies the whole drag to the positions captured
// at the stroke's start, so every observable result must equal applying every
// update: positions and normals bit for bit, undo records, dirty pages, and
// (once the background work is done) BVH boxes, normal cones, the top level and
// picking. While a stroke runs the incrementally widened cones may differ (they
// are widened by fewer intermediate normal sets) but must stay conservative.

#include "Application/AppController.h"
#include "Geometry/ChunkedBvh.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

using namespace se;
using sculpt::BrushId;

namespace {

u64 hashMesh(const geo::Mesh& mesh)
{
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

std::vector<u32> pickAll(const geo::ChunkedBvh& bvh, const geo::Mesh& mesh)
{
    std::vector<u32> bits;
    for (int y = -12; y <= 12; ++y) {
        for (int x = -12; x <= 12; ++x) {
            const geo::Ray ray{{0.09f * static_cast<float>(x), 0.09f * static_cast<float>(y), 5.0f}, {0.0f, 0.0f, -1.0f}};
            const std::optional<geo::RayHit> hit = bvh.raycast(mesh, ray);
            if (!hit) {
                bits.push_back(kInvalidIndex);
                continue;
            }
            u32 words[4];
            std::memcpy(&words[0], &hit->distance, 4);
            std::memcpy(&words[1], &hit->position, 12);
            bits.push_back(hit->triangle);
            bits.insert(bits.end(), words, words + 4);
        }
    }
    return bits;
}

geo::Ray rayAt(float x, float y)
{
    return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}};
}

/// Everything a step must leave the same, read once the controller is idle.
struct Snapshot {
    u64 hash = 0;
    std::vector<u32> picks;
    std::unique_ptr<geo::ChunkedBvh> bvh;
    std::vector<u8> dirtyPages; // pending dirty set (never drained here): union of every change so far
    std::vector<u32> undoVertices;
    std::vector<glm::vec3> undoPositions;
    std::vector<u32> redoVertices;
    std::vector<glm::vec3> redoPositions;
    std::size_t undoCount = 0;
    std::size_t redoCount = 0;
    bool needsOptimize = false;
    bool staleCones = false;
};

struct ScriptRun {
    std::vector<Snapshot> steps;
    u32 coalesced = 0;           // dropped updates, summed over the Move strokes
    u32 maxStrokeCoalesced = 0;
    u32 coneViolations = 0;      // published cones checked while strokes ran
    u32 sampleMismatches = 0;    // strokes whose input samples did not add up
};

Snapshot snapshot(app::AppController& controller)
{
    controller.waitUntilIdle();
    Snapshot s;
    const std::scoped_lock lock(controller.documentMutex());
    s.hash = hashMesh(controller.mesh());
    s.picks = pickAll(controller.spatialIndex(), controller.mesh());
    s.bvh = std::make_unique<geo::ChunkedBvh>(controller.spatialIndex());
    const geo::DirtyVertexSet& dirty = controller.pendingVertexUpdates();
    for (u32 page = 0; page < dirty.pageCount(); ++page) {
        s.dirtyPages.push_back(dirty.pageDirty(page) ? 1 : 0);
    }
    const sculpt::UndoStack& history = controller.history();
    s.undoVertices.assign(history.undoVertices().begin(), history.undoVertices().end());
    s.undoPositions.assign(history.undoPositions().begin(), history.undoPositions().end());
    s.redoVertices.assign(history.redoVertices().begin(), history.redoVertices().end());
    s.redoPositions.assign(history.redoPositions().begin(), history.redoPositions().end());
    s.undoCount = history.undoCount();
    s.redoCount = history.redoCount();
    s.needsOptimize = controller.spatialIndex().needsOptimize();
    s.staleCones = controller.spatialIndex().hasStaleCones();
    return s;
}

/// One stroke whose updates are queued while the test holds the document lock,
/// so the sculpt thread is held at its first commit and the later updates pile
/// up behind it (an overloaded queue). Settings changes are made in the middle
/// of the stroke too: a stroke keeps the settings it began with.
void overloadedStroke(app::AppController& controller, ScriptRun& run, BrushId brush, float radius, float strength,
                      const std::vector<geo::Ray>& rays, bool releaseEarly = false)
{
    controller.setActiveBrush(brush);
    controller.setRadius(radius);
    controller.setStrength(strength);
    controller.waitUntilIdle();
    ASSERT_TRUE(controller.beginStroke(rays.front())); // picks under the document lock
    {
        const std::scoped_lock lock(controller.documentMutex());
        for (std::size_t i = 1; i < rays.size(); ++i) {
            controller.updateStroke(rays[i]);
            if (i == rays.size() / 2) {
                controller.setRadius(radius * 0.5f); // client-side only; the stroke keeps its radius
                controller.setStrength(strength * 0.5f);
            }
        }
        if (!releaseEarly) {
            controller.endStroke();
        }
    }
    if (releaseEarly) {
        controller.endStroke(); // immediate release once the sculpt thread can run
    }
    // Published cones must hold every normal at any time, widened or not.
    for (int i = 0; i < 6; ++i) {
        const auto [violations, usable] = controller.checkNormalCones();
        run.coneViolations += violations;
    }
    controller.waitUntilIdle();
    const app::InteractionStats stats = controller.stats();
    if (stats.inputSamples.size() != rays.size()) {
        ++run.sampleMismatches;
    }
    u32 dropped = 0;
    for (const auto& sample : stats.inputSamples) {
        dropped += sample.coalesced ? 1u : 0u;
    }
    if (dropped != stats.strokeCoalescedInputs) {
        ++run.sampleMismatches;
    }
    run.coalesced += dropped;
    run.maxStrokeCoalesced = std::max(run.maxStrokeCoalesced, dropped);
}

std::vector<geo::Ray> path(float x0, float y0, float x1, float y1, int updates)
{
    std::vector<geo::Ray> rays{rayAt(x0, y0)};
    for (int i = 1; i <= updates; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(updates);
        rays.push_back(rayAt(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
    }
    return rays;
}

ScriptRun runScript(bool coalesce, int helpers)
{
    app::AppConfig config;
    config.sphereSubdivisions = 7;
    config.jobThreads = helpers;
    config.coalesceMoveUpdates = coalesce;
    app::AppController controller(config);
    ScriptRun run;
    const auto step = [&] { run.steps.push_back(snapshot(controller)); };
    step();

    // 1. Overloaded Move: 80 updates dragging outward.
    overloadedStroke(controller, run, BrushId::Move, 0.6f, 1.0f, path(-0.2f, 0.1f, 0.25f, 0.3f, 80));
    step();
    // 2. Direction reversals: out, back past the start, out again.
    {
        std::vector<geo::Ray> rays = path(0.1f, -0.2f, 0.4f, -0.2f, 30);
        const std::vector<geo::Ray> back = path(0.4f, -0.2f, -0.1f, -0.15f, 30);
        rays.insert(rays.end(), back.begin() + 1, back.end());
        const std::vector<geo::Ray> again = path(-0.1f, -0.15f, 0.2f, -0.35f, 20);
        rays.insert(rays.end(), again.begin() + 1, again.end());
        overloadedStroke(controller, run, BrushId::Move, 0.5f, 0.8f, rays);
        step();
    }
    // 3. Out and back to exactly the start: the final drag is zero.
    {
        std::vector<geo::Ray> rays = path(-0.3f, -0.3f, 0.1f, -0.1f, 25);
        const std::vector<geo::Ray> back = path(0.1f, -0.1f, -0.3f, -0.3f, 25);
        rays.insert(rays.end(), back.begin() + 1, back.end());
        rays.back() = rays.front();
        overloadedStroke(controller, run, BrushId::Move, 0.5f, 1.0f, rays);
        step();
    }
    // 4. The newest updates are no-ops for the Grab (pointing away from the
    //    drag plane, parallel to it): the newest one that applies must win.
    {
        std::vector<geo::Ray> rays = path(0.0f, 0.3f, 0.3f, 0.45f, 20);
        rays.push_back(geo::Ray{{0.5f, 0.5f, 5.0f}, {0.0f, 0.0f, 1.0f}});
        rays.push_back(geo::Ray{{0.5f, 0.5f, 5.0f}, {1.0f, 0.0f, 0.0f}});
        overloadedStroke(controller, run, BrushId::Move, 0.4f, 1.0f, rays);
        step();
    }
    // 5. Immediate release: one update; none; all updates queued with the end.
    overloadedStroke(controller, run, BrushId::Move, 0.3f, 1.0f, path(0.2f, 0.0f, 0.35f, 0.05f, 1));
    step();
    overloadedStroke(controller, run, BrushId::Move, 0.3f, 1.0f, path(0.2f, 0.0f, 0.2f, 0.0f, 0));
    step();
    overloadedStroke(controller, run, BrushId::Move, 0.3f, 1.0f, path(-0.25f, 0.2f, -0.4f, 0.4f, 40), true);
    step();
    // 6. Other brushes between Move strokes: never coalesced.
    overloadedStroke(controller, run, BrushId::Draw, 0.3f, 0.5f, path(-0.4f, -0.1f, 0.4f, 0.0f, 30));
    step();
    overloadedStroke(controller, run, BrushId::Move, 0.5f, 1.0f, path(0.0f, 0.0f, -0.2f, 0.25f, 60));
    step();
    overloadedStroke(controller, run, BrushId::Smooth, 0.3f, 0.5f, path(-0.4f, 0.05f, 0.4f, 0.1f, 30));
    step();
    // 7. Undo and redo, a new overloaded Move stroke on top of an undone one
    //    (discarding the redo), then undo all and redo all.
    for (int i = 0; i < 3; ++i) {
        controller.undo();
        step();
    }
    controller.redo();
    step();
    overloadedStroke(controller, run, BrushId::Move, 0.7f, 1.0f, path(0.15f, 0.15f, -0.3f, -0.1f, 80));
    step();
    while (controller.canUndo()) {
        controller.undo();
        step();
    }
    while (controller.canRedo()) {
        controller.redo();
        step();
    }
    return run;
}

void expectSame(const Snapshot& a, const Snapshot& b, std::size_t step)
{
    SCOPED_TRACE(testing::Message() << "step " << step);
    EXPECT_EQ(a.hash, b.hash) << "positions or normals";
    EXPECT_EQ(a.picks, b.picks);
    EXPECT_TRUE(a.bvh->sameBounds(*b.bvh)) << "boxes or cones";
    const auto& ta = a.bvh->topNodes();
    const auto& tb = b.bvh->topNodes();
    ASSERT_EQ(ta.size(), tb.size());
    for (std::size_t i = 0; i < ta.size(); ++i) {
        EXPECT_TRUE(ta[i].bounds == tb[i].bounds && ta[i].left == tb[i].left && ta[i].chunk == tb[i].chunk) << "top node " << i;
    }
    for (u32 c = 0; c < a.bvh->chunkCount(); ++c) {
        EXPECT_EQ(a.bvh->chunkConeStale(c), b.bvh->chunkConeStale(c)) << "chunk " << c;
    }
    EXPECT_EQ(a.dirtyPages, b.dirtyPages);
    EXPECT_EQ(a.undoVertices, b.undoVertices);
    EXPECT_TRUE(test::bitwiseEqual(a.undoPositions, b.undoPositions)) << "undo record";
    EXPECT_EQ(a.redoVertices, b.redoVertices);
    EXPECT_TRUE(test::bitwiseEqual(a.redoPositions, b.redoPositions)) << "redo record";
    EXPECT_EQ(a.undoCount, b.undoCount);
    EXPECT_EQ(a.redoCount, b.redoCount);
    EXPECT_EQ(a.needsOptimize, b.needsOptimize);
    EXPECT_EQ(a.staleCones, b.staleCones);
}

} // namespace

TEST(MoveCoalescing, MatchesEveryUpdateAppliedAtAnyHelperCount)
{
    const ScriptRun reference = runScript(false, 3);
    EXPECT_EQ(reference.coalesced, 0u) << "off: nothing is dropped";
    EXPECT_EQ(reference.coneViolations, 0u);
    EXPECT_EQ(reference.sampleMismatches, 0u);
    // The script really sculpts and really comes back.
    ASSERT_GE(reference.steps.size(), 20u);
    EXPECT_NE(reference.steps[1].hash, reference.steps[0].hash);
    for (const int helpers : {0, 1, 3, 11}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const ScriptRun coalesced = runScript(true, helpers);
        EXPECT_GT(coalesced.coalesced, 100u) << "the overloaded strokes must drop most updates";
        EXPECT_GE(coalesced.maxStrokeCoalesced, 60u);
        EXPECT_EQ(coalesced.coneViolations, 0u) << "widened cones stay conservative";
        EXPECT_EQ(coalesced.sampleMismatches, 0u);
        ASSERT_EQ(coalesced.steps.size(), reference.steps.size());
        for (std::size_t i = 0; i < reference.steps.size(); ++i) {
            expectSame(coalesced.steps[i], reference.steps[i], i);
        }
        if (helpers != 3) {
            const ScriptRun uncoalesced = runScript(false, helpers);
            for (std::size_t i = 0; i < reference.steps.size(); ++i) {
                EXPECT_EQ(uncoalesced.steps[i].hash, reference.steps[i].hash) << "step " << i;
            }
        }
    }
}

TEST(MoveCoalescing, OnlyConsecutiveMoveUpdatesOfOneStrokeAreDropped)
{
    // Draw and Smooth strokes queued the same way drop nothing; a Move stroke
    // keeps its begin and its end; the newest update is always applied.
    for (const int helpers : {0, 3}) {
        app::AppConfig config;
        config.sphereSubdivisions = 6;
        config.jobThreads = helpers;
        config.coalesceMoveUpdates = true;
        app::AppController controller(config);
        ScriptRun run;
        overloadedStroke(controller, run, BrushId::Draw, 0.3f, 0.5f, path(-0.4f, 0.0f, 0.4f, 0.1f, 40));
        overloadedStroke(controller, run, BrushId::Smooth, 0.3f, 0.5f, path(-0.4f, 0.1f, 0.4f, 0.0f, 40));
        EXPECT_EQ(run.coalesced, 0u);
        overloadedStroke(controller, run, BrushId::Move, 0.5f, 1.0f, path(-0.2f, 0.0f, 0.3f, 0.2f, 40));
        const app::InteractionStats stats = controller.stats();
        ASSERT_EQ(stats.inputSamples.size(), 41u);
        EXPECT_FALSE(stats.inputSamples.front().coalesced) << "the begin input";
        EXPECT_FALSE(stats.inputSamples.back().coalesced) << "the newest update";
        EXPECT_GE(stats.strokeCoalescedInputs, 30u);
        for (std::size_t i = 1; i < stats.inputSamples.size(); ++i) {
            EXPECT_LE(stats.inputSamples[i - 1].doneAtMs, stats.inputSamples[i].startedAtMs);
        }
        EXPECT_EQ(controller.undoCount(), 3u);
        EXPECT_GE(stats.coalescedInputs, stats.strokeCoalescedInputs);
    }
}
