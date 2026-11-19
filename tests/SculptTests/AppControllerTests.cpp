#include "Application/AppController.h"
#include "Geometry/Icosphere.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace se;
using sculpt::BrushId;

// The controller applies edits on its own sculpt thread. These tests call
// waitUntilIdle() before looking at results; that is the documented way to get
// a deterministic view.

namespace {

geo::Ray rayAt(float x, float y)
{
    return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}};
}

/// Takes the pending dirty vertices, like a renderer does; returns true if there were any.
bool drainDirty(app::AppController& controller)
{
    geo::DirtyVertexSet dirty(controller.mesh().vertexCount());
    controller.drainDirtyVertices(dirty);
    return !dirty.empty();
}

/// Copy of the positions once all queued work is done.
std::vector<glm::vec3> settledPositions(app::AppController& controller)
{
    controller.waitUntilIdle();
    const auto positions = controller.mesh().positions();
    return {positions.begin(), positions.end()};
}

void stroke(app::AppController& controller, float x0, float y0, float x1, float y1, int samples = 8,
            app::StrokeModifiers modifiers = {})
{
    ASSERT_TRUE(controller.beginStroke(rayAt(x0, y0), modifiers));
    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        controller.updateStroke(rayAt(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
    }
    controller.endStroke();
}

} // namespace

TEST(AppController, StartsWithConfiguredSphere)
{
    const app::AppController controller({.sphereSubdivisions = 3, .sphereRadius = 2.0f});
    EXPECT_EQ(controller.mesh().triangleCount(), geo::icosphereTriangleCount(3));
    EXPECT_NEAR(controller.bounds().max.y, 2.0f, 0.1f);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_EQ(controller.activeBrush(), BrushId::Draw);

    const auto hit = controller.pick(rayAt(0.0f, 0.0f));
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->position.z, 2.0f, 0.02f);
    EXPECT_FALSE(controller.pick(rayAt(5.0f, 0.0f)).has_value());
}

TEST(AppController, BrushesRememberTheirOwnSettings)
{
    app::AppController controller({.sphereSubdivisions = 2});
    controller.setRadius(0.7f);
    controller.setStrength(0.9f);
    controller.setActiveBrush(BrushId::Smooth);
    controller.setRadius(0.1f);
    controller.setActiveBrush(BrushId::Draw);
    EXPECT_FLOAT_EQ(controller.activeSettings().radius, 0.7f);
    EXPECT_FLOAT_EQ(controller.activeSettings().strength, 0.9f);

    controller.setRadius(100.0f);
    EXPECT_FLOAT_EQ(controller.activeSettings().radius, app::AppController::kMaxRadius);
    controller.setStrength(-1.0f);
    EXPECT_FLOAT_EQ(controller.activeSettings().strength, 0.0f);
}

