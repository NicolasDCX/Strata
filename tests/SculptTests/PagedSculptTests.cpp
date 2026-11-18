// Sculpting on paged, chunked geometry: strokes that cross vertex pages and
// spatial chunks, undo/redo across them, and independence from the layout.

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/NormalUpdater.h"
#include "Sculpt/StrokeProcessor.h"
#include "Sculpt/UndoStack.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <vector>

using namespace se;
using sculpt::BrushId;
using sculpt::BrushSettings;

namespace {

/// Mesh + chunked BVH + stroke processor with an explicit layout.
struct PagedScene {
    PagedScene(int level, u32 vertexPageShift, u32 chunkTriangleShift)
        : mesh(geo::makeIcosphere(level, 1.0f, geo::VertexOrder::Spatial,
                                  {.vertexPageShift = vertexPageShift, .trianglePageShift = chunkTriangleShift})),
          stroke(mesh, bvh)
    {
        bvh.build(mesh, chunkTriangleShift);
    }

    geo::Mesh mesh;
    geo::ChunkedBvh bvh;
    sculpt::StrokeProcessor stroke;
    sculpt::BrushLibrary brushes;
};

geo::Ray rayAt(float x, float y)
{
    return geo::Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}};
}

sculpt::StrokeDelta runStroke(PagedScene& scene, BrushId brush, const BrushSettings& settings, float y)
{
    EXPECT_TRUE(scene.stroke.begin(scene.brushes.brush(brush), settings, rayAt(-0.3f, y)));
    for (int i = 1; i <= 20; ++i) {
        const float t = static_cast<float>(i) / 20.0f;
        scene.stroke.update(rayAt(-0.3f + 0.6f * t, y + 0.1f * std::sin(t * 5.0f)));
    }
    auto delta = scene.stroke.end();
    EXPECT_TRUE(delta.has_value());
    return delta ? std::move(*delta) : sculpt::StrokeDelta{};
}

std::vector<glm::vec3> positionsOf(const geo::Mesh& mesh)
{
    return {mesh.positions().begin(), mesh.positions().end()};
}

} // namespace

TEST(PagedSculpt, StrokeSpansManyPagesAndChunksWithoutDuplicatingVertices)
{
    PagedScene scene(5, 5, 6); // 32 vertices per page, 64 triangles per chunk
    const std::vector<glm::vec3> original = positionsOf(scene.mesh);
    const u32 vertexCountBefore = scene.mesh.vertexCount();

    const BrushSettings settings{.radius = 0.25f, .strength = 0.8f, .invert = false};
    ASSERT_TRUE(scene.stroke.begin(scene.brushes.brush(BrushId::Draw), settings, rayAt(-0.3f, 0.0f)));
    scene.stroke.update(rayAt(0.3f, 0.05f));
    geo::DirtyVertexSet dirty = scene.mesh.makeDirtySet();
    scene.stroke.drainDirty(dirty);
    const auto delta = scene.stroke.end();
    ASSERT_TRUE(delta.has_value());

    // The edit touched several vertex pages and several chunks.
    std::set<u32> pages;
    std::set<u32> chunks;
    for (const geo::VertexId v : delta->vertices) {
        pages.insert(scene.mesh.vertexPageOf(v));
        for (const geo::TriangleId t : scene.mesh.incidentTriangles(v)) {
            chunks.insert(scene.bvh.chunkOf(t));
        }
    }
    EXPECT_GT(pages.size(), 8u);
    EXPECT_GT(chunks.size(), 8u);

    // Each vertex was recorded once, whichever chunks its triangles are in.
    std::vector<u32> sorted = delta->vertices;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_TRUE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    EXPECT_EQ(scene.mesh.vertexCount(), vertexCountBefore);

    // Every changed vertex is in a dirty page, and dirty pages are a minority.
    for (geo::VertexId v = 0; v < scene.mesh.vertexCount(); ++v) {
        if (scene.mesh.positions()[v] != original[v]) {
            ASSERT_TRUE(dirty.contains(v)) << "vertex " << v;
        }
    }
    EXPECT_EQ(dirty.pageSize(), 32u);
    EXPECT_LT(dirty.dirtyPageCount(), dirty.pageCount() / 2);

    // The surface stays continuous across page and chunk borders: no vertex moved
    // far more than its neighbours (which a missed or doubled vertex would show).
    const float peak = settings.radius * settings.strength * 0.15f * 8.0f; // several overlapping dabs
    for (const geo::VertexId v : delta->vertices) {
        const float moved = glm::distance(scene.mesh.positions()[v], original[v]);
        ASSERT_LT(moved, peak);
        for (const geo::VertexId n : scene.mesh.neighbors(v)) {
            const float neighbourMoved = glm::distance(scene.mesh.positions()[n], original[n]);
            ASSERT_LT(std::abs(moved - neighbourMoved), 0.06f) << "discontinuity between " << v << " and " << n;
        }
    }

    // Normals and BVH were maintained across all of them.
    const std::vector<glm::vec3> incremental(scene.mesh.normals().begin(), scene.mesh.normals().end());
    scene.mesh.recomputeNormals();
    for (geo::VertexId v = 0; v < scene.mesh.vertexCount(); ++v) {
        ASSERT_NEAR(glm::distance(incremental[v], scene.mesh.normals()[v]), 0.0f, 1e-5f);
    }
    std::mt19937 rng(4);
    for (int i = 0; i < 200; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const auto expected = test::bruteForceRaycast(scene.mesh, ray);
        const auto actual = scene.bvh.raycast(scene.mesh, ray);
        ASSERT_EQ(expected.has_value(), actual.has_value());
        if (expected) {
            ASSERT_NEAR(*expected, actual->distance, 1e-4f);
        }
    }
}

