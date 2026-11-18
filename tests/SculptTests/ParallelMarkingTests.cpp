// Parallel BVH leaf marking: ChunkedBvh::markTriangles with a job system groups
// the triangles by chunk (keeping their order) and marks one chunk per task.
// Every chunk must see the same marks in the same order as in the serial loop,
// and the refit queue must list the chunks in the same order, so boxes, cones,
// the background optimize steps that follow, picking, strokes and undo/redo
// are all bit-identical at any thread count.

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/EditRegion.h"
#include "Geometry/Icosphere.h"
#include "Geometry/NormalCone.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/NormalUpdater.h"
#include "Sculpt/StrokeProcessor.h"
#include "Sculpt/UndoStack.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
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

/// Rays over the front of the sphere; the hits (triangle, distance, position)
/// as raw bits, for comparing picking between two trees.
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

/// Optimizes in small steps and compares the two trees after every step: the
/// optimize queue is filled in refit-queue order, so a different chunk order
/// would re-partition a different chunk first.
void expectSameOptimizeSteps(geo::ChunkedBvh& a, geo::ChunkedBvh& b, const geo::Mesh& mesh)
{
    int steps = 0;
    while (a.needsOptimize() || b.needsOptimize()) {
        ASSERT_EQ(a.needsOptimize(), b.needsOptimize());
        EXPECT_EQ(a.optimize(mesh, 512), b.optimize(mesh, 512));
        ASSERT_TRUE(a.sameBounds(b)) << "after optimize step " << steps;
        ++steps;
    }
    EXPECT_GT(steps, 2);
}

} // namespace

TEST(ParallelMarking, RefitMatchesSerialMarkingForAnyTriangleOrder)
{
    // A list that jumps between chunks (shuffled in runs), so chunks are
    // reached out of index order and a block reaches a chunk several times.
    geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    for (const u32 shift : {8u, 12u}) {
        SCOPED_TRACE(testing::Message() << "chunk shift " << shift);
        geo::ChunkedBvh serial;
        serial.build(mesh, shift, nullptr, true);
        std::vector<u32> moved;
        for (u32 v = 0; v < mesh.vertexCount(); ++v) {
            const glm::vec3 p = mesh.positions()[v];
            if (p.z > 0.0f && p.x * p.x + p.y * p.y < 0.5f) {
                moved.push_back(v);
            }
        }
        std::mt19937 random(7);
        // Move them (normals do not matter for boxes, and cones are recomputed).
        for (const u32 v : moved) {
            mesh.mutablePositions()[v] += glm::vec3(0.0f, 0.0f, 0.01f + 0.001f * static_cast<float>(random() % 7));
        }
        geo::EditRegion region;
        region.collect(mesh, moved);
        std::vector<u32> triangles(region.triangles().begin(), region.triangles().end());
        ASSERT_GT(triangles.size(), 40000u);
        for (std::size_t i = 0; i + 64 <= triangles.size(); i += 64) {
            if (random() % 3 == 0) {
                const std::size_t j = (random() % (triangles.size() / 64)) * 64;
                std::swap_ranges(triangles.begin() + static_cast<std::ptrdiff_t>(i),
                                 triangles.begin() + static_cast<std::ptrdiff_t>(i + 64),
                                 triangles.begin() + static_cast<std::ptrdiff_t>(j));
            }
        }
        // Some triangles twice, as when two moved vertices share them.
        triangles.insert(triangles.end(), triangles.begin(), triangles.begin() + 5000);

        geo::ChunkedBvh reference = serial;
        reference.markTriangles(triangles);
        reference.finishMarkedRefit(mesh);
        for (const unsigned helpers : {0u, 1u, 3u, 11u}) {
            SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
            std::unique_ptr<core::JobSystem> jobs = helpers > 0 ? std::make_unique<core::JobSystem>(helpers) : nullptr;
            geo::ChunkedBvh parallel = serial;
            parallel.markTriangles(triangles, jobs.get());
            parallel.finishMarkedRefit(mesh, jobs.get());
            EXPECT_TRUE(parallel.sameBounds(reference));
            EXPECT_EQ(parallel.lastConeTriangles(), reference.lastConeTriangles());
            EXPECT_EQ(pickAll(parallel, mesh), pickAll(reference, mesh));
            geo::ChunkedBvh referenceCopy = reference;
            expectSameOptimizeSteps(parallel, referenceCopy, mesh);
            EXPECT_EQ(pickAll(parallel, mesh), pickAll(referenceCopy, mesh));
        }
        mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    }
}

