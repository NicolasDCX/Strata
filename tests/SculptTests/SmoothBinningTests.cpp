// Parallel binning of Smooth's kernel grid (SmoothBrush::setParallelBinning).
// Each cell must sum its vertices' positions, normals and counts in the order
// of the dab's vertex list, so the brush result is bit-identical to the serial
// loop for any list order and any number of helper threads.

#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/Brushes/SmoothBrush.h"
#include "Sculpt/StrokeProcessor.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <vector>

using namespace se;
using sculpt::BrushId;
using sculpt::SmoothBrush;

namespace {

/// Restores the default (parallel binning) when a test ends.
struct BinningGuard {
    ~BinningGuard() { SmoothBrush::setParallelBinning(true); }
};

enum class Order { Ascending, Shuffled, Reversed };

/// Applies Smooth once to a cap of the sphere with the vertices listed in
/// `order` and returns every position afterwards.
std::vector<glm::vec3> applyOnce(Order order, unsigned helpers, bool parallel, float radius, float strength)
{
    geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    // Some relief, so the kernel pass has something to remove.
    {
        auto positions = mesh.mutablePositions();
        for (std::size_t v = 0; v < positions.size(); ++v) {
            const glm::vec3 p = positions[v];
            positions[v] = p * (1.0f + 0.02f * std::sin(40.0f * p.x) * std::cos(30.0f * p.y));
        }
    }
    const glm::vec3 center(0.0f, 0.0f, 1.0f);
    std::vector<u32> vertices;
    std::vector<float> weights;
    const auto positions = mesh.positions();
    for (u32 v = 0; v < positions.size(); ++v) {
        const float distance = glm::length(positions[v] - center);
        if (distance < radius) {
            vertices.push_back(v);
        }
    }
    if (order == Order::Shuffled) {
        std::mt19937 rng(7);
        std::shuffle(vertices.begin(), vertices.end(), rng);
    } else if (order == Order::Reversed) {
        std::reverse(vertices.begin(), vertices.end());
    }
    for (const u32 v : vertices) {
        weights.push_back(std::max(0.05f, 1.0f - glm::length(positions[v] - center) / radius));
    }

    std::unique_ptr<core::JobSystem> jobs = helpers > 0 ? std::make_unique<core::JobSystem>(helpers) : nullptr;
    sculpt::DabScratch scratch;
    const sculpt::DabContext ctx{
        .mesh = mesh,
        .positions = mesh.mutablePositions(),
        .vertices = vertices,
        .weights = weights,
        .center = center,
        .normal = {0.0f, 0.0f, 1.0f},
        .radius = radius,
        .strength = strength,
        .scratch = scratch,
        .jobs = jobs.get(),
    };
    SmoothBrush::setParallelBinning(parallel);
    const SmoothBrush brush;
    brush.apply(ctx);
    const auto after = mesh.positions();
    return {after.begin(), after.end()};
}

} // namespace

TEST(SmoothBinning, BrushMatchesSerialBinningForAnyListOrder)
{
    const BinningGuard guard;
    for (const Order order : {Order::Ascending, Order::Shuffled, Order::Reversed}) {
        SCOPED_TRACE(testing::Message() << "order " << static_cast<int>(order));
        const std::vector<glm::vec3> serial = applyOnce(order, 0, false, 1.2f, 0.7f);
        for (const unsigned helpers : {1u, 3u, 11u}) {
            SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
            SmoothBrush::resetSteps();
            const std::vector<glm::vec3> parallel = applyOnce(order, helpers, true, 1.2f, 0.7f);
            const SmoothBrush::Steps steps = SmoothBrush::steps();
            EXPECT_EQ(steps.parallelBins, 1u) << "the cap must be large enough to bin in parallel";
            EXPECT_EQ(steps.denseCalls, 1u) << "the cap must be dense";
            EXPECT_TRUE(test::bitwiseEqual(parallel, serial));
            // Serial binning with the same helpers (threaded kernel and
            // Laplacian passes) is the same too.
            EXPECT_TRUE(test::bitwiseEqual(applyOnce(order, helpers, false, 1.2f, 0.7f), serial));
        }
    }
    // Below the threshold the serial loop runs even when parallel binning is on.
    SmoothBrush::resetSteps();
    const std::vector<glm::vec3> small = applyOnce(Order::Shuffled, 3, true, 0.3f, 0.7f);
    EXPECT_EQ(SmoothBrush::steps().parallelBins, 0u);
    EXPECT_TRUE(test::bitwiseEqual(small, applyOnce(Order::Shuffled, 0, false, 0.3f, 0.7f)));
}

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
    std::vector<u64> dabHashes;
    std::vector<sculpt::StrokeDelta> deltas;
    u64 parallelBins = 0;
};

/// A Draw stroke for relief, then Smooth strokes of several radii and
/// strengths (dense and sparse dabs, above and below the threshold).
StrokeRun runStrokes(bool parallel, unsigned helpers)
{
    SmoothBrush::setParallelBinning(parallel);
    geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
    std::unique_ptr<core::JobSystem> jobs = helpers > 0 ? std::make_unique<core::JobSystem>(helpers) : nullptr;
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(jobs.get());
    stroke.setParallelGather(true);
    stroke.setParallelMarking(true);
    stroke.setParallelRegion(true);
    stroke.setOverlapPrepare(true);
    StrokeRun run;
    stroke.setCommitHooks([] {}, [&] { run.dabHashes.push_back(hashMesh(mesh)); });
    struct Plan {
        BrushId brush;
        float radius;
        float strength;
        float y;
    };
    const Plan plans[] = {{BrushId::Draw, 0.6f, 0.6f, -0.1f},
                          {BrushId::Smooth, 1.2f, 0.5f, -0.05f},
                          {BrushId::Smooth, 1.0f, 1.0f, 0.0f},
                          {BrushId::Smooth, 0.15f, 0.5f, 0.05f},
                          {BrushId::Smooth, 0.03f, 0.9f, 0.1f}};
    const sculpt::BrushLibrary brushes;
    SmoothBrush::resetSteps();
    for (const Plan& plan : plans) {
        sculpt::BrushSettings settings = brushes.settings(plan.brush);
        settings.radius = plan.radius;
        settings.strength = plan.strength;
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
    run.parallelBins = SmoothBrush::steps().parallelBins;
    return run;
}

} // namespace

TEST(SmoothBinning, StrokesAndUndoRecordsMatchSerialBinning)
{
    const BinningGuard guard;
    const StrokeRun serial = runStrokes(false, 3);
    EXPECT_EQ(serial.parallelBins, 0u);
    for (const unsigned helpers : {0u, 1u, 3u, 11u}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const StrokeRun parallel = runStrokes(true, helpers);
        if (helpers > 0) {
            EXPECT_GE(parallel.parallelBins, 5u) << "the large dabs must bin in parallel";
        } else {
            EXPECT_EQ(parallel.parallelBins, 0u);
        }
        EXPECT_EQ(parallel.dabHashes, serial.dabHashes);
        ASSERT_EQ(parallel.deltas.size(), serial.deltas.size());
        for (std::size_t i = 0; i < serial.deltas.size(); ++i) {
            EXPECT_EQ(parallel.deltas[i].vertices, serial.deltas[i].vertices) << "stroke " << i;
            EXPECT_TRUE(test::bitwiseEqual(parallel.deltas[i].before, serial.deltas[i].before)) << "stroke " << i;
            EXPECT_TRUE(test::bitwiseEqual(parallel.deltas[i].after, serial.deltas[i].after)) << "stroke " << i;
        }
    }
}
