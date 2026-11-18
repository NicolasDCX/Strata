// Incremental normal cones: a refit that changed at most a set share of a
// chunk's leaves widens the chunk's cone around its old axis by the new
// normals of the changed triangles, or turns the chunk's culling off, and a
// background rebuild later makes the cone exact again. Checked here:
// - every cone holds every normal of its chunk after every edit, so chunk
//   culling never drops a triangle that faces the eye (tested directly too);
// - boxes, picks and optimize steps are those of exact cones, bit for bit;
// - cones that are not stale are the exact ones, and after the rebuild every
//   cone is, so the result equals exact cones bit for bit;
// - through the controller, strokes and undo/redo give the same mesh, boxes,
//   picks and (once the sculpt thread is idle, i.e. rebuilt) cones.

#include "Application/AppController.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/EditRegion.h"
#include "Geometry/Icosphere.h"
#include "Geometry/NormalCone.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
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

/// Eyes on a ring and at the poles, near and far.
std::vector<glm::vec3> eyes()
{
    std::vector<glm::vec3> result;
    for (const float distance : {1.6f, 4.0f}) {
        for (int i = 0; i < 8; ++i) {
            const float a = 0.785398f * static_cast<float>(i) + 0.1f;
            result.emplace_back(distance * std::cos(a), 0.3f * distance * std::sin(2.0f * a), distance * std::sin(a));
        }
        result.emplace_back(0.0f, distance, 0.0f);
        result.emplace_back(0.0f, -distance, 0.0f);
    }
    return result;
}

/// Empty, or what is wrong: a cone misses a normal, or a chunk that the cone
/// test rejects from some eye has a triangle facing that eye.
std::string checkConservative(const geo::ChunkedBvh& bvh, const geo::Mesh& mesh, const std::vector<glm::vec3>& from)
{
    const u32 capacity = 1u << bvh.chunkTriangleShift();
    const auto positions = mesh.positions();
    for (u32 chunk = 0; chunk < bvh.chunkCount(); ++chunk) {
        const u32 first = chunk * capacity;
        const u32 count = std::min(capacity, mesh.triangleCount() - first);
        if (!geo::coneContainsNormals(bvh.chunkCone(chunk), mesh, first, count)) {
            return "chunk " + std::to_string(chunk) + ": the cone misses a normal";
        }
        for (const glm::vec3& eye : from) {
            if (!geo::coneFacesAway(bvh.chunkCone(chunk), bvh.chunk(chunk).bounds(), eye)) {
                continue;
            }
            for (u32 t = first; t < first + count; ++t) {
                const auto& tri = mesh.triangle(t);
                const glm::vec3 n = glm::cross(positions[tri[1]] - positions[tri[0]], positions[tri[2]] - positions[tri[0]]);
                for (int k = 0; k < 3; ++k) {
                    if (glm::dot(n, positions[tri[k]] - eye) <= 0.0f) {
                        return "chunk " + std::to_string(chunk) + " is culled but a triangle faces an eye";
                    }
                }
            }
        }
    }
    return {};
}

/// Empty, or what differs: boxes everywhere, and the cones of chunks that are
/// not stale, against the exact-cone tree.
std::string checkAgainstExact(const geo::ChunkedBvh& incremental, const geo::ChunkedBvh& exact)
{
    if (!incremental.sameBounds(exact, false)) {
        return "boxes differ";
    }
    for (u32 chunk = 0; chunk < exact.chunkCount(); ++chunk) {
        if (exact.chunkConeStale(chunk)) {
            return "exact cones have a stale chunk";
        }
        if (!incremental.chunkConeStale(chunk) && !(incremental.chunkCone(chunk) == exact.chunkCone(chunk))) {
            return "chunk " + std::to_string(chunk) + ": cone not stale but not exact";
        }
    }
    return {};
}

/// Moves every vertex within `radius` of `center` (a random tilt, so normals
/// turn) and refits both trees from the region's triangles.
void editBoth(geo::Mesh& mesh, geo::ChunkedBvh& a, geo::ChunkedBvh& b, const glm::vec3& center, float radius,
              std::mt19937& random)
{
    std::vector<u32> moved;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        const glm::vec3 d = mesh.positions()[v] - center;
        if (glm::dot(d, d) < radius * radius) {
            moved.push_back(v);
        }
    }
    const glm::vec3 direction = glm::normalize(glm::vec3(static_cast<float>(random() % 7) - 3.0f,
                                                         static_cast<float>(random() % 7) - 3.0f, 2.0f));
    for (const u32 v : moved) {
        const glm::vec3 d = mesh.positions()[v] - center;
        const float falloff = 1.0f - glm::dot(d, d) / (radius * radius);
        mesh.mutablePositions()[v] += (0.15f * radius * falloff * static_cast<float>(1 + random() % 4)) * direction;
    }
    geo::EditRegion region;
    region.collect(mesh, moved);
    a.markTriangles(region.triangles());
    a.finishMarkedRefit(mesh);
    b.markTriangles(region.triangles());
    b.finishMarkedRefit(mesh);
}

} // namespace