namespace {

struct StrokeRun {
    std::vector<u64> dabHashes;
    std::vector<u32> picks;   // after the last stroke, before optimizing
    std::vector<u32> picksOptimized;
    std::vector<sculpt::StrokeDelta> deltas;
    u64 initialHash = 0;
    u64 undoneHash = 0;
    u64 redoneHash = 0;
    std::unique_ptr<geo::ChunkedBvh> finalBvh;
    bool undoneBvhMatchesFresh = false;
};

/// Draw, Smooth and Move strokes large enough (about 100K triangles per dab)
/// to take the parallel marking, then exact undo and redo.
StrokeRun runStrokes(bool parallelMarking, unsigned helpers)
{
    geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
    std::unique_ptr<core::JobSystem> jobs = helpers > 0 ? std::make_unique<core::JobSystem>(helpers) : nullptr;
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(jobs.get());
    stroke.setParallelGather(true);
    stroke.setParallelMarking(parallelMarking);
    StrokeRun run;
    stroke.setCommitHooks([] {}, [&] { run.dabHashes.push_back(hashMesh(mesh)); });
    run.initialHash = hashMesh(mesh);

    struct Plan {
        BrushId brush;
        float radius;
        float y;
    };
    const Plan plans[] = {{BrushId::Draw, 0.8f, -0.2f},
                          {BrushId::Smooth, 0.8f, 0.0f},
                          {BrushId::Move, 0.8f, 0.1f},
                          {BrushId::Draw, 0.1f, 0.2f}};
    const sculpt::BrushLibrary brushes;
    for (const Plan& plan : plans) {
        sculpt::BrushSettings settings = brushes.settings(plan.brush);
        settings.radius = plan.radius;
        const auto ray = [&](int i) {
            const float t = static_cast<float>(i) / 12.0f;
            return geo::Ray{{-0.4f + 0.8f * t, plan.y + 0.1f * t, 5.0f}, {0.0f, 0.0f, -1.0f}};
        };
        EXPECT_TRUE(stroke.begin(brushes.brush(plan.brush), settings, ray(0)));
        for (int i = 1; i <= 12; ++i) {
            stroke.update(ray(i));
        }
        std::optional<sculpt::StrokeDelta> delta = stroke.end();
        EXPECT_TRUE(delta.has_value());
        if (delta) {
            run.deltas.push_back(*delta);
        }
    }
    run.picks = pickAll(bvh, mesh);
    run.finalBvh = std::make_unique<geo::ChunkedBvh>(bvh);
    while (bvh.needsOptimize()) {
        bvh.optimize(mesh, 4096);
    }
    run.picksOptimized = pickAll(bvh, mesh);

    sculpt::UndoStack undo;
    for (const sculpt::StrokeDelta& delta : run.deltas) {
        undo.push(sculpt::StrokeDelta(delta));
    }
    sculpt::NormalUpdater normals;
    geo::DirtyVertexSet dirty(mesh.vertexCount());
    const auto refresh = [&](std::span<const u32> vertices) {
        normals.update(mesh, vertices, dirty);
        bvh.refit(mesh, vertices);
    };
    while (undo.canUndo()) {
        refresh(undo.undo(mesh));
    }
    run.undoneHash = hashMesh(mesh);
    // Undone positions give the same boxes and cones as a full refit.
    geo::ChunkedBvh fresh = bvh;
    fresh.refit(mesh);
    run.undoneBvhMatchesFresh = fresh.sameBounds(bvh);
    while (undo.canRedo()) {
        refresh(undo.redo(mesh));
    }
    run.redoneHash = hashMesh(mesh);
    return run;
}

} // namespace

