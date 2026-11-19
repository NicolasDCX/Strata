// Parallel edit region: geo::EditRegion::collect with a job system walks large
// edits on the helper threads. It must give the serial walk's two lists in
// the same order (the triangle list fixes the BVH marking order, the vertex
// list the order normals are computed and published in), so strokes, boxes,
// cones, picking and undo/redo are bit-identical at any thread count.

#include "Application/AppController.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/EditRegion.h"
#include "Geometry/Icosphere.h"
#include "Sculpt/BrushLibrary.h"
#include "Sculpt/StrokeProcessor.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
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

std::vector<u32> toVector(std::span<const u32> span)
{
    return {span.begin(), span.end()};
}

/// Vertices within `radius` of `center`, in a shuffled order (brushes gather
/// in BVH order, undo in record order).
std::vector<u32> movedNear(const geo::Mesh& mesh, const glm::vec3& center, float radius, u32 seed)
{
    std::vector<u32> moved;
    const auto positions = mesh.positions();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        if (glm::distance(positions[vertex], center) < radius) {
            moved.push_back(vertex);
        }
    }
    std::mt19937 rng(seed);
    std::shuffle(moved.begin(), moved.end(), rng);
    return moved;
}

/// Moved-vertex lists that exercise the order rules: compact and scattered,
/// sorted, repeated vertices, every vertex, and around the parallel threshold.
std::vector<std::vector<u32>> movedSets(const geo::Mesh& mesh)
{
    std::vector<std::vector<u32>> sets;
    sets.push_back(movedNear(mesh, {0.0f, 0.0f, 1.0f}, 0.6f, 1));
    sets.push_back(movedNear(mesh, {0.6f, -0.6f, 0.5f}, 1.2f, 2));
    std::vector<u32> sorted = movedNear(mesh, {-0.5f, 0.5f, -0.7f}, 0.7f, 3);
    std::sort(sorted.begin(), sorted.end());
    sets.push_back(sorted);
    // Repeats: the second copy of a vertex must add nothing, wherever it is.
    std::vector<u32> repeated = movedNear(mesh, {0.0f, 1.0f, 0.0f}, 0.6f, 4);
    std::mt19937 rng(5);
    const std::size_t original = repeated.size();
    for (std::size_t i = 0; i < original / 3; ++i) {
        repeated.insert(repeated.begin() + static_cast<std::ptrdiff_t>(rng() % repeated.size()), repeated[rng() % original]);
    }
    sets.push_back(repeated);
    // Two distant patches, interleaved (one row's triangles can be owned by a
    // vertex in a far block).
    std::vector<u32> a = movedNear(mesh, {1.0f, 0.0f, 0.0f}, 0.5f, 6);
    const std::vector<u32> b = movedNear(mesh, {-1.0f, 0.0f, 0.0f}, 0.5f, 7);
    std::vector<u32> both;
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        if (i < a.size()) {
            both.push_back(a[i]);
        }
        if (i < b.size()) {
            both.push_back(b[i]);
        }
    }
    sets.push_back(both);
    std::vector<u32> all(mesh.vertexCount());
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        all[v] = v;
    }
    std::shuffle(all.begin(), all.end(), rng);
    sets.push_back(all);
    // Sparse: most triangles have one-ring corners, and most one-ring vertices
    // are corners of triangles owned by several rows in different blocks.
    const std::vector<u32> patch = movedNear(mesh, {0.3f, 0.3f, 0.9f}, 1.2f, 9);
    std::vector<u32> sparse;
    for (std::size_t i = 0; i < patch.size(); i += 3) {
        sparse.push_back(patch[i]);
    }
    sets.push_back(sparse);
    std::vector<u32> random;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (rng() % 10 < 3) {
            random.push_back(v);
        }
    }
    std::shuffle(random.begin(), random.end(), rng);
    sets.push_back(random);
    // Just below and at the threshold.
    std::vector<u32> big = movedNear(mesh, {0.0f, 0.0f, -1.0f}, 1.0f, 8);
    for (const std::size_t n : {geo::EditRegion::kParallelMinVertices - 1, geo::EditRegion::kParallelMinVertices,
                                geo::EditRegion::kParallelMinVertices + geo::EditRegion::kParallelVertexBlock + 1}) {
        if (big.size() >= n) {
            sets.emplace_back(big.begin(), big.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    return sets;
}

} // namespace

TEST(ParallelRegion, ListsEqualTheSerialWalkInOrder)
{
    for (const geo::MeshLayout layout : {geo::MeshLayout{}, geo::MeshLayout{.vertexPageShift = 6, .trianglePageShift = 7},
                                         geo::MeshLayout{.adjacency = geo::AdjacencyLayout::Wide}}) {
        const geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial, layout);
        const std::vector<std::vector<u32>> sets = movedSets(mesh);
        ASSERT_GE(sets.size(), 8u);
        geo::EditRegion serial;
        geo::EditRegion parallel; // reused: its stamps must be clear after every call
        for (const unsigned helpers : {1u, 3u, 11u}) {
            core::JobSystem jobs(helpers);
            for (std::size_t s = 0; s < sets.size(); ++s) {
                SCOPED_TRACE(testing::Message() << "page shift " << layout.vertexPageShift << ", " << helpers
                                                << " helper(s), set " << s << " (" << sets[s].size() << " moved)");
                serial.collect(mesh, sets[s]);
                parallel.collect(mesh, sets[s], &jobs);
                EXPECT_FALSE(serial.lastWasParallel());
                EXPECT_EQ(parallel.lastWasParallel(), sets[s].size() >= geo::EditRegion::kParallelMinVertices);
                ASSERT_EQ(toVector(parallel.triangles()), toVector(serial.triangles()));
                ASSERT_EQ(toVector(parallel.vertices()), toVector(serial.vertices()));
                // The serial walk after a parallel one on the same object, and the reverse.
                parallel.collect(mesh, sets[(s + 1) % sets.size()]);
                serial.collect(mesh, sets[(s + 1) % sets.size()]);
                ASSERT_EQ(toVector(parallel.vertices()), toVector(serial.vertices()));
            }
        }
    }
    // Without helpers the job system is not used.
    const geo::Mesh mesh = geo::makeIcosphere(6);
    core::JobSystem none(0);
    geo::EditRegion region;
    std::vector<u32> all(mesh.vertexCount());
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        all[v] = v;
    }
    region.collect(mesh, all, &none);
    EXPECT_FALSE(region.lastWasParallel());
    EXPECT_GT(geo::EditRegion::parallelStampBytes(), 0u);
}

