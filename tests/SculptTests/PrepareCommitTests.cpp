// Prepare/commit dabs: a dab is computed without touching the mesh and then
// published as one update between the stroke processor's commit hooks. These
// tests check what that promises:
// - preparing a dab writes nothing a reader sees (mesh positions, normals, BVH);
// - after every commit, positions, normals, BVH boxes and normal cones agree;
// - a reader that locks the document while strokes run only ever sees states a
//   serial, single-threaded run passes through between two dabs, and the dirty
//   vertices it collects describe every change;
// - undo/redo still restores everything exactly.

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Geometry/NormalCone.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"
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
using sculpt::BrushSettings;

namespace {

geo::Ray rayAt(float x, float y)
{
    return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}};
}

/// FNV-1a over the bits of every position and normal.
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

/// Normals, BVH boxes and normal cones all describe the current positions.
/// Returns an empty string, or what is inconsistent.
std::string checkConsistent(const geo::Mesh& mesh, const geo::ChunkedBvh& bvh)
{
    // Every normal is what the positions give (bit for bit).
    std::vector<u32> all(mesh.vertexCount());
    std::iota(all.begin(), all.end(), 0u);
    std::vector<glm::vec3> expected(mesh.vertexCount());
    mesh.computeNormals(all, mesh.positions(), expected);
    if (!test::bitwiseEqual(expected, mesh.normals())) {
        return "normals do not match the positions";
    }
    // Every box is the one a full refit gives, and so is every cone.
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

struct StrokePlan {
    BrushId brush;
    float y;
};

/// The strokes both runs below perform: dab brushes along a line and a Move drag.
constexpr StrokePlan kStrokes[] = {
    {BrushId::Draw, -0.2f}, {BrushId::Smooth, -0.15f}, {BrushId::Move, 0.1f}, {BrushId::Draw, 0.05f}};
constexpr int kSamples = 30;
constexpr float kRadius = 0.3f;

geo::Ray sampleRay(const StrokePlan& plan, int i)
{
    const float t = static_cast<float>(i) / static_cast<float>(kSamples);
    return rayAt(-0.4f + 0.8f * t, plan.y + 0.1f * t);
}

} // namespace

TEST(PrepareCommit, PreparingADabWritesNothingReadersSee)
{
    // Between two commits (that is, while gathering, running the brush and
    // computing normals) the mesh and BVH must stay as the last commit left
    // them, and after every commit they must be consistent with each other.
    geo::Mesh mesh = geo::makeIcosphere(5);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 8, nullptr, true);
    core::JobSystem jobs(3);
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(&jobs);

    u64 publishedHash = hashMesh(mesh);
    geo::ChunkedBvh published = bvh;
    int enters = 0;
    int leaves = 0;
    bool inside = false;
    stroke.setCommitHooks(
        [&] {
            EXPECT_FALSE(inside);
            inside = true;
            ++enters;
            EXPECT_EQ(hashMesh(mesh), publishedHash) << "the mesh changed outside a commit";
            EXPECT_TRUE(published.sameBounds(bvh)) << "the BVH changed outside a commit";
        },
        [&] {
            EXPECT_TRUE(inside);
            inside = false;
            ++leaves;
            EXPECT_EQ(checkConsistent(mesh, bvh), "");
            publishedHash = hashMesh(mesh);
            published = bvh;
        });

    const sculpt::BrushLibrary brushes;
    for (const StrokePlan& plan : kStrokes) {
        BrushSettings settings = brushes.settings(plan.brush);
        settings.radius = kRadius;
        ASSERT_TRUE(stroke.begin(brushes.brush(plan.brush), settings, sampleRay(plan, 0)));
        for (int i = 1; i <= kSamples; ++i) {
            stroke.update(sampleRay(plan, i));
        }
        ASSERT_TRUE(stroke.end().has_value());
    }
    EXPECT_GT(enters, 40);
    EXPECT_EQ(enters, leaves);
}

TEST(PrepareCommit, BrushWritesStagingNotTheMesh)
{
    // A brush sees its own writes (and a later pass sees an earlier one's),
    // but the mesh only changes when the dab is committed.
    class CheckingBrush final : public sculpt::Brush {
    public:
        std::string_view name() const noexcept override { return "Checking"; }
        sculpt::StrokeMode mode() const noexcept override { return sculpt::StrokeMode::Dabs; }
        void apply(const sculpt::DabContext& ctx) const override
        {
            const auto mesh = ctx.mesh.positions();
            for (const u32 vertex : ctx.vertices) {
                const glm::vec3 staged = ctx.positions[vertex];
                ASSERT_EQ(std::memcmp(&staged, &mesh[vertex], sizeof(glm::vec3)), 0);
            }
            for (int pass = 0; pass < 2; ++pass) {
                for (std::size_t i = 0; i < ctx.vertices.size(); ++i) {
                    ctx.positions[ctx.vertices[i]] += ctx.normal * (0.01f * ctx.weights[i]);
                }
            }
            u32 moved = 0;
            for (const u32 vertex : ctx.vertices) {
                moved += ctx.positions[vertex] != mesh[vertex] ? 1u : 0u;
            }
            ++applications;
            movedTotal += moved;
        }
        mutable int applications = 0;
        mutable u32 movedTotal = 0;
    };

    geo::Mesh mesh = geo::makeIcosphere(4);
    geo::ChunkedBvh bvh;
    bvh.build(mesh);
    sculpt::StrokeProcessor stroke(mesh, bvh);
    const std::vector<glm::vec3> before(mesh.positions().begin(), mesh.positions().end());
    const CheckingBrush brush;
    ASSERT_TRUE(stroke.begin(brush, {.radius = 0.4f, .strength = 0.5f, .invert = false}, rayAt(0.0f, 0.0f)));
    ASSERT_EQ(brush.applications, 1);
    EXPECT_GT(brush.movedTotal, 10u);
    u32 changed = 0;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        changed += mesh.positions()[v] != before[v] ? 1u : 0u;
    }
    EXPECT_EQ(changed, brush.movedTotal); // committed after apply(): exactly what the brush moved
    stroke.end();
}

