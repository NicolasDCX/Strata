// Sculpting with derived neighbours must give bit-identical results to the
// stored neighbour table, on one thread or several, with either adjacency
// layout, and undo/redo must stay exact across vertex pages and chunks.

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace se;
using sculpt::BrushId;
using sculpt::BrushSettings;

namespace {

geo::Ray rayAt(float x, float y)
{
    return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}};
}

struct Scene {
    Scene(geo::AdjacencyLayout adjacency, geo::NeighborStorage neighbors, core::JobSystem* jobs)
        : mesh(geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial,
                                  {.vertexPageShift = 8, .adjacency = adjacency, .neighbors = neighbors})),
          stroke(mesh, bvh)
    {
        bvh.build(mesh, 10, jobs, true); // small chunks: strokes cross many of them
        stroke.setJobSystem(jobs);
    }

    geo::Mesh mesh;
    geo::ChunkedBvh bvh;
    sculpt::StrokeProcessor stroke;
};

sculpt::StrokeDelta runStroke(Scene& scene, const sculpt::Brush& brush, const BrushSettings& settings)
{
    EXPECT_TRUE(scene.stroke.begin(brush, settings, rayAt(-0.25f, -0.1f)));
    for (int i = 1; i <= 24; ++i) {
        const float t = static_cast<float>(i) / 24.0f;
        scene.stroke.update(rayAt(-0.25f + 0.5f * t, -0.1f + 0.2f * std::sin(t * 6.0f)));
    }
    auto delta = scene.stroke.end();
    EXPECT_TRUE(delta.has_value());
    return delta ? std::move(*delta) : sculpt::StrokeDelta{};
}

void expectSameScene(const Scene& a, const Scene& b, const sculpt::StrokeDelta& da, const sculpt::StrokeDelta& db)
{
    EXPECT_TRUE(test::bitwiseEqual(a.mesh.positions(), b.mesh.positions())) << "positions differ";
    EXPECT_TRUE(test::bitwiseEqual(a.mesh.normals(), b.mesh.normals())) << "normals differ";
    EXPECT_EQ(da.vertices, db.vertices);
    EXPECT_TRUE(test::bitwiseEqual(da.before, db.before));
    EXPECT_TRUE(test::bitwiseEqual(da.after, db.after));
    EXPECT_TRUE(a.bvh.sameBounds(b.bvh)) << "BVH boxes differ";
}

void expectStorageAgrees(BrushId brushId, const BrushSettings& settings, bool largeFootprint = true)
{
    const sculpt::BrushLibrary brushes;
    core::JobSystem jobs(5);
    Scene stored(geo::AdjacencyLayout::Compact, geo::NeighborStorage::Stored, nullptr);
    Scene derived(geo::AdjacencyLayout::Compact, geo::NeighborStorage::Derived, nullptr);
    Scene derivedParallel(geo::AdjacencyLayout::Compact, geo::NeighborStorage::Derived, &jobs);
    Scene derivedWide(geo::AdjacencyLayout::Wide, geo::NeighborStorage::Derived, nullptr);

    const BrushSettings relief{.radius = 0.55f, .strength = 0.3f, .invert = false};
    for (Scene* scene : {&stored, &derived, &derivedParallel, &derivedWide}) {
        runStroke(*scene, brushes.brush(BrushId::Draw), relief);
    }
    const sculpt::StrokeDelta storedDelta = runStroke(stored, brushes.brush(brushId), settings);
    const sculpt::StrokeDelta derivedDelta = runStroke(derived, brushes.brush(brushId), settings);
    const sculpt::StrokeDelta parallelDelta = runStroke(derivedParallel, brushes.brush(brushId), settings);
    const sculpt::StrokeDelta wideDelta = runStroke(derivedWide, brushes.brush(brushId), settings);
    if (largeFootprint) {
        ASSERT_GT(storedDelta.vertices.size(), 2 * sculpt::kBrushParallelGrain) << "footprint too small for the parallel path";
    }
    expectSameScene(stored, derived, storedDelta, derivedDelta);
    expectSameScene(derived, derivedParallel, derivedDelta, parallelDelta);
    expectSameScene(derived, derivedWide, derivedDelta, wideDelta);
}