TEST(IncrementalCones, WidenedConesAreConservativeAndRebuildToExact)
{
    const std::vector<glm::vec3> from = eyes();
    for (const u32 fallback : {50u, 100u}) {
        for (const u32 shift : {6u, 8u, 12u}) {
            SCOPED_TRACE(testing::Message() << "chunk shift " << shift << ", fallback " << fallback << "%");
            geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
            geo::ChunkedBvh exact;
            exact.build(mesh, shift, nullptr, true);
            exact.setDenseRefit(true);
            geo::ChunkedBvh widened = exact;
            widened.setIncrementalCones(true, fallback);
            std::mt19937 random(5);
            u32 exactCones = 0, widenedCones = 0, disabledCones = 0;
            const float radii[] = {0.01f, 0.05f, 0.15f, 0.4f, 0.9f, 2.5f, 0.03f, 0.6f, 0.08f, 0.2f};
            for (int edit = 0; edit < static_cast<int>(std::size(radii)); ++edit) {
                SCOPED_TRACE(testing::Message() << "edit " << edit);
                const float angle = 0.7f * static_cast<float>(edit);
                const glm::vec3 center(std::sin(angle) * 0.8f, std::cos(angle) * 0.6f, 0.0f);
                editBoth(mesh, exact, widened, center, radii[edit], random);
                const auto& timings = widened.lastRefitTimings();
                exactCones += timings.conesExact;
                widenedCones += timings.conesWidened;
                disabledCones += timings.conesDisabled;
                EXPECT_EQ(timings.conesExact + timings.conesWidened + timings.conesDisabled, timings.chunks);
                EXPECT_EQ(exact.lastRefitTimings().conesExact, exact.lastRefitTimings().chunks);
                ASSERT_EQ(checkConservative(widened, mesh, from), "");
                ASSERT_EQ(checkAgainstExact(widened, exact), "");
                EXPECT_EQ(pickAll(widened, mesh), pickAll(exact, mesh));
                // Part of the rebuild (a step of one chunk) and of the optimize
                // work, so later edits meet both half done.
                if (widened.hasStaleCones() && edit % 2 == 0) {
                    widened.computeStaleCones(mesh, 1);
                    widened.publishStaleCones();
                    ASSERT_EQ(checkAgainstExact(widened, exact), "") << "after a rebuild step";
                }
                for (int step = 0; step < 4 && exact.needsOptimize(); ++step) {
                    ASSERT_EQ(widened.needsOptimize(), exact.needsOptimize());
                    EXPECT_EQ(widened.optimize(mesh, 1024), exact.optimize(mesh, 1024));
                    ASSERT_EQ(checkAgainstExact(widened, exact), "") << "after optimize step " << step;
                }
            }
            EXPECT_GT(widenedCones, 0u) << "no cone was widened";
            if (fallback == 50u) {
                EXPECT_GT(exactCones, 0u) << "no chunk fell back to an exact cone";
            } else {
                EXPECT_EQ(exactCones, 0u) << "a chunk fell back although the fallback is off";
            }
            static_cast<void>(disabledCones);
            // The rebuild in batches, then everything is exact: equal bit for bit.
            ASSERT_TRUE(widened.hasStaleCones());
            while (widened.hasStaleCones()) {
                widened.computeStaleCones(mesh, 3);
                widened.publishStaleCones();
            }
            EXPECT_EQ(widened.staleConeCount(), 0u);
            EXPECT_TRUE(widened.sameBounds(exact));
            geo::ChunkedBvh fresh = exact;
            fresh.refit(mesh);
            EXPECT_TRUE(fresh.sameBounds(widened));
        }
    }
}