// Run with the incremental box refit and with the dense one (DenseRefitTests).
// 0: incremental box refit, 1: dense, 2: dense with incremental cones.
class PrepareCommitReader : public testing::TestWithParam<int> {};

TEST_P(PrepareCommitReader, ConcurrentReaderSeesOnlyStatesBetweenDabs)
{
    constexpr int kLevel = 6;

    // Reference: the same strokes on a serial stroke processor, recording the
    // state after every dab. Every state a reader may observe is in this set.
    std::unordered_set<u64> states;
    std::vector<u64> strokeEnds;
    u64 initialHash = 0;
    {
        geo::Mesh mesh = geo::makeIcosphere(kLevel, 1.0f, geo::VertexOrder::Spatial);
        geo::ChunkedBvh bvh;
        bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
        sculpt::StrokeProcessor stroke(mesh, bvh);
        stroke.setCommitHooks([] {}, [&] { states.insert(hashMesh(mesh)); });
        initialHash = hashMesh(mesh);
        states.insert(initialHash);
        const sculpt::BrushLibrary brushes;
        for (const StrokePlan& plan : kStrokes) {
            BrushSettings settings = brushes.settings(plan.brush);
            settings.radius = kRadius;
            ASSERT_TRUE(stroke.begin(brushes.brush(plan.brush), settings, sampleRay(plan, 0)));
            for (int i = 1; i <= kSamples; ++i) {
                stroke.update(sampleRay(plan, i));
            }
            ASSERT_TRUE(stroke.end().has_value());
            strokeEnds.push_back(hashMesh(mesh));
            // The controller tidies the BVH in the background between strokes,
            // in steps of the same size; the next stroke's raycasts use it.
            while (bvh.needsOptimize()) {
                bvh.optimize(mesh, 4096);
            }
        }
    }
    ASSERT_GT(states.size(), 60u);

    // The same strokes through the controller (sculpt thread plus helpers),
    // while a second thread locks the document as often as it can.
    app::AppConfig config;
    config.sphereSubdivisions = kLevel;
    config.denseBoxRefit = GetParam() != 0;
    config.incrementalCones = GetParam() == 2;
    app::AppController controller(config);
    ASSERT_EQ(hashMesh(controller.mesh()), initialHash);
    std::vector<glm::vec3> mirror(controller.mesh().positions().begin(), controller.mesh().positions().end());
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
        // Like the renderer: follow the published dirty vertices only.
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

    controller.setRadius(kRadius);
    std::vector<u64> controllerStrokeEnds;
    for (const StrokePlan& plan : kStrokes) {
        controller.setActiveBrush(plan.brush);
        controller.setRadius(kRadius);
        ASSERT_TRUE(controller.beginStroke(sampleRay(plan, 0)));
        for (int i = 1; i <= kSamples; ++i) {
            controller.updateStroke(sampleRay(plan, i));
        }
        controller.endStroke();
        controller.waitUntilIdle();
        const std::scoped_lock lock(controller.documentMutex());
        controllerStrokeEnds.push_back(hashMesh(controller.mesh()));
    }
    stop.store(true);
    reader.join();
    readOnce(true);

    EXPECT_EQ(controllerStrokeEnds, strokeEnds) << "threaded result differs from the serial one";
    EXPECT_EQ(unknownStates.load(), 0) << "a reader saw a state between two dabs' commits";
    EXPECT_EQ(mirrorMismatches.load(), 0) << "a published change was missing from the dirty vertices";
    EXPECT_EQ(inconsistent.load(), 0);
    EXPECT_GT(reads.load(), 20);
    // The reader saw states from the middle of strokes, not only between them.
    EXPECT_GT(seen.size(), std::size(kStrokes) + 1);

    // Exact undo, then redo, of everything, through the same lock.
    for (std::size_t i = 0; i < std::size(kStrokes); ++i) {
        controller.undo();
    }
    controller.waitUntilIdle();
    {
        const std::scoped_lock lock(controller.documentMutex());
        EXPECT_EQ(hashMesh(controller.mesh()), initialHash);
        EXPECT_EQ(checkConsistent(controller.mesh(), controller.spatialIndex()), "");
    }
    for (std::size_t i = 0; i < std::size(kStrokes); ++i) {
        controller.redo();
    }
    controller.waitUntilIdle();
    const std::scoped_lock lock(controller.documentMutex());
    EXPECT_EQ(hashMesh(controller.mesh()), strokeEnds.back());
    EXPECT_EQ(checkConsistent(controller.mesh(), controller.spatialIndex()), "");
}

INSTANTIATE_TEST_SUITE_P(BoxRefit, PrepareCommitReader, testing::Values(0, 1, 2),
                         [](const testing::TestParamInfo<int>& param) {
                             return param.param == 0 ? "Incremental" : param.param == 1 ? "Dense" : "DenseIncrementalCones";
                         });