std::vector<glm::vec3> settledPositions(app::AppController& controller)
{
    controller.waitUntilIdle();
    return {controller.mesh().positions().begin(), controller.mesh().positions().end()};
}

std::vector<glm::vec3> settledNormals(app::AppController& controller)
{
    controller.waitUntilIdle();
    return {controller.mesh().normals().begin(), controller.mesh().normals().end()};
}

void controllerStroke(app::AppController& controller, BrushId brush, float x0, float y0, float x1, float y1)
{
    controller.setActiveBrush(brush);
    ASSERT_TRUE(controller.beginStroke(rayAt(x0, y0)));
    for (int i = 1; i <= 8; ++i) {
        const float t = static_cast<float>(i) / 8.0f;
        controller.updateStroke(rayAt(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
    }
    controller.endStroke();
}

} // namespace

TEST(DerivedNeighborsSculpt, DrawMatchesStoredNeighbors)
{
    expectStorageAgrees(BrushId::Draw, {.radius = 0.6f, .strength = 0.7f, .invert = false});
}

TEST(DerivedNeighborsSculpt, SmoothMatchesStoredNeighbors)
{
    expectStorageAgrees(BrushId::Smooth, {.radius = 0.6f, .strength = 0.8f, .invert = false}); // dense kernel + Laplacian
    expectStorageAgrees(BrushId::Smooth, {.radius = 0.2f, .strength = 0.8f, .invert = false}, false); // Laplacian passes only
    expectStorageAgrees(BrushId::Smooth, {.radius = 0.35f, .strength = 1.0f, .invert = false}); // most passes
}

TEST(DerivedNeighborsSculpt, MoveMatchesStoredNeighbors)
{
    expectStorageAgrees(BrushId::Move, {.radius = 0.5f, .strength = 1.0f, .invert = false});
}

TEST(DerivedNeighborsSculpt, UndoRedoIsExactAcrossPagesAndChunks)
{
    app::AppConfig storedConfig{.sphereSubdivisions = 5, .jobThreads = 3};
    storedConfig.meshLayout.vertexPageShift = 6; // 64 vertices per page
    storedConfig.chunkTriangleShift = 7;         // 128 triangles per chunk
    storedConfig.meshLayout.neighbors = geo::NeighborStorage::Stored;
    app::AppConfig derivedConfig = storedConfig;
    derivedConfig.meshLayout.neighbors = geo::NeighborStorage::Derived;
    app::AppController stored(storedConfig);
    app::AppController derived(derivedConfig);
    ASSERT_TRUE(stored.mesh().storesNeighbors());
    ASSERT_FALSE(derived.mesh().storesNeighbors());

    const std::vector<glm::vec3> original = settledPositions(derived);
    for (app::AppController* controller : {&stored, &derived}) {
        controllerStroke(*controller, BrushId::Smooth, -0.3f, 0.0f, 0.3f, 0.1f);
        controllerStroke(*controller, BrushId::Draw, -0.3f, 0.1f, 0.3f, -0.1f);
        controllerStroke(*controller, BrushId::Smooth, -0.2f, 0.0f, 0.2f, 0.0f);
        controllerStroke(*controller, BrushId::Move, 0.0f, 0.8f, 0.0f, 1.3f);
    }
    const std::vector<glm::vec3> sculpted = settledPositions(derived);
    EXPECT_NE(sculpted, original);
    EXPECT_TRUE(test::bitwiseEqual(settledPositions(stored), sculpted));
    EXPECT_TRUE(test::bitwiseEqual(settledNormals(stored), settledNormals(derived)));

    for (app::AppController* controller : {&stored, &derived}) {
        for (int i = 0; i < 4; ++i) {
            controller->undo();
        }
    }
    EXPECT_TRUE(test::bitwiseEqual(settledPositions(derived), original)) << "undo restores every vertex exactly";
    EXPECT_TRUE(test::bitwiseEqual(settledNormals(stored), settledNormals(derived)));

    for (app::AppController* controller : {&stored, &derived}) {
        for (int i = 0; i < 4; ++i) {
            controller->redo();
        }
    }
    EXPECT_TRUE(test::bitwiseEqual(settledPositions(derived), sculpted)) << "redo reapplies every vertex exactly";
    EXPECT_TRUE(test::bitwiseEqual(settledNormals(stored), settledNormals(derived)));
}
