// Undo and redo with the normals computed before the document lock: the
// history entry's positions are staged over the mesh (PositionStaging, the
// dab staging pages), the normals of the affected vertices are computed from
// that view, and under the lock only positions, normals, BVH and dirty pages
// are written. These tests check that
// - the result equals computing the normals under the lock, bit for bit, at
//   any thread count, and undo/redo stay exact;
// - a reader locking the document during undo and redo only ever sees the
//   states before or after a command, consistent with each other (normals,
//   BVH boxes and cones match the positions), and its mirror built from the
//   dirty pages equals the mesh.

#include "Application/AppController.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/NormalCone.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_set>
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

std::string checkConsistent(const geo::Mesh& mesh, const geo::ChunkedBvh& bvh)
{
    std::vector<u32> all(mesh.vertexCount());
    std::iota(all.begin(), all.end(), 0u);
    std::vector<glm::vec3> expected(mesh.vertexCount());
    mesh.computeNormals(all, mesh.positions(), expected);
    if (!test::bitwiseEqual(expected, mesh.normals())) {
        return "normals do not match the positions";
    }
    // With incremental cones, a widened (stale) cone only has to hold every normal.
    geo::ChunkedBvh refitted = bvh;
    refitted.refit(mesh);
    if (!refitted.sameBounds(bvh, false)) {
        return "BVH boxes do not match the positions";
    }
    const u32 capacity = 1u << bvh.chunkTriangleShift();
    for (u32 chunk = 0; chunk < bvh.chunkCount(); ++chunk) {
        if (!bvh.chunkConeStale(chunk) && !(bvh.chunkCone(chunk) == refitted.chunkCone(chunk))) {
            return "a cone that is not stale differs from the exact one";
        }
        const u32 first = chunk * capacity;
        const u32 count = std::min(capacity, mesh.triangleCount() - first);
        if (!geo::coneContainsNormals(bvh.chunkCone(chunk), mesh, first, count)) {
            return "a normal cone misses one of its triangles";
        }
    }
    return {};
}

app::AppConfig configFor(int level, int helpers, bool normalsBeforeLock, bool denseRefit = false,
                         bool incrementalCones = false)
{
    app::AppConfig config;
    config.denseBoxRefit = denseRefit;
    config.incrementalCones = incrementalCones;
    config.sphereSubdivisions = level;
    config.jobThreads = helpers;
    config.historyNormalsBeforeLock = normalsBeforeLock;
    return config;
}

/// Four overlapping strokes (Draw, Smooth, Move, Draw inverted).
void sculptStrokes(app::AppController& controller, float radius)
{
    const BrushId brushes[] = {BrushId::Draw, BrushId::Smooth, BrushId::Move, BrushId::Draw};
    for (int s = 0; s < 4; ++s) {
        controller.setActiveBrush(brushes[s]);
        controller.setRadius(radius);
        const float y = -0.2f + 0.1f * static_cast<float>(s);
        const auto ray = [&](int i) {
            const float t = static_cast<float>(i) / 16.0f;
            return geo::Ray{{-0.4f + 0.8f * t, y + 0.1f * t, 5.0f}, {0.0f, 0.0f, -1.0f}};
        };
        ASSERT_TRUE(controller.beginStroke(ray(0)));
        for (int i = 1; i <= 16; ++i) {
            controller.updateStroke(ray(i));
        }
        controller.endStroke();
        controller.waitUntilIdle();
    }
}

/// The history steps run by both tests: undo 4, redo 2, undo 1, redo 3.
constexpr int kHistory[] = {-1, -1, -1, -1, +1, +1, -1, +1, +1, +1};

struct Snapshot {
    u64 hash = 0;
    std::unique_ptr<geo::ChunkedBvh> bvh;
};

std::vector<Snapshot> runHistory(app::AppController& controller)
{
    std::vector<Snapshot> result;
    const auto take = [&] {
        controller.waitUntilIdle();
        const std::scoped_lock lock(controller.documentMutex());
        result.push_back({hashMesh(controller.mesh()), std::make_unique<geo::ChunkedBvh>(controller.spatialIndex())});
    };
    take();
    for (const int step : kHistory) {
        if (step < 0) {
            controller.undo();
        } else {
            controller.redo();
        }
        take();
    }
    return result;
}

} // namespace

