// Overlapped dab preparation: with helper threads, a large dab's undo record,
// average normal and page staging run at the same time
// (StrokeProcessor::setOverlapPrepare). Each keeps its own serial loop, so the
// vertex order, falloff weights, brush normal (summed in gather order) and
// undo records must be bit-identical to running them one after the other, at
// any thread count, through strokes and through undo/redo.

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
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

struct StrokeRun {
    std::vector<u64> dabHashes;
    std::vector<sculpt::StrokeDelta> deltas;
    std::vector<u32> picks;
    std::unique_ptr<geo::ChunkedBvh> finalBvh;
    u32 overlapped = 0;
    u32 dabs = 0;
};

/// Draw, Smooth and Move strokes with dabs of about 25K moved vertices (above
/// the threshold), strokes that cross earlier ones (vertices already in the
/// undo record), then a small Draw stroke (below it).
StrokeRun runStrokes(bool overlap, unsigned helpers)
{
    geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
    std::unique_ptr<core::JobSystem> jobs = helpers > 0 ? std::make_unique<core::JobSystem>(helpers) : nullptr;
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(jobs.get());
    stroke.setParallelGather(true);
    stroke.setParallelMarking(true);
    stroke.setParallelRegion(true);
    stroke.setOverlapPrepare(overlap);
    StrokeRun run;
    stroke.setCommitHooks([] {}, [&] { run.dabHashes.push_back(hashMesh(mesh)); });
    struct Plan {
        BrushId brush;
        float radius;
        float y;
    };
    const Plan plans[] = {{BrushId::Draw, 0.8f, -0.2f},
                          {BrushId::Smooth, 0.8f, 0.0f},
                          {BrushId::Draw, 0.8f, 0.05f},
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
        run.overlapped += stroke.stats().gather.overlappedDabs;
        run.dabs += stroke.stats().dabCount;
        std::optional<sculpt::StrokeDelta> delta = stroke.end();
        EXPECT_TRUE(delta.has_value());
        if (delta) {
            run.deltas.push_back(*delta);
        }
    }
    run.picks = pickAll(bvh, mesh);
    run.finalBvh = std::make_unique<geo::ChunkedBvh>(bvh);
    return run;
}

} // namespace

TEST(OverlappedPrepare, StrokesAndUndoRecordsMatchSerialPreparation)
{
    const StrokeRun serial = runStrokes(false, 3);
    ASSERT_GT(serial.dabHashes.size(), 30u);
    EXPECT_EQ(serial.overlapped, 0u);
    for (const unsigned helpers : {0u, 1u, 3u, 11u}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const StrokeRun overlapped = runStrokes(true, helpers);
        if (helpers > 0) {
            EXPECT_GE(overlapped.overlapped, 10u) << "the dabs must be large enough to overlap";
            EXPECT_LT(overlapped.overlapped, overlapped.dabs) << "the small stroke must stay serial";
        } else {
            EXPECT_EQ(overlapped.overlapped, 0u);
        }
        EXPECT_EQ(overlapped.dabHashes, serial.dabHashes);
        ASSERT_EQ(overlapped.deltas.size(), serial.deltas.size());
        for (std::size_t i = 0; i < serial.deltas.size(); ++i) {
            EXPECT_EQ(overlapped.deltas[i].vertices, serial.deltas[i].vertices) << "stroke " << i;
            EXPECT_TRUE(test::bitwiseEqual(overlapped.deltas[i].before, serial.deltas[i].before)) << "stroke " << i;
            EXPECT_TRUE(test::bitwiseEqual(overlapped.deltas[i].after, serial.deltas[i].after)) << "stroke " << i;
        }
        EXPECT_TRUE(overlapped.finalBvh->sameBounds(*serial.finalBvh)) << "boxes or cones differ";
        EXPECT_EQ(overlapped.picks, serial.picks);
    }
}

namespace {

struct HistoryRun {
    std::vector<u64> hashes; // after every stroke, undo and redo
    std::vector<std::vector<u32>> picks;
    std::vector<std::unique_ptr<geo::ChunkedBvh>> trees;
};

HistoryRun runHistory(bool overlap, int helpers)
{
    app::AppConfig config;
    config.sphereSubdivisions = 7;
    config.jobThreads = helpers;
    config.overlapPrepare = overlap;
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

TEST(OverlappedPrepare, UndoRedoThroughControllerMatchSerialPreparation)
{
    const HistoryRun serial = runHistory(false, 3);
    ASSERT_EQ(serial.hashes.size(), 13u);
    // Undo all returns to the start, redo all to the sculpted mesh.
    EXPECT_EQ(serial.hashes[8], serial.hashes[0]);
    EXPECT_EQ(serial.hashes[12], serial.hashes[4]);
    for (const int helpers : {0, 3, 11}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const HistoryRun overlapped = runHistory(true, helpers);
        EXPECT_EQ(overlapped.hashes, serial.hashes);
        EXPECT_EQ(overlapped.picks, serial.picks);
        ASSERT_EQ(overlapped.trees.size(), serial.trees.size());
        for (std::size_t i = 0; i < serial.trees.size(); ++i) {
            EXPECT_TRUE(overlapped.trees[i]->sameBounds(*serial.trees[i])) << "step " << i;
        }
    }
}