TEST(AppController, FullSculptUndoRedoCycle)
{
    app::AppController controller({.sphereSubdivisions = 4});
    const std::vector<glm::vec3> original = settledPositions(controller);
    const std::vector<glm::vec3> originalNormals(controller.mesh().normals().begin(), controller.mesh().normals().end());
    EXPECT_FALSE(controller.hasDirtyVertices());

    // Draw
    ASSERT_TRUE(controller.beginStroke(rayAt(-0.2f, 0.0f)));
    EXPECT_TRUE(controller.strokeActive());
    EXPECT_FALSE(controller.canUndo()) << "no undo while a stroke is in progress";
    controller.undo(); // must be ignored during a stroke
    controller.updateStroke(rayAt(0.2f, 0.0f));
    controller.endStroke();
    EXPECT_FALSE(controller.strokeActive());
    const std::vector<glm::vec3> afterDraw = settledPositions(controller);
    EXPECT_NE(afterDraw, original);
    EXPECT_EQ(controller.undoCount(), 1u);
    EXPECT_TRUE(drainDirty(controller));
    EXPECT_FALSE(controller.hasDirtyVertices());

    // Smooth
    controller.setActiveBrush(BrushId::Smooth);
    stroke(controller, -0.2f, 0.0f, 0.2f, 0.0f, 1);
    const std::vector<glm::vec3> afterSmooth = settledPositions(controller);
    EXPECT_NE(afterSmooth, afterDraw);

    // Move
    controller.setActiveBrush(BrushId::Move);
    stroke(controller, 0.0f, 0.8f, 0.0f, 1.3f, 1);
    const std::vector<glm::vec3> afterMove = settledPositions(controller);
    EXPECT_NE(afterMove, afterSmooth);
    EXPECT_GT(controller.bounds().max.y, 1.2f) << "bounds must follow the edit";

    EXPECT_EQ(controller.undoCount(), 3u);
    drainDirty(controller);

    controller.undo();
    EXPECT_EQ(settledPositions(controller), afterSmooth);
    EXPECT_TRUE(drainDirty(controller)) << "undo must notify the renderer";
    EXPECT_EQ(controller.redoCount(), 1u);
    controller.undo();
    EXPECT_EQ(settledPositions(controller), afterDraw);
    controller.undo();
    EXPECT_EQ(settledPositions(controller), original);
    EXPECT_FALSE(controller.canUndo());
    controller.undo(); // nothing left: must be harmless
    EXPECT_EQ(settledPositions(controller), original);
    EXPECT_LT(controller.bounds().max.y, 1.001f) << "bounds must shrink back after undo";

    const auto normals = controller.mesh().normals();
    for (std::size_t i = 0; i < originalNormals.size(); ++i) {
        ASSERT_NEAR(glm::distance(normals[i], originalNormals[i]), 0.0f, 1e-6f);
    }

    // Requests queue up: three redos without waiting in between.
    controller.redo();
    controller.redo();
    controller.redo();
    EXPECT_EQ(settledPositions(controller), afterMove);
    EXPECT_FALSE(controller.canRedo());
    controller.redo();
    EXPECT_EQ(settledPositions(controller), afterMove);

    // Spatial queries must be correct in the final state (including the BVH
    // blocks that were re-partitioned in the background).
    std::mt19937 rng(5);
    for (int i = 0; i < 200; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const auto expected = test::bruteForceRaycast(controller.mesh(), ray);
        const auto actual = controller.pick(ray);
        ASSERT_EQ(expected.has_value(), actual.has_value());
        if (expected) {
            ASSERT_NEAR(*expected, actual->distance, 1e-4f);
        }
    }
}

TEST(AppController, ModifiersInvertAndSmooth)
{
    app::AppController controller({.sphereSubdivisions = 5});
    // A small, sharp dent: a few edges wide, which is what edge-based smoothing removes quickly.
    controller.setRadius(0.1f);
    controller.setStrength(1.0f);

    ASSERT_TRUE(controller.beginStroke(rayAt(0.0f, 0.0f), {.invert = true, .smooth = false}));
    controller.endStroke();
    controller.waitUntilIdle();
    const auto hit = controller.pick(rayAt(0.0f, 0.0f));
    ASSERT_TRUE(hit.has_value());
    EXPECT_LT(hit->position.z, 0.995f) << "inverted draw must carve";

    // Depth of the dent relative to the brush rim, measured radially. (Absolute
    // height is not a good measure: smoothing also flattens convex surfaces slightly.)
    const auto dentDepth = [&controller] {
        controller.waitUntilIdle();
        const auto centre = controller.pick(rayAt(0.0f, 0.0f));
        const auto rim = controller.pick(rayAt(0.09f, 0.0f));
        return glm::length(rim->position) - glm::length(centre->position);
    };
    const float carved = dentDepth();
    EXPECT_GT(carved, 0.008f);

    // Shift = temporary smooth while Draw stays selected.
    ASSERT_TRUE(controller.beginStroke(rayAt(0.0f, 0.0f), {.invert = false, .smooth = true}));
    for (int i = 0; i < 30; ++i) {
        controller.updateStroke(rayAt(0.04f * static_cast<float>(i % 2), 0.0f));
    }
    controller.endStroke();
    EXPECT_EQ(controller.activeBrush(), BrushId::Draw);
    EXPECT_LT(dentDepth(), carved * 0.6f);
    EXPECT_EQ(controller.undoCount(), 2u);
}