TEST(IncrementalCones, ConeTurnsCullingOffWhenItWouldBecomeTooWide)
{
    // A chunk of 64 triangles on the sphere, folded over: some of its normals
    // turn by more than 90 degrees, so no cone around the old axis can hold them.
    geo::Mesh mesh = geo::makeIcosphere(5, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 6, nullptr, true);
    bvh.setIncrementalCones(true, 100);
    ASSERT_TRUE(bvh.chunkCone(3).usable());
    // Flip one triangle of chunk 3 by pulling one of its vertices through the surface.
    const u32 triangle = 3u * 64u + 10u;
    const auto tri = mesh.triangle(triangle);
    const glm::vec3 p = mesh.positions()[tri[0]];
    mesh.mutablePositions()[tri[0]] = p * 0.5f;
    std::vector<u32> moved{tri[0]};
    geo::EditRegion region;
    region.collect(mesh, moved);
    bvh.markTriangles(region.triangles());
    bvh.finishMarkedRefit(mesh);
    EXPECT_GT(bvh.lastRefitTimings().conesDisabled, 0u);
    EXPECT_FALSE(bvh.chunkCone(3).usable());
    EXPECT_TRUE(bvh.chunkConeStale(3));
    EXPECT_EQ(checkConservative(bvh, mesh, eyes()), "");
    // The rebuild gives exactly what a full recompute gives.
    while (bvh.hasStaleCones()) {
        bvh.computeStaleCones(mesh, 8);
        bvh.publishStaleCones();
    }
    geo::ChunkedBvh fresh = bvh;
    fresh.refit(mesh);
    EXPECT_TRUE(fresh.sameBounds(bvh));
}

namespace {

struct Step {
    u64 hash = 0;
    std::unique_ptr<geo::ChunkedBvh> bvh;
    std::vector<u32> picks;
};

/// Draw, Smooth, Move and a small Draw through the controller, then undo 4,
/// redo 2, undo 1, redo 3; a snapshot after every stroke and command, taken
/// once the sculpt thread is idle (cones rebuilt).
std::vector<Step> runController(bool incremental, int helpers, u64* rebuiltChunks, u32* widened, bool reuseNormals = false)
{
    app::AppConfig config;
    config.reuseConeNormals = reuseNormals;
    config.sphereSubdivisions = 7;
    config.jobThreads = helpers;
    config.incrementalCones = incremental;
    app::AppController controller(config);
    std::vector<Step> steps;
    const auto take = [&] {
        controller.waitUntilIdle();
        const std::scoped_lock lock(controller.documentMutex());
        EXPECT_FALSE(controller.spatialIndex().hasStaleCones()) << "idle with stale cones";
        steps.push_back({hashMesh(controller.mesh()), std::make_unique<geo::ChunkedBvh>(controller.spatialIndex()),
                         pickAll(controller.spatialIndex(), controller.mesh())});
        *widened += controller.stats().lastHistory.refit.conesWidened + controller.stats().lastHistory.refit.conesDisabled;
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
    *rebuiltChunks = controller.stats().coneRebuildChunks;
    return steps;
}

} // namespace

TEST(IncrementalCones, StrokesAndUndoRedoThroughControllerMatchExactCones)
{
    u64 unusedChunks = 0;
    u32 unusedWidened = 0;
    const std::vector<Step> reference = runController(false, 0, &unusedChunks, &unusedWidened);
    ASSERT_EQ(reference.size(), 14u);
    EXPECT_EQ(reference[13].hash, reference[3].hash) << "redo of everything is not exact";
    for (const int helpers : {0, 3, 11}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        u64 rebuiltChunks = 0;
        u32 widened = 0;
        const std::vector<Step> incremental = runController(true, helpers, &rebuiltChunks, &widened);
        ASSERT_EQ(incremental.size(), reference.size());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            EXPECT_EQ(incremental[i].hash, reference[i].hash) << "step " << i;
            EXPECT_TRUE(incremental[i].bvh->sameBounds(*reference[i].bvh)) << "step " << i;
            EXPECT_EQ(incremental[i].picks, reference[i].picks) << "step " << i;
        }
        EXPECT_GT(rebuiltChunks, 0u) << "no cone was rebuilt in the background";
        EXPECT_GT(widened, 0u) << "undo and redo never widened a cone";
    }
    // Exact cones that reuse the first pass's face normals: identical too.
    for (const bool incremental : {false, true}) {
        SCOPED_TRACE(testing::Message() << "reused face normals, " << (incremental ? "incremental" : "exact") << " cones");
        u64 rebuiltChunks = 0;
        u32 widened = 0;
        const std::vector<Step> reused = runController(incremental, 3, &rebuiltChunks, &widened, true);
        ASSERT_EQ(reused.size(), reference.size());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            EXPECT_EQ(reused[i].hash, reference[i].hash) << "step " << i;
            EXPECT_TRUE(reused[i].bvh->sameBounds(*reference[i].bvh)) << "step " << i;
            EXPECT_EQ(reused[i].picks, reference[i].picks) << "step " << i;
        }
    }
}
