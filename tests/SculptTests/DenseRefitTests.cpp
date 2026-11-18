// Dense box refit: a chunk whose refit queued at least half of its leaves
// recomputes every leaf, then every inner node, in node order, instead of the
// queued nodes level by level. Boxes are minima and maxima of the same values,
// so the result must equal the incremental refit bit for bit: every node box,
// every cone, the top level, picking, the background optimize steps that
// follow, strokes and undo/redo through the controller.

#include "Application/AppController.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/EditRegion.h"
#include "Geometry/Icosphere.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <random>
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

/// Moves every vertex within `radius` of `center` (on the unit sphere) along z
/// and refits both trees with the region's triangles.
void editBoth(geo::Mesh& mesh, geo::ChunkedBvh& incremental, geo::ChunkedBvh& dense, const glm::vec3& center, float radius,
              std::mt19937& random)
{
    std::vector<u32> moved;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        const glm::vec3 d = mesh.positions()[v] - center;
        if (d.x * d.x + d.y * d.y + d.z * d.z < radius * radius) {
            moved.push_back(v);
        }
    }
    for (const u32 v : moved) {
        mesh.mutablePositions()[v] += glm::vec3(0.0f, 0.0f, 0.002f * static_cast<float>(1 + random() % 9));
    }
    geo::EditRegion region;
    region.collect(mesh, moved);
    incremental.markTriangles(region.triangles());
    incremental.finishMarkedRefit(mesh);
    dense.markTriangles(region.triangles());
    dense.finishMarkedRefit(mesh);
}

} // namespace

TEST(DenseRefit, MatchesIncrementalRefitAtAnyCoverage)
{
    for (const u32 shift : {6u, 8u, 12u}) {
        SCOPED_TRACE(testing::Message() << "chunk shift " << shift);
        geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
        geo::ChunkedBvh incremental;
        incremental.build(mesh, shift, nullptr, true);
        geo::ChunkedBvh dense = incremental;
        dense.setDenseRefit(true);
        std::mt19937 random(11);
        u32 denseChunks = 0;
        u32 incrementalChunks = 0;
        // Edits from a few triangles to most of the sphere, so chunks are
        // refitted below, at and above the dense threshold; then half the
        // optimize work, so later edits meet partly rebuilt trees.
        const float radii[] = {0.01f, 0.05f, 0.15f, 0.4f, 0.9f, 2.5f, 0.03f, 0.6f};
        for (int edit = 0; edit < static_cast<int>(std::size(radii)); ++edit) {
            SCOPED_TRACE(testing::Message() << "edit " << edit);
            const float angle = 0.7f * static_cast<float>(edit);
            const glm::vec3 center(std::sin(angle) * 0.8f, std::cos(angle) * 0.6f, 0.0f);
            editBoth(mesh, incremental, dense, center, radii[edit], random);
            denseChunks += dense.lastRefitTimings().denseChunks;
            incrementalChunks += dense.lastRefitTimings().chunks - dense.lastRefitTimings().denseChunks;
            EXPECT_EQ(incremental.lastRefitTimings().denseChunks, 0u);
            ASSERT_TRUE(dense.sameBounds(incremental));
            EXPECT_EQ(pickAll(dense, mesh), pickAll(incremental, mesh));
            // Both equal a refit of everything from the positions.
            geo::ChunkedBvh fresh = incremental;
            fresh.refit(mesh);
            EXPECT_TRUE(fresh.sameBounds(dense));
            // The optimize queue is the same: same steps, same trees.
            for (int step = 0; step < 6 && (dense.needsOptimize() || incremental.needsOptimize()); ++step) {
                ASSERT_EQ(dense.needsOptimize(), incremental.needsOptimize());
                EXPECT_EQ(dense.optimize(mesh, 1024), incremental.optimize(mesh, 1024));
                ASSERT_TRUE(dense.sameBounds(incremental)) << "after optimize step " << step;
            }
        }
        while (dense.needsOptimize() || incremental.needsOptimize()) {
            ASSERT_EQ(dense.needsOptimize(), incremental.needsOptimize());
            EXPECT_EQ(dense.optimize(mesh, 4096), incremental.optimize(mesh, 4096));
            ASSERT_TRUE(dense.sameBounds(incremental));
        }
        EXPECT_EQ(pickAll(dense, mesh), pickAll(incremental, mesh));
        EXPECT_GT(denseChunks, 0u) << "no chunk took the dense sweep";
        EXPECT_GT(incrementalChunks, 0u) << "no chunk stayed below the dense threshold";
    }
}

