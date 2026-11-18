// Sculpting on a mesh with compact adjacency must give bit-identical results to
// the wide layout, on one thread or several, and undo/redo must stay exact.

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
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
    Scene(int level, geo::AdjacencyLayout adjacency, core::JobSystem* jobs)
        : mesh(geo::makeIcosphere(level, 1.0f, geo::VertexOrder::Spatial, {.vertexPageShift = 8, .adjacency = adjacency})),
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
    EXPECT_TRUE(a.bvh.hasNormalCones() && b.bvh.hasNormalCones());
}

void expectLayoutsAgree(BrushId brushId, const BrushSettings& settings, bool largeFootprint = true)
{
    const sculpt::BrushLibrary brushes;
    core::JobSystem jobs(5);
    Scene wideSerial(7, geo::AdjacencyLayout::Wide, nullptr);
    Scene compactSerial(7, geo::AdjacencyLayout::Compact, nullptr);
    Scene compactParallel(7, geo::AdjacencyLayout::Compact, &jobs);
    ASSERT_GT(compactSerial.mesh.wideAdjacencyRows(), 0u) << "the mesh should also hold u32 rows";

    const BrushSettings relief{.radius = 0.55f, .strength = 0.3f, .invert = false};
    for (Scene* scene : {&wideSerial, &compactSerial, &compactParallel}) {
        runStroke(*scene, brushes.brush(BrushId::Draw), relief);
    }
    const sculpt::StrokeDelta wideDelta = runStroke(wideSerial, brushes.brush(brushId), settings);
    const sculpt::StrokeDelta compactDelta = runStroke(compactSerial, brushes.brush(brushId), settings);
    const sculpt::StrokeDelta parallelDelta = runStroke(compactParallel, brushes.brush(brushId), settings);
    if (largeFootprint) {
        ASSERT_GT(wideDelta.vertices.size(), 2 * sculpt::kBrushParallelGrain) << "footprint too small for the parallel path";
    }

    expectSameScene(wideSerial, compactSerial, wideDelta, compactDelta);
    expectSameScene(compactSerial, compactParallel, compactDelta, parallelDelta);
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

TEST(CompactAdjacencySculpt, DrawMatchesWideLayout)
{
    expectLayoutsAgree(BrushId::Draw, {.radius = 0.6f, .strength = 0.7f, .invert = false});
}

TEST(CompactAdjacencySculpt, SmoothMatchesWideLayout)
{
    expectLayoutsAgree(BrushId::Smooth, {.radius = 0.6f, .strength = 0.8f, .invert = false}); // dense kernel + Laplacian
    expectLayoutsAgree(BrushId::Smooth, {.radius = 0.2f, .strength = 0.8f, .invert = false}, false); // Laplacian passes only
}

TEST(CompactAdjacencySculpt, MoveMatchesWideLayout)
{
    expectLayoutsAgree(BrushId::Move, {.radius = 0.5f, .strength = 1.0f, .invert = false});
}

TEST(CompactAdjacencySculpt, UndoRedoIsExactAndMatchesWideLayout)
{
    app::AppConfig wideConfig{.sphereSubdivisions = 5, .jobThreads = 3};
    wideConfig.meshLayout.adjacency = geo::AdjacencyLayout::Wide;
    app::AppConfig compactConfig = wideConfig;
    compactConfig.meshLayout.adjacency = geo::AdjacencyLayout::Compact;
    app::AppController wide(wideConfig);
    app::AppController compact(compactConfig);
    ASSERT_EQ(compact.mesh().layout().adjacency, geo::AdjacencyLayout::Compact);
    ASSERT_EQ(wide.mesh().layout().adjacency, geo::AdjacencyLayout::Wide);

    const std::vector<glm::vec3> original = settledPositions(compact);
    const std::vector<glm::vec3> originalNormals = settledNormals(compact);
    for (app::AppController* controller : {&wide, &compact}) {
        controllerStroke(*controller, BrushId::Draw, -0.3f, 0.0f, 0.3f, 0.1f);
        controllerStroke(*controller, BrushId::Smooth, -0.2f, 0.0f, 0.2f, 0.0f);
        controllerStroke(*controller, BrushId::Move, 0.0f, 0.8f, 0.0f, 1.3f);
    }
    const std::vector<glm::vec3> sculpted = settledPositions(compact);
    EXPECT_NE(sculpted, original);
    EXPECT_TRUE(test::bitwiseEqual(settledPositions(wide), sculpted));
    EXPECT_TRUE(test::bitwiseEqual(settledNormals(wide), settledNormals(compact)));

    for (app::AppController* controller : {&wide, &compact}) {
        controller->undo();
        controller->undo();
        controller->undo();
    }
    EXPECT_TRUE(test::bitwiseEqual(settledPositions(compact), original)) << "undo restores every vertex exactly";
    EXPECT_TRUE(test::bitwiseEqual(settledNormals(wide), settledNormals(compact)));
    const std::vector<glm::vec3> undoneNormals = settledNormals(compact);
    for (std::size_t i = 0; i < originalNormals.size(); ++i) {
        ASSERT_NEAR(glm::distance(undoneNormals[i], originalNormals[i]), 0.0f, 1e-6f);
    }

    for (app::AppController* controller : {&wide, &compact}) {
        controller->redo();
        controller->redo();
        controller->redo();
    }
    EXPECT_TRUE(test::bitwiseEqual(settledPositions(compact), sculpted)) << "redo reapplies every vertex exactly";
    EXPECT_TRUE(test::bitwiseEqual(settledNormals(wide), settledNormals(compact)));
}