TEST(HistoryStaging, NormalsBeforeTheLockMatchNormalsUnderIt)
{
    // Level 7, radius 0.5: undo steps of tens of thousands of vertices, so the
    // normals run on the helpers in several pieces.
    std::vector<Snapshot> reference;
    u64 initialHash = 0;
    {
        app::AppController controller(configFor(7, 0, false));
        initialHash = hashMesh(controller.mesh());
        sculptStrokes(controller, 0.5f);
        reference = runHistory(controller);
        const std::scoped_lock lock(controller.documentMutex());
        EXPECT_EQ(checkConsistent(controller.mesh(), controller.spatialIndex()), "");
    }
    ASSERT_EQ(reference.size(), std::size(kHistory) + 1);
    EXPECT_EQ(reference[4].hash, initialHash) << "undo of everything is not exact";
    EXPECT_EQ(reference.back().hash, reference.front().hash) << "redo of everything is not exact";

    for (const int helpers : {0, 1, 3, 11}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        app::AppController controller(configFor(7, helpers, true));
        sculptStrokes(controller, 0.5f);
        const std::vector<Snapshot> staged = runHistory(controller);
        ASSERT_EQ(staged.size(), reference.size());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            EXPECT_EQ(staged[i].hash, reference[i].hash) << "step " << i;
            EXPECT_TRUE(staged[i].bvh->sameBounds(*reference[i].bvh)) << "step " << i;
        }
        const std::scoped_lock lock(controller.documentMutex());
        EXPECT_EQ(checkConsistent(controller.mesh(), controller.spatialIndex()), "");
    }
}

// Run with the incremental box refit and with the dense one (DenseRefitTests).
class HistoryStagingReader : public testing::TestWithParam<int> {};

TEST_P(HistoryStagingReader, ConcurrentReaderSeesOnlyStatesBetweenCommands)
{
    constexpr int kLevel = 6;
    // Reference: the states after every command, normals under the lock.
    std::unordered_set<u64> states;
    {
        app::AppController controller(configFor(kLevel, 0, false));
        sculptStrokes(controller, 0.35f);
        for (const Snapshot& snapshot : runHistory(controller)) {
            states.insert(snapshot.hash);
        }
    }
    ASSERT_GE(states.size(), 5u);

    app::AppController controller(configFor(kLevel, 3, true, GetParam() != 0, GetParam() == 2));
    sculptStrokes(controller, 0.35f);
    std::vector<glm::vec3> mirror(controller.mesh().positions().begin(), controller.mesh().positions().end());
    {
        // Strokes ran before the reader started: take their dirty pages now.
        const std::scoped_lock lock(controller.documentMutex());
        controller.pendingVertexUpdates().clear();
    }
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> unknownStates{0};
    std::atomic<int> mirrorMismatches{0};
    std::atomic<int> inconsistent{0};
    std::unordered_set<u64> seen;
    const auto readOnce = [&](bool fullCheck) {
        const std::scoped_lock lock(controller.documentMutex());
        const geo::Mesh& mesh = controller.mesh();
        const u64 hash = hashMesh(mesh);
        seen.insert(hash);
        if (!states.contains(hash)) {
            unknownStates.fetch_add(1);
        }
        geo::DirtyVertexSet& pending = controller.pendingVertexUpdates();
        pending.forEachRun([&](u32 first, u32 count) { mesh.copyPositions(first, count, mirror.data() + first); });
        pending.clear();
        const auto positions = mesh.positions();
        for (u32 v = 0; v < mesh.vertexCount(); ++v) {
            if (mirror[v] != positions[v]) {
                mirrorMismatches.fetch_add(1);
                break;
            }
        }
        if (fullCheck) {
            const std::string problem = checkConsistent(mesh, controller.spatialIndex());
            if (!problem.empty()) {
                ADD_FAILURE() << problem;
                inconsistent.fetch_add(1);
            }
        }
        reads.fetch_add(1);
    };
    std::thread reader([&] {
        for (int i = 0; !stop.load(); ++i) {
            readOnce(i % 2 == 0);
            std::this_thread::yield();
        }
    });
    // Several rounds of the history steps, queued without waiting, so the
    // reader runs while undo and redo prepare and commit.
    for (int round = 0; round < 3; ++round) {
        for (const int step : kHistory) {
            if (step < 0) {
                controller.undo();
            } else {
                controller.redo();
            }
        }
        // kHistory ends where it began (net zero steps).
        controller.waitUntilIdle();
    }
    stop.store(true);
    reader.join();
    readOnce(true);

    EXPECT_EQ(unknownStates.load(), 0) << "a reader saw a state in the middle of an undo or redo";
    EXPECT_EQ(mirrorMismatches.load(), 0) << "a published change was missing from the dirty vertices";
    EXPECT_EQ(inconsistent.load(), 0);
    EXPECT_GT(reads.load(), 20);
    EXPECT_GE(seen.size(), 3u) << "the reader saw too few history states to test anything";
}

// 0: incremental box refit, 1: dense, 2: dense with incremental cones.
INSTANTIATE_TEST_SUITE_P(BoxRefit, HistoryStagingReader, testing::Values(0, 1, 2),
                         [](const testing::TestParamInfo<int>& param) {
                             return param.param == 0 ? "Incremental" : param.param == 1 ? "Dense" : "DenseIncrementalCones";
                         });