TEST(ParallelMarking, StrokesMatchSerialMarkingAtEveryThreadCount)
{
    const StrokeRun serial = runStrokes(false, 3);
    ASSERT_GT(serial.dabHashes.size(), 20u);
    EXPECT_EQ(serial.undoneHash, serial.initialHash) << "undo is not exact";
    EXPECT_EQ(serial.redoneHash, serial.dabHashes.back()) << "redo is not exact";
    EXPECT_TRUE(serial.undoneBvhMatchesFresh);
    for (const unsigned helpers : {0u, 1u, 3u, 11u}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const StrokeRun parallel = runStrokes(true, helpers);
        EXPECT_EQ(parallel.dabHashes, serial.dabHashes);
        ASSERT_EQ(parallel.deltas.size(), serial.deltas.size());
        for (std::size_t i = 0; i < serial.deltas.size(); ++i) {
            EXPECT_EQ(parallel.deltas[i].vertices, serial.deltas[i].vertices) << "stroke " << i;
            EXPECT_TRUE(test::bitwiseEqual(parallel.deltas[i].after, serial.deltas[i].after)) << "stroke " << i;
        }
        EXPECT_TRUE(parallel.finalBvh->sameBounds(*serial.finalBvh)) << "boxes or cones differ";
        EXPECT_EQ(parallel.picks, serial.picks);
        EXPECT_EQ(parallel.picksOptimized, serial.picksOptimized);
        EXPECT_EQ(parallel.undoneHash, serial.initialHash);
        EXPECT_EQ(parallel.redoneHash, serial.redoneHash);
        EXPECT_TRUE(parallel.undoneBvhMatchesFresh);
    }
}

namespace {

struct HistoryRun {
    std::vector<u64> hashes;   // after every stroke, undo and redo
    std::vector<std::vector<u32>> picks;
    std::vector<std::unique_ptr<geo::ChunkedBvh>> trees;
};

/// Strokes, then undo and redo of everything, through the controller (the
/// marking of undo and redo runs before it takes the document lock).
HistoryRun runHistory(bool parallelMarking, int helpers)
{
    app::AppConfig config;
    config.sphereSubdivisions = 7;
    config.jobThreads = helpers;
    config.parallelMarking = parallelMarking;
    app::AppController controller(config);
    HistoryRun run;
    const auto snapshot = [&] {
        controller.waitUntilIdle();
        const std::scoped_lock lock(controller.documentMutex());
        run.hashes.push_back(hashMesh(controller.mesh()));
        run.picks.push_back(pickAll(controller.spatialIndex(), controller.mesh()));
        run.trees.push_back(std::make_unique<geo::ChunkedBvh>(controller.spatialIndex()));
    };
    snapshot();
    const BrushId brushes[] = {BrushId::Draw, BrushId::Smooth, BrushId::Move, BrushId::Draw};
    for (int s = 0; s < 4; ++s) {
        controller.setActiveBrush(brushes[s]);
        controller.setRadius(0.8f);
        const float y = -0.2f + 0.1f * static_cast<float>(s);
        const auto ray = [&](int i) {
            const float t = static_cast<float>(i) / 12.0f;
            return geo::Ray{{-0.4f + 0.8f * t, y + 0.1f * t, 5.0f}, {0.0f, 0.0f, -1.0f}};
        };
        EXPECT_TRUE(controller.beginStroke(ray(0)));
        for (int i = 1; i <= 12; ++i) {
            controller.updateStroke(ray(i));
        }
        controller.endStroke();
        snapshot();
    }
    for (int i = 0; i < 4; ++i) {
        controller.undo();
        snapshot();
    }
    for (int i = 0; i < 4; ++i) {
        controller.redo();
        snapshot();
    }
    return run;
}

} // namespace

TEST(ParallelMarking, UndoRedoThroughControllerMatchSerialMarking)
{
    const HistoryRun serial = runHistory(false, 0);
    ASSERT_EQ(serial.hashes.size(), 13u);
    EXPECT_EQ(serial.hashes[8], serial.hashes[0]) << "undo is not exact";
    EXPECT_EQ(serial.hashes[12], serial.hashes[4]) << "redo is not exact";
    for (const int helpers : {0, 1, 3, 11}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const HistoryRun parallel = runHistory(true, helpers);
        ASSERT_EQ(parallel.hashes.size(), serial.hashes.size());
        for (std::size_t i = 0; i < serial.hashes.size(); ++i) {
            EXPECT_EQ(parallel.hashes[i], serial.hashes[i]) << "step " << i;
            EXPECT_EQ(parallel.picks[i], serial.picks[i]) << "step " << i;
            // waitUntilIdle() returns once the background optimize is done too.
            EXPECT_TRUE(parallel.trees[i]->sameBounds(*serial.trees[i])) << "step " << i;
        }
    }
}