TEST(AppController, StrokeThatMissesChangesNothing)
{
    app::AppController controller({.sphereSubdivisions = 3});
    EXPECT_FALSE(controller.beginStroke(rayAt(4.0f, 4.0f)));
    EXPECT_FALSE(controller.strokeActive());
    controller.updateStroke(rayAt(0.0f, 0.0f));
    controller.endStroke();
    controller.waitUntilIdle();
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.hasDirtyVertices());
}

TEST(AppController, InputCallsReturnWithoutWaitingForTheEdit)
{
    // A large brush on a dense mesh makes each input expensive for the sculpt
    // thread. Queuing a whole stroke must nevertheless be nearly instant.
    app::AppController controller({.sphereSubdivisions = 7, .sphereRadius = 1.0f, .jobThreads = 0});
    controller.setRadius(0.4f);

    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(controller.beginStroke(rayAt(-0.4f, 0.0f)));
    for (int i = 1; i <= 200; ++i) {
        controller.updateStroke(rayAt(-0.4f + 0.004f * static_cast<float>(i), 0.0f));
    }
    controller.endStroke();
    const double queueMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    controller.waitUntilIdle();
    const double processMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(controller.undoCount(), 1u);
    EXPECT_GT(controller.stats().strokeDabCount, 5u);
    // Generous bounds: this is about orders of magnitude, not exact timing.
    EXPECT_LT(queueMs, processMs * 0.5) << "queuing took " << queueMs << " ms of " << processMs << " ms total";
}

TEST(AppController, InputSamplesAccountForEveryInputAndDab)
{
    // One sample per stroke input (begin + updates), in order, with consistent
    // timestamps; their dabs add up to the stroke's dab count. Move applies once
    // per update and none at begin.
    for (const BrushId brush : {BrushId::Draw, BrushId::Smooth, BrushId::Move}) {
        SCOPED_TRACE(static_cast<int>(brush));
        // Every Move update applied (latest-wins coalescing has its own tests).
        app::AppConfig config{.sphereSubdivisions = 6, .sphereRadius = 1.0f, .jobThreads = 3};
        config.coalesceMoveUpdates = false;
        app::AppController controller(config);
        controller.setActiveBrush(brush);
        ASSERT_TRUE(controller.beginStroke(rayAt(-0.4f, 0.0f)));
        constexpr int kUpdates = 60;
        for (int i = 1; i <= kUpdates; ++i) {
            controller.updateStroke(rayAt(-0.4f + 0.012f * static_cast<float>(i), 0.0f));
        }
        controller.waitUntilIdle();
        const app::InteractionStats stats = controller.stats(); // read before the stroke ends
        controller.endStroke();
        controller.waitUntilIdle();

        ASSERT_EQ(stats.inputSamples.size(), static_cast<std::size_t>(kUpdates + 1));
        u32 dabs = 0;
        for (std::size_t i = 0; i < stats.inputSamples.size(); ++i) {
            const auto& sample = stats.inputSamples[i];
            EXPECT_LE(sample.queuedAtMs, sample.startedAtMs);
            EXPECT_LE(sample.startedAtMs, sample.doneAtMs);
            if (i > 0) {
                EXPECT_LE(stats.inputSamples[i - 1].doneAtMs, sample.startedAtMs) << "inputs run one after the other";
            }
            if (brush == BrushId::Move) {
                EXPECT_EQ(sample.dabs, i == 0 ? 0u : 1u);
            }
            dabs += sample.dabs;
        }
        EXPECT_EQ(dabs, stats.strokeDabCount);
        EXPECT_GT(dabs, 5u);
    }
}