namespace {

struct Step {
    u64 hash = 0;
    std::unique_ptr<geo::ChunkedBvh> bvh;
    std::vector<u32> picks;
};

/// Draw, Smooth, Move and a small Draw through the controller, then undo 4,
/// redo 2, undo 1, redo 3; a snapshot after every stroke and command.
std::vector<Step> runController(bool dense, int helpers, u32* denseChunksSeen)
{
    app::AppConfig config;
    config.sphereSubdivisions = 7;
    config.jobThreads = helpers;
    config.denseBoxRefit = dense;
    app::AppController controller(config);
    std::vector<Step> steps;
    const auto take = [&] {
        controller.waitUntilIdle();
        const std::scoped_lock lock(controller.documentMutex());
        steps.push_back({hashMesh(controller.mesh()), std::make_unique<geo::ChunkedBvh>(controller.spatialIndex()),
                         pickAll(controller.spatialIndex(), controller.mesh())});
        *denseChunksSeen += controller.stats().lastHistory.refit.denseChunks;
    };
    struct Plan {
        BrushId brush;
        float radius;
        float y;
    };
    const Plan plans[] = {{BrushId::Draw, 0.8f, -0.2f},
                          {BrushId::Smooth, 0.6f, 0.0f},
                          {BrushId::Move, 0.7f, 0.1f},
                          {BrushId::Draw, 0.08f, 0.2f}};
    for (const Plan& plan : plans) {
        controller.setActiveBrush(plan.brush);
        controller.setRadius(plan.radius);
        const auto ray = [&](int i) {
            const float t = static_cast<float>(i) / 12.0f;
            return geo::Ray{{-0.4f + 0.8f * t, plan.y + 0.1f * t, 5.0f}, {0.0f, 0.0f, -1.0f}};
        };
        EXPECT_TRUE(controller.beginStroke(ray(0)));
        for (int i = 1; i <= 12; ++i) {
            controller.updateStroke(ray(i));
        }
        controller.endStroke();
        take();
    }
    for (const int step : {-1, -1, -1, -1, +1, +1, -1, +1, +1, +1}) {
        if (step < 0) {
            controller.undo();
        } else {
            controller.redo();
        }
        take();
    }
    return steps;
}

} // namespace

TEST(DenseRefit, StrokesAndUndoRedoThroughControllerMatchIncremental)
{
    u32 unused = 0;
    const std::vector<Step> reference = runController(false, 0, &unused);
    ASSERT_EQ(reference.size(), 14u);
    EXPECT_EQ(reference[13].hash, reference[3].hash) << "redo of everything is not exact";
    for (const int helpers : {0, 3, 11}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        u32 denseChunks = 0;
        const std::vector<Step> dense = runController(true, helpers, &denseChunks);
        ASSERT_EQ(dense.size(), reference.size());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            EXPECT_EQ(dense[i].hash, reference[i].hash) << "step " << i;
            EXPECT_TRUE(dense[i].bvh->sameBounds(*reference[i].bvh)) << "step " << i;
            EXPECT_EQ(dense[i].picks, reference[i].picks) << "step " << i;
        }
        EXPECT_GT(denseChunks, 0u) << "undo and redo never took the dense sweep";
    }
}
