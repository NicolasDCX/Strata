// Parallel gather: a dab's sphere query and vertex selection run on the job
// system, joined in a fixed order. The result must be the serial gather's,
// bit for bit, whatever the number of threads: the same vertices in the same
// order with the same weights, so the same positions, normals and undo record.

#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/NormalUpdater.h"
#include "Sculpt/StrokeProcessor.h"
#include "Sculpt/UndoStack.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
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

struct StrokeRun {
    std::vector<u64> dabHashes;            // after every commit
    std::vector<sculpt::StrokeDelta> deltas; // one per stroke
    u64 initialHash = 0;
    u64 undoneHash = 0;
    u64 redoneHash = 0;
};

/// Strokes over a 327K-triangle sphere with a brush large enough (about 50K
/// triangles per dab) to take the parallel query and selection, plus a small
/// one that stays on the calling thread.
StrokeRun runStrokes(bool parallelGather, unsigned helpers)
{
    geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
    std::unique_ptr<core::JobSystem> jobs = helpers > 0 ? std::make_unique<core::JobSystem>(helpers) : nullptr;
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(jobs.get());
    stroke.setParallelGather(parallelGather);
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

    sculpt::UndoStack undo;
    sculpt::NormalUpdater normals;
    geo::DirtyVertexSet dirty(mesh.vertexCount());
    const auto refresh = [&](std::span<const u32> vertices) {
        normals.update(mesh, vertices, dirty);
        bvh.refit(mesh, vertices);
    };
    for (sculpt::StrokeDelta& delta : run.deltas) {
        undo.push(sculpt::StrokeDelta(delta));
    }
    while (undo.canUndo()) {
        refresh(undo.undo(mesh));
    }
    run.undoneHash = hashMesh(mesh);
    while (undo.canRedo()) {
        refresh(undo.redo(mesh));
    }
    run.redoneHash = hashMesh(mesh);
    return run;
}

void expectSame(const StrokeRun& a, const StrokeRun& b)
{
    EXPECT_EQ(a.dabHashes, b.dabHashes);
    ASSERT_EQ(a.deltas.size(), b.deltas.size());
    for (std::size_t i = 0; i < a.deltas.size(); ++i) {
        EXPECT_EQ(a.deltas[i].vertices, b.deltas[i].vertices) << "stroke " << i;
        EXPECT_TRUE(test::bitwiseEqual(a.deltas[i].before, b.deltas[i].before)) << "stroke " << i;
        EXPECT_TRUE(test::bitwiseEqual(a.deltas[i].after, b.deltas[i].after)) << "stroke " << i;
    }
    EXPECT_EQ(a.undoneHash, b.undoneHash);
    EXPECT_EQ(a.redoneHash, b.redoneHash);
}

} // namespace

TEST(ParallelGather, MatchesSerialGatherBitForBitAtEveryThreadCount)
{
    const StrokeRun serial = runStrokes(false, 0);
    ASSERT_GT(serial.dabHashes.size(), 20u);
    EXPECT_EQ(serial.undoneHash, serial.initialHash) << "undo is not exact";
    EXPECT_EQ(serial.redoneHash, serial.dabHashes.back()) << "redo is not exact";
    for (const unsigned helpers : {0u, 1u, 3u, 11u}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        expectSame(serial, runStrokes(true, helpers));
    }
    // The serial gather with helpers busy elsewhere is still the serial gather.
    expectSame(serial, runStrokes(false, 3));
}

TEST(ParallelGather, SwitchingModesKeepsResults)
{
    // The per-vertex stamps change meaning between the modes; switching between
    // strokes must leave nothing behind.
    geo::Mesh mesh = geo::makeIcosphere(6, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
    core::JobSystem jobs(3);
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(&jobs);
    geo::Mesh reference = geo::makeIcosphere(6, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh referenceBvh;
    referenceBvh.build(reference, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
    sculpt::StrokeProcessor serial(reference, referenceBvh);
    const sculpt::BrushLibrary brushes;
    sculpt::BrushSettings settings = brushes.settings(BrushId::Draw);
    settings.radius = 0.9f;
    for (int s = 0; s < 4; ++s) {
        stroke.setParallelGather(s % 2 == 0);
        const geo::Ray ray{{-0.3f + 0.2f * static_cast<float>(s), 0.1f, 5.0f}, {0.0f, 0.0f, -1.0f}};
        ASSERT_TRUE(stroke.begin(brushes.brush(BrushId::Draw), settings, ray));
        ASSERT_TRUE(serial.begin(brushes.brush(BrushId::Draw), settings, ray));
        const auto a = stroke.end();
        const auto b = serial.end();
        ASSERT_TRUE(a && b);
        EXPECT_EQ(a->vertices, b->vertices) << "stroke " << s;
        EXPECT_EQ(hashMesh(mesh), hashMesh(reference)) << "stroke " << s;
    }
}
