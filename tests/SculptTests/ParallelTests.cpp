// Reference tests: the multithreaded code paths must produce exactly the same
// mesh as the single-threaded ones. "Exactly" means bit-identical floats.

#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace se;
using sculpt::BrushId;
using sculpt::BrushSettings;

namespace {

struct Scene {
    explicit Scene(int level) : mesh(geo::makeIcosphere(level)), stroke(mesh, bvh) { bvh.build(mesh); }

    geo::Mesh mesh;
    geo::ChunkedBvh bvh;
    sculpt::StrokeProcessor stroke;
};

geo::Ray rayAt(float x, float y)
{
    return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}};
}

/// A stroke long enough to overlap itself, with a footprint above the parallel grain.
sculpt::StrokeDelta runStroke(Scene& scene, const sculpt::Brush& brush, const BrushSettings& settings)
{
    EXPECT_TRUE(scene.stroke.begin(brush, settings, rayAt(-0.25f, -0.1f)));
    for (int i = 1; i <= 24; ++i) {
        const float t = static_cast<float>(i) / 24.0f;
        scene.stroke.update(rayAt(-0.25f + 0.5f * t, -0.1f + 0.2f * std::sin(t * 6.0f)));
    }
    EXPECT_GT(scene.stroke.stats().maxDabVertexCount, 2 * sculpt::kBrushParallelGrain)
        << "footprint too small to exercise the parallel path";
    auto delta = scene.stroke.end();
    EXPECT_TRUE(delta.has_value());
    return delta ? std::move(*delta) : sculpt::StrokeDelta{};
}

using test::bitwiseEqual;

void expectIdenticalResults(BrushId brushId, const BrushSettings& settings, int level)
{
    const sculpt::BrushLibrary brushes;
    core::JobSystem jobs(5);

    Scene serial(level);
    Scene parallel(level);
    parallel.stroke.setJobSystem(&jobs);

    // Give the surface some relief first so Smooth has something to do. This
    // also runs through both paths.
    const BrushSettings relief{.radius = 0.55f, .strength = 0.3f, .invert = false};
    runStroke(serial, brushes.brush(BrushId::Draw), relief);
    runStroke(parallel, brushes.brush(BrushId::Draw), relief);

    const sculpt::StrokeDelta serialDelta = runStroke(serial, brushes.brush(brushId), settings);
    const sculpt::StrokeDelta parallelDelta = runStroke(parallel, brushes.brush(brushId), settings);

    EXPECT_TRUE(bitwiseEqual(serial.mesh.positions(), parallel.mesh.positions())) << "positions differ";
    EXPECT_TRUE(bitwiseEqual(serial.mesh.normals(), parallel.mesh.normals())) << "normals differ";

    // Undo records must match too (same vertices, same order, same values).
    EXPECT_EQ(serialDelta.vertices, parallelDelta.vertices);
    EXPECT_TRUE(bitwiseEqual(serialDelta.before, parallelDelta.before));
    EXPECT_TRUE(bitwiseEqual(serialDelta.after, parallelDelta.after));

    // And so must the acceleration structure.
    ASSERT_GT(serial.bvh.chunkCount(), 8u) << "the strokes should cross several chunks";
    EXPECT_TRUE(serial.bvh.sameBounds(parallel.bvh)) << "BVH boxes differ";

    // Sanity: the result is also *correct*, not merely equal.
    std::mt19937 rng(11);
    for (int i = 0; i < 100; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const auto expected = test::bruteForceRaycast(parallel.mesh, ray);
        const auto actual = parallel.bvh.raycast(parallel.mesh, ray);
        ASSERT_EQ(expected.has_value(), actual.has_value());
        if (expected) {
            ASSERT_NEAR(*expected, actual->distance, 1e-4f);
        }
    }
}

} // namespace

TEST(ParallelReference, DrawMatchesSingleThreaded)
{
    expectIdenticalResults(BrushId::Draw, {.radius = 0.6f, .strength = 0.7f, .invert = false}, 7);
}

TEST(ParallelReference, SmoothDenseModeMatchesSingleThreaded)
{
    // Level 7 with a large radius: many vertices per kernel cell -> dense mode.
    expectIdenticalResults(BrushId::Smooth, {.radius = 0.6f, .strength = 0.8f, .invert = false}, 7);
}

// (Smooth's sparse mode only occurs when a dab holds a few hundred vertices at
// most, which is below the parallel grain, so it always runs on one thread.)

TEST(ParallelReference, MoveMatchesSingleThreaded)
{
    expectIdenticalResults(BrushId::Move, {.radius = 0.6f, .strength = 1.0f, .invert = false}, 7);
}