TEST(PagedSculpt, ResultDoesNotDependOnPageOrChunkSize)
{
    // The same strokes on tiny pages/chunks and on a single page and chunk.
    // (Not bit-identical: the order in which a dab's vertices are visited follows
    // the BVH layout, and floating-point sums depend on order. The difference
    // must stay at rounding level.)
    PagedScene tiny(5, 4, 5);
    PagedScene monolithic(5, 20, 20);
    ASSERT_GT(tiny.mesh.vertexPageCount(), 500u);
    ASSERT_EQ(monolithic.mesh.vertexPageCount(), 1u);
    ASSERT_EQ(monolithic.bvh.chunkCount(), 1u);

    const BrushSettings draw{.radius = 0.25f, .strength = 0.6f, .invert = false};
    const BrushSettings smooth{.radius = 0.3f, .strength = 0.5f, .invert = false};
    const BrushSettings move{.radius = 0.35f, .strength = 1.0f, .invert = false};
    for (PagedScene* scene : {&tiny, &monolithic}) {
        runStroke(*scene, BrushId::Draw, draw, 0.1f);
        runStroke(*scene, BrushId::Smooth, smooth, 0.05f);
        runStroke(*scene, BrushId::Move, move, -0.2f);
    }

    float worst = 0.0f;
    u32 changed = 0;
    const geo::Mesh pristine = geo::makeIcosphere(5);
    for (geo::VertexId v = 0; v < tiny.mesh.vertexCount(); ++v) {
        worst = std::max(worst, glm::distance(tiny.mesh.positions()[v], monolithic.mesh.positions()[v]));
        changed += tiny.mesh.positions()[v] != pristine.positions()[v] ? 1u : 0u;
    }
    EXPECT_GT(changed, 500u);
    EXPECT_LT(worst, 1e-4f) << "largest position difference between layouts";
}

TEST(PagedSculpt, UndoRedoRestoresExactlyAcrossPages)
{
    PagedScene scene(5, 5, 6);
    sculpt::UndoStack undo;
    sculpt::NormalUpdater normalUpdater;
    const std::vector<glm::vec3> original = positionsOf(scene.mesh);
    const std::vector<glm::vec3> originalNormals(scene.mesh.normals().begin(), scene.mesh.normals().end());

    const auto apply = [&](std::span<const u32> vertices) {
        geo::DirtyVertexSet dirty = scene.mesh.makeDirtySet();
        normalUpdater.update(scene.mesh, vertices, dirty);
        scene.bvh.refit(scene.mesh, vertices);
        scene.bvh.optimize(scene.mesh);
        return dirty;
    };

    std::vector<std::vector<glm::vec3>> states;
    const BrushId order[] = {BrushId::Draw, BrushId::Smooth, BrushId::Move};
    for (int i = 0; i < 3; ++i) {
        sculpt::StrokeDelta delta = runStroke(scene, order[i], {.radius = 0.3f, .strength = 0.8f, .invert = false},
                                             -0.2f + 0.2f * static_cast<float>(i));
        std::set<u32> pages;
        for (const geo::VertexId v : delta.vertices) {
            pages.insert(scene.mesh.vertexPageOf(v));
        }
        ASSERT_GT(pages.size(), 5u) << "stroke " << i << " should span several pages";
        undo.push(std::move(delta));
        states.push_back(positionsOf(scene.mesh));
    }

    // Undo everything: bit-exact positions, and the dirty set names the pages to re-upload.
    for (int i = 2; i >= 0; --i) {
        const geo::DirtyVertexSet dirty = apply(undo.undo(scene.mesh));
        EXPECT_GT(dirty.dirtyPageCount(), 5u);
        const std::vector<glm::vec3>& expected = i > 0 ? states[static_cast<std::size_t>(i - 1)] : original;
        ASSERT_TRUE(test::bitwiseEqual(scene.mesh.positions(), expected)) << "after undoing stroke " << i;
    }
    for (geo::VertexId v = 0; v < scene.mesh.vertexCount(); ++v) {
        ASSERT_NEAR(glm::distance(scene.mesh.normals()[v], originalNormals[v]), 0.0f, 1e-6f);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        apply(undo.redo(scene.mesh));
        ASSERT_TRUE(test::bitwiseEqual(scene.mesh.positions(), states[i])) << "after redoing stroke " << i;
    }

    std::mt19937 rng(8);
    for (int i = 0; i < 200; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const auto expected = test::bruteForceRaycast(scene.mesh, ray);
        const auto actual = scene.bvh.raycast(scene.mesh, ray);
        ASSERT_EQ(expected.has_value(), actual.has_value());
        if (expected) {
            ASSERT_NEAR(*expected, actual->distance, 1e-4f);
        }
    }
}