TEST(AppController, ListenerIsCalledAndCanBeRemoved)
{
    app::AppController controller({.sphereSubdivisions = 4});
    std::atomic<int> notifications{0};
    controller.setChangeListener([&notifications] { notifications.fetch_add(1); });

    stroke(controller, -0.2f, 0.0f, 0.2f, 0.0f, 4);
    controller.waitUntilIdle();
    EXPECT_GE(notifications.load(), 3) << "begin, updates and end each report a change";

    controller.setChangeListener({});
    const int afterRemoval = notifications.load();
    controller.undo();
    controller.waitUntilIdle();
    EXPECT_EQ(notifications.load(), afterRemoval);
}

TEST(AppController, ReaderSeesConsistentMeshWhileSculpting)
{
    // A second thread plays the renderer: it repeatedly locks the document,
    // takes the pending pages and reads the vertex data, while strokes run. It
    // keeps its own copy of the mesh up to date from the dirty pages alone; at
    // the end that copy must equal the real mesh, which proves no change was
    // lost or torn.
    app::AppController controller({.sphereSubdivisions = 6});
    const u32 vertexCount = controller.mesh().vertexCount();
    std::vector<glm::vec3> mirror(controller.mesh().positions().begin(), controller.mesh().positions().end());

    std::atomic<bool> stop{false};
    std::atomic<int> readsWithChanges{0};
    const auto syncMirror = [&] {
        const std::scoped_lock lock(controller.documentMutex());
        geo::DirtyVertexSet& pending = controller.pendingVertexUpdates();
        if (pending.empty()) {
            return;
        }
        const auto normals = controller.mesh().normals();
        pending.forEachRun([&](u32 first, u32 count) {
            controller.mesh().copyPositions(first, count, mirror.data() + first);
            for (u32 v = first; v < first + count; ++v) {
                // Normals are maintained with the positions: always unit length.
                const float length = glm::length(normals[v]);
                if (length < 0.99f || length > 1.01f) {
                    ADD_FAILURE() << "normal of vertex " << v << " has length " << length;
                }
            }
        });
        pending.clear();
        readsWithChanges.fetch_add(1);
    };
    std::thread reader([&] {
        while (!stop.load()) {
            syncMirror();
            std::this_thread::yield();
        }
    });

    controller.setRadius(0.3f);
    const BrushId brushSequence[] = {BrushId::Draw, BrushId::Smooth, BrushId::Move, BrushId::Draw};
    for (int round = 0; round < 8; ++round) {
        controller.setActiveBrush(brushSequence[round % 4]);
        const float y = -0.35f + 0.1f * static_cast<float>(round);
        stroke(controller, -0.4f, y, 0.4f, y + 0.05f, 40);
        if (round == 5) {
            controller.undo();
            controller.undo();
            controller.redo();
        }
    }
    controller.waitUntilIdle();
    stop.store(true);
    reader.join();
    syncMirror(); // whatever was published after the reader's last pass

    EXPECT_GT(readsWithChanges.load(), 1);
    const auto positions = controller.mesh().positions();
    u32 mismatches = 0;
    for (u32 v = 0; v < vertexCount; ++v) {
        mismatches += mirror[v] != positions[v] ? 1u : 0u;
    }
    EXPECT_EQ(mismatches, 0u);
}

TEST(AppController, DestructionWithQueuedWorkIsClean)
{
    // Destroying the controller while input is still queued must not hang or crash.
    for (int i = 0; i < 5; ++i) {
        app::AppController controller({.sphereSubdivisions = 5});
        ASSERT_TRUE(controller.beginStroke(rayAt(-0.3f, 0.0f)));
        for (int k = 1; k <= 50; ++k) {
            controller.updateStroke(rayAt(-0.3f + 0.012f * static_cast<float>(k), 0.0f));
        }
        controller.endStroke();
    }
    SUCCEED();
}