namespace {

struct StrokeRun {
    std::vector<u64> dabHashes;
    std::vector<sculpt::StrokeDelta> deltas;
    std::vector<u32> picks;
    std::unique_ptr<geo::ChunkedBvh> finalBvh;
    u32 parallelRegions = 0;
};

/// Draw, Smooth and Move strokes with dabs of about 25K moved vertices (above
/// the parallel threshold), then a small one, as in the parallel-marking test.
StrokeRun runStrokes(bool parallelRegion, unsigned helpers)
{
    geo::Mesh mesh = geo::makeIcosphere(7, 1.0f, geo::VertexOrder::Spatial);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, geo::ChunkedBvh::kDefaultChunkTriangleShift, nullptr, true);
    std::unique_ptr<core::JobSystem> jobs = helpers > 0 ? std::make_unique<core::JobSystem>(helpers) : nullptr;
    sculpt::StrokeProcessor stroke(mesh, bvh);
    stroke.setJobSystem(jobs.get());
    stroke.setParallelGather(true);
    stroke.setParallelMarking(true);
    stroke.setParallelRegion(parallelRegion);
    StrokeRun run;
    stroke.setCommitHooks([] {}, [&] { run.dabHashes.push_back(hashMesh(mesh)); });
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
        run.parallelRegions += stroke.stats().normals.parallelRegions;
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

TEST(ParallelRegion, StrokesMatchTheSerialWalkAtEveryThreadCount)
{
    const StrokeRun serial = runStrokes(false, 3);
    ASSERT_GT(serial.dabHashes.size(), 20u);
    EXPECT_EQ(serial.parallelRegions, 0u);
    for (const unsigned helpers : {0u, 1u, 3u, 11u}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const StrokeRun parallel = runStrokes(true, helpers);
        if (helpers > 0) {
            EXPECT_GT(parallel.parallelRegions, 10u) << "the dabs must be large enough to walk in parallel";
        } else {
            EXPECT_EQ(parallel.parallelRegions, 0u);
        }
        EXPECT_EQ(parallel.dabHashes, serial.dabHashes);
        ASSERT_EQ(parallel.deltas.size(), serial.deltas.size());
        for (std::size_t i = 0; i < serial.deltas.size(); ++i) {
            EXPECT_EQ(parallel.deltas[i].vertices, serial.deltas[i].vertices) << "stroke " << i;
            EXPECT_TRUE(test::bitwiseEqual(parallel.deltas[i].after, serial.deltas[i].after)) << "stroke " << i;
        }
        EXPECT_TRUE(parallel.finalBvh->sameBounds(*serial.finalBvh)) << "boxes or cones differ";
        EXPECT_EQ(parallel.picks, serial.picks);
    }
}

namespace {

struct HistoryRun {
    std::vector<u64> hashes; // after every stroke, undo and redo
    std::vector<std::vector<u32>> picks;
    std::vector<std::unique_ptr<geo::ChunkedBvh>> trees;
    int parallelHistory = 0;
};

HistoryRun runHistory(bool parallelRegion, int helpers)
{
    app::AppConfig config;
    config.sphereSubdivisions = 7;
    config.jobThreads = helpers;
    config.parallelRegion = parallelRegion;
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
    const auto history = [&] {
        snapshot();
        run.parallelHistory += controller.stats().lastHistory.parallelRegion ? 1 : 0;
    };
    for (int i = 0; i < 4; ++i) {
        controller.undo();
        history();
    }
    for (int i = 0; i < 4; ++i) {
        controller.redo();
        history();
    }
    return run;
}

} // namespace

TEST(ParallelRegion, UndoRedoThroughControllerMatchTheSerialWalk)
{
    const HistoryRun serial = runHistory(false, 3);
    ASSERT_EQ(serial.hashes.size(), 13u);
    EXPECT_EQ(serial.hashes[8], serial.hashes[0]) << "undo is not exact";
    EXPECT_EQ(serial.hashes[12], serial.hashes[4]) << "redo is not exact";
    EXPECT_EQ(serial.parallelHistory, 0);
    for (const int helpers : {0, 1, 3, 11}) {
        SCOPED_TRACE(testing::Message() << helpers << " helper(s)");
        const HistoryRun parallel = runHistory(true, helpers);
        if (helpers > 0) {
            EXPECT_EQ(parallel.parallelHistory, 8) << "every undo and redo moves more than the threshold";
        }
        ASSERT_EQ(parallel.hashes.size(), serial.hashes.size());
        for (std::size_t i = 0; i < serial.hashes.size(); ++i) {
            EXPECT_EQ(parallel.hashes[i], serial.hashes[i]) << "step " << i;
            EXPECT_EQ(parallel.picks[i], serial.picks[i]) << "step " << i;
            EXPECT_TRUE(parallel.trees[i]->sameBounds(*serial.trees[i])) << "step " << i;
        }
    }
}