TEST(PagedSculpt, ThreadedResultIsBitIdenticalWithSmallPagesAndChunks)
{
    // Many pages and many chunks per dab: the parallel chunk refit and the
    // parallel brush loops must not change a single bit.
    core::JobSystem jobs(5);
    PagedScene serial(7, 6, 8);   // 64 vertices per page, 256 triangles per chunk
    PagedScene parallel(7, 6, 8);
    parallel.stroke.setJobSystem(&jobs);
    ASSERT_GT(serial.bvh.chunkCount(), 1000u);

    const BrushSettings large{.radius = 0.6f, .strength = 0.5f, .invert = false};
    for (PagedScene* scene : {&serial, &parallel}) {
        runStroke(*scene, BrushId::Draw, large, 0.0f);
        runStroke(*scene, BrushId::Smooth, large, 0.1f);
        runStroke(*scene, BrushId::Move, large, -0.1f);
        EXPECT_GT(scene->stroke.stats().maxDabVertexCount, 2 * sculpt::kBrushParallelGrain);
    }
    EXPECT_TRUE(test::bitwiseEqual(serial.mesh.positions(), parallel.mesh.positions()));
    EXPECT_TRUE(test::bitwiseEqual(serial.mesh.normals(), parallel.mesh.normals()));
    EXPECT_TRUE(serial.bvh.sameBounds(parallel.bvh));

    serial.bvh.optimize(serial.mesh);
    parallel.bvh.optimize(parallel.mesh);
    EXPECT_TRUE(serial.bvh.sameBounds(parallel.bvh));
}

TEST(PagedSculpt, ControllerWorksWithACustomLayout)
{
    app::AppConfig config;
    config.sphereSubdivisions = 5;
    config.meshLayout = {.vertexPageShift = 5, .trianglePageShift = 6};
    config.chunkTriangleShift = 6;
    app::AppController controller(config);
    EXPECT_EQ(controller.mesh().layout().vertexPageCapacity(), 32u);
    EXPECT_EQ(controller.chunkCount(), controller.mesh().triangleCount() / 64u);
    EXPECT_EQ(controller.pendingVertexUpdates().pageSize(), 32u);

    const auto settled = [&controller] {
        controller.waitUntilIdle();
        return positionsOf(controller.mesh());
    };
    const std::vector<glm::vec3> original = settled();

    controller.setRadius(0.3f);
    ASSERT_TRUE(controller.beginStroke(rayAt(-0.3f, 0.0f)));
    for (int i = 1; i <= 30; ++i) {
        controller.updateStroke(rayAt(-0.3f + 0.02f * static_cast<float>(i), 0.0f));
    }
    controller.endStroke();
    const std::vector<glm::vec3> sculpted = settled();
    EXPECT_NE(sculpted, original);

    geo::DirtyVertexSet dirty = controller.mesh().makeDirtySet();
    controller.drainDirtyVertices(dirty);
    EXPECT_GT(dirty.dirtyPageCount(), 5u);
    EXPECT_LT(dirty.dirtyPageCount(), dirty.pageCount());

    controller.undo();
    EXPECT_EQ(settled(), original);
    controller.redo();
    EXPECT_EQ(settled(), sculpted);

    // Background optimisation finished (waitUntilIdle) and queries are correct.
    std::mt19937 rng(21);
    for (int i = 0; i < 200; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const auto expected = test::bruteForceRaycast(controller.mesh(), ray);
        const auto actual = controller.pick(ray);
        ASSERT_EQ(expected.has_value(), actual.has_value());
        if (expected) {
            ASSERT_NEAR(*expected, actual->distance, 1e-4f);
        }
    }
    EXPECT_GT(controller.bounds().max.z, 1.0f);
}
