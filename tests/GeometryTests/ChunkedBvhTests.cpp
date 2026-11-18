#include "Core/JobSystem.h"
#include "Geometry/Bvh.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

using namespace se;

namespace {

/// Checks every invariant of the two-level structure against the mesh itself.
void expectValid(const geo::Mesh& mesh, const geo::ChunkedBvh& bvh)
{
    const auto positions = mesh.positions();
    std::vector<u32> stored(mesh.triangleCount(), 0);
    geo::Aabb whole;

    for (u32 chunkIndex = 0; chunkIndex < bvh.chunkCount(); ++chunkIndex) {
        const geo::Bvh& tree = bvh.chunk(chunkIndex);
        ASSERT_EQ(tree.firstTriangle(), chunkIndex << bvh.chunkTriangleShift());
        ASSERT_LE(tree.triangleCount(), 1u << bvh.chunkTriangleShift());

        // Local tree: tight leaves, inner boxes are unions, each triangle stored once.
        for (u32 index = 0; index < tree.nodeCount(); ++index) {
            const geo::Bvh::Node& node = tree.nodes()[index];
            if (node.isLeaf()) {
                geo::Aabb exact;
                for (u32 k = 0; k < node.triangleCount; ++k) {
                    const geo::TriangleId triangle = tree.triangleIndices()[node.leftOrFirst + k];
                    ASSERT_EQ(bvh.chunkOf(triangle), chunkIndex) << "triangle stored in the wrong chunk";
                    ASSERT_EQ(tree.leafOf(triangle), index);
                    ++stored[triangle];
                    for (const geo::VertexId vertex : mesh.triangle(triangle)) {
                        exact.expand(positions[vertex]);
                    }
                }
                ASSERT_TRUE(node.bounds == exact) << "chunk " << chunkIndex << " leaf " << index << " is not tight";
            } else {
                geo::Aabb merged = tree.nodes()[node.leftOrFirst].bounds;
                merged.expand(tree.nodes()[node.leftOrFirst + 1].bounds);
                ASSERT_TRUE(node.bounds == merged) << "chunk " << chunkIndex << " node " << index;
            }
        }
        whole.expand(tree.bounds());
    }
    ASSERT_TRUE(std::all_of(stored.begin(), stored.end(), [](u32 c) { return c == 1; }));

    // Top level: one leaf per chunk carrying the chunk's box; inner boxes are unions.
    std::vector<u32> chunkLeaves(bvh.chunkCount(), 0);
    const auto& top = bvh.topNodes();
    for (u32 index = 0; index < top.size(); ++index) {
        const geo::ChunkedBvh::TopNode& node = top[index];
        if (node.isLeaf()) {
            ASSERT_LT(node.chunk, bvh.chunkCount());
            ++chunkLeaves[node.chunk];
            ASSERT_TRUE(node.bounds == bvh.chunk(node.chunk).bounds()) << "top leaf of chunk " << node.chunk;
        } else {
            ASSERT_GT(node.left, index);
            geo::Aabb merged = top[node.left].bounds;
            merged.expand(top[node.left + 1].bounds);
            ASSERT_TRUE(node.bounds == merged) << "top node " << index;
        }
    }
    ASSERT_TRUE(std::all_of(chunkLeaves.begin(), chunkLeaves.end(), [](u32 c) { return c == 1; }));
    if (bvh.chunkCount() > 0) {
        ASSERT_TRUE(bvh.bounds() == whole);
    }
}

void expectMatchesBruteForce(const geo::Mesh& mesh, const geo::ChunkedBvh& bvh, unsigned seed, int rayCount)
{
    std::mt19937 rng(seed);
    int hits = 0;
    for (int i = 0; i < rayCount; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const std::optional<float> expected = test::bruteForceRaycast(mesh, ray);
        const std::optional<geo::RayHit> actual = bvh.raycast(mesh, ray);
        ASSERT_EQ(expected.has_value(), actual.has_value()) << "ray " << i;
        if (expected) {
            ++hits;
            ASSERT_NEAR(*expected, actual->distance, 1e-4f) << "ray " << i;
            ASSERT_LT(actual->triangle, mesh.triangleCount());
        }
    }
    EXPECT_GT(hits, rayCount / 10);
    EXPECT_LT(hits, rayCount);
}

/// Pushes the vertices near `center` and returns their ids.
std::vector<u32> deformRegion(geo::Mesh& mesh, const glm::vec3& center, float radius, const glm::vec3& offset)
{
    std::vector<u32> moved;
    const auto positions = mesh.mutablePositions();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        const float distance = glm::distance(positions[vertex], center);
        if (distance < radius) {
            positions[vertex] += offset * (1.0f - distance / radius);
            moved.push_back(vertex);
        }
    }
    return moved;
}

} // namespace

TEST(ChunkedBvh, EmptyMesh)
{
    const geo::Mesh mesh;
    geo::ChunkedBvh bvh;
    bvh.build(mesh);
    EXPECT_TRUE(bvh.empty());
    EXPECT_EQ(bvh.chunkCount(), 0u);
    EXPECT_FALSE(bvh.bounds().valid());
    EXPECT_FALSE(bvh.raycast(mesh, {{0, 0, 5}, {0, 0, -1}}).has_value());
    std::vector<u32> triangles;
    bvh.querySphere({0, 0, 0}, 1.0f, triangles);
    EXPECT_TRUE(triangles.empty());
    EXPECT_FALSE(bvh.needsOptimize());
}

TEST(ChunkedBvh, ChunksPartitionTheTriangles)
{
    const geo::Mesh mesh = geo::makeIcosphere(4); // 5,120 triangles
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 6); // 64 triangles per chunk
    EXPECT_EQ(bvh.chunkCount(), 80u);
    EXPECT_EQ(bvh.chunkOf(63), 0u);
    EXPECT_EQ(bvh.chunkOf(64), 1u);
    expectValid(mesh, bvh);

    // A chunk size that does not divide the triangle count: the last chunk is partial.
    bvh.build(mesh, 10);
    EXPECT_EQ(bvh.chunkCount(), 5u);
    EXPECT_EQ(bvh.chunk(4).triangleCount(), 5120u - 4u * 1024u);
    expectValid(mesh, bvh);

    // One chunk holding everything is a valid degenerate case.
    bvh.build(mesh, 20);
    EXPECT_EQ(bvh.chunkCount(), 1u);
    expectValid(mesh, bvh);
}

TEST(ChunkedBvh, ChunksAreSpatiallyCompact)
{
    // Spatially ordered triangles: a chunk covers a small patch of the sphere.
    const geo::Mesh mesh = geo::makeIcosphere(6);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 10); // 1,024 of 81,920 triangles per chunk = 1/80 of the surface
    float largest = 0.0f;
    float total = 0.0f;
    for (u32 index = 0; index < bvh.chunkCount(); ++index) {
        const glm::vec3 extent = bvh.chunk(index).bounds().extent();
        const float size = std::max({extent.x, extent.y, extent.z});
        largest = std::max(largest, size);
        total += size;
    }
    const float mean = total / static_cast<float>(bvh.chunkCount());
    // An ideal patch of 1/80 of the sphere is about 0.4 across.
    EXPECT_LT(mean, 0.6f) << "mean chunk extent on a sphere of diameter 2";
    EXPECT_LT(largest, 1.2f) << "largest chunk extent (mean " << mean << ")";
}

TEST(ChunkedBvh, ChunksShareBorderVertices)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 6);
    // Vertices used by triangles of more than one chunk exist, and are single vertices.
    u32 borderVertices = 0;
    for (geo::VertexId v = 0; v < mesh.vertexCount(); ++v) {
        const auto triangles = mesh.incidentTriangles(v);
        const u32 first = bvh.chunkOf(triangles.front());
        borderVertices += std::any_of(triangles.begin(), triangles.end(), [&](u32 t) { return bvh.chunkOf(t) != first; }) ? 1u : 0u;
    }
    EXPECT_GT(borderVertices, bvh.chunkCount());
    EXPECT_EQ(mesh.vertexCount(), geo::icosphereVertexCount(4));
}

TEST(ChunkedBvh, RaycastMatchesBruteForceForAnyChunkSize)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    for (const u32 shift : {4u, 6u, 9u, 12u, 20u}) {
        geo::ChunkedBvh bvh;
        bvh.build(mesh, shift);
        expectMatchesBruteForce(mesh, bvh, 100u + shift, 250);
    }
}

TEST(ChunkedBvh, RaycastAgreesWithTheSingleTree)
{
    const geo::Mesh mesh = geo::makeIcosphere(5);
    geo::Bvh single;
    single.build(mesh);
    geo::ChunkedBvh chunked;
    chunked.build(mesh, 7);

    std::mt19937 rng(3);
    for (int i = 0; i < 500; ++i) {
        const geo::Ray ray = test::randomRayTowardsOrigin(rng);
        const auto a = single.raycast(mesh, ray);
        const auto b = chunked.raycast(mesh, ray);
        ASSERT_EQ(a.has_value(), b.has_value());
        if (a) {
            ASSERT_FLOAT_EQ(a->distance, b->distance);
            ASSERT_NEAR(glm::distance(a->normal, b->normal), 0.0f, 1e-6f);
        }
    }
    // Rays from inside, and with a distance limit.
    const auto inside = chunked.raycast(mesh, {{0, 0, 0}, {0, 1, 0}});
    ASSERT_TRUE(inside.has_value());
    EXPECT_NEAR(inside->distance, 1.0f, 5e-3f);
    EXPECT_FALSE(chunked.raycast(mesh, {{0, 0, 5}, {0, 0, -1}}, 3.5f).has_value());
}

TEST(ChunkedBvh, SphereQueryFindsEveryTriangleAcrossChunks)
{
    const geo::Mesh mesh = geo::makeIcosphere(5);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 6);

    const glm::vec3 center(0.0f, 0.6f, 0.8f);
    const float radius = 0.4f;
    std::vector<u32> found;
    bvh.querySphere(center, radius, found);
    std::sort(found.begin(), found.end());
    ASSERT_TRUE(std::adjacent_find(found.begin(), found.end()) == found.end()) << "duplicates";

    std::vector<u32> chunksHit;
    u32 required = 0;
    for (geo::TriangleId t = 0; t < mesh.triangleCount(); ++t) {
        bool inside = false;
        for (const geo::VertexId v : mesh.triangle(t)) {
            inside = inside || glm::distance(mesh.positions()[v], center) < radius;
        }
        if (inside) {
            ++required;
            ASSERT_TRUE(std::binary_search(found.begin(), found.end(), t)) << "missing triangle " << t;
            chunksHit.push_back(bvh.chunkOf(t));
        }
    }
    std::sort(chunksHit.begin(), chunksHit.end());
    chunksHit.erase(std::unique(chunksHit.begin(), chunksHit.end()), chunksHit.end());
    EXPECT_GT(required, 0u);
    EXPECT_GT(chunksHit.size(), 4u) << "the query should span several chunks";
    EXPECT_LT(found.size(), mesh.triangleCount() / 3);
}

TEST(ChunkedBvh, IncrementalRefitEqualsFullRefit)
{
    geo::Mesh mesh = geo::makeIcosphere(5);
    geo::ChunkedBvh incremental;
    incremental.build(mesh, 7); // 128 triangles per chunk, 160 chunks

    const glm::vec3 centers[] = {{0, 0, 1}, {0.2f, 0.1f, 0.97f}, {-1, 0, 0}, {0, 0, 1}};
    const glm::vec3 offsets[] = {{0, 0, 0.5f}, {0.3f, 0, 0}, {-0.4f, 0.2f, 0}, {0, 0, -0.45f}};
    for (int step = 0; step < 4; ++step) {
        const std::vector<u32> moved = deformRegion(mesh, centers[step], 0.35f, offsets[step]);
        ASSERT_FALSE(moved.empty());
        incremental.refit(mesh, moved);

        geo::ChunkedBvh full = incremental;
        full.refit(mesh);
        ASSERT_TRUE(incremental.sameBounds(full)) << "after step " << step;
        expectValid(mesh, incremental);
    }
    expectMatchesBruteForce(mesh, incremental, 321u, 300);
}

TEST(ChunkedBvh, EditAcrossChunkBordersUpdatesEveryAffectedChunk)
{
    geo::Mesh mesh = geo::makeIcosphere(5);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 6);
    std::vector<geo::Aabb> before(bvh.chunkCount());
    for (u32 index = 0; index < bvh.chunkCount(); ++index) {
        before[index] = bvh.chunk(index).bounds();
    }

    const std::vector<u32> moved = deformRegion(mesh, {0, 0, 1}, 0.3f, {0, 0, 0.4f});
    bvh.refit(mesh, moved);

    u32 changed = 0;
    for (u32 index = 0; index < bvh.chunkCount(); ++index) {
        changed += bvh.chunk(index).bounds() == before[index] ? 0u : 1u;
    }
    EXPECT_GT(changed, 3u) << "the deformation should cross chunk borders";
    EXPECT_LT(changed, bvh.chunkCount() / 2) << "and still be local";
    expectValid(mesh, bvh);
    EXPECT_GT(bvh.bounds().max.z, 1.3f) << "the top level must see the change";
    expectMatchesBruteForce(mesh, bvh, 9u, 300);

    // No moved vertices: nothing happens.
    const geo::ChunkedBvh snapshot = bvh;
    bvh.refit(mesh, {});
    EXPECT_TRUE(bvh.sameBounds(snapshot));
}

TEST(ChunkedBvh, OptimizeIsSteppedLocalAndRebuildsTheTopLevel)
{
    geo::Mesh mesh = geo::makeIcosphere(6);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 9); // 512 triangles per chunk, 160 chunks
    EXPECT_FALSE(bvh.needsOptimize());
    EXPECT_EQ(bvh.optimize(mesh), 0u);

    const std::vector<u32> moved = deformRegion(mesh, {0, 0, 1}, 0.5f, {0.9f, -0.6f, 0.4f});
    bvh.refit(mesh, moved);
    ASSERT_TRUE(bvh.needsOptimize());

    geo::ChunkedBvh atOnce = bvh;
    const u32 total = atOnce.optimize(mesh);
    EXPECT_FALSE(atOnce.needsOptimize());
    EXPECT_GT(total, 0u);
    EXPECT_LT(total, mesh.triangleCount() / 2) << "optimize must stay local";
    expectValid(mesh, atOnce);
    expectMatchesBruteForce(mesh, atOnce, 71u, 300);

    // In small steps: valid and correct after every one.
    u32 steppedTotal = 0;
    int steps = 0;
    while (bvh.needsOptimize()) {
        steppedTotal += bvh.optimize(mesh, 1);
        ASSERT_LT(++steps, 10000);
        if (steps % 5 == 1) {
            expectValid(mesh, bvh);
            expectMatchesBruteForce(mesh, bvh, 500u + static_cast<unsigned>(steps), 40);
        }
    }
    EXPECT_GT(steps, 3);
    EXPECT_EQ(steppedTotal, total);
    EXPECT_TRUE(bvh.sameBounds(atOnce));
    EXPECT_EQ(bvh.optimize(mesh), 0u);

    // A new edit while another region is only partly optimized is not lost.
    bvh.refit(mesh, deformRegion(mesh, {-1, 0, 0}, 0.5f, {-0.3f, 0.2f, 0.1f}));
    bvh.optimize(mesh, 1);
    bvh.refit(mesh, deformRegion(mesh, {0, -1, 0}, 0.5f, {0.1f, -0.4f, 0.0f}));
    expectValid(mesh, bvh);
    while (bvh.needsOptimize()) {
        bvh.optimize(mesh, 1);
    }
    expectValid(mesh, bvh);
    expectMatchesBruteForce(mesh, bvh, 72u, 300);
}

TEST(ChunkedBvh, FullRefitThenOptimizeCoversEverything)
{
    geo::Mesh mesh = geo::makeIcosphere(4);
    geo::ChunkedBvh bvh;
    bvh.build(mesh, 8);
    for (glm::vec3& position : mesh.mutablePositions()) {
        position.y *= 1.5f;
    }
    bvh.refit(mesh);
    expectValid(mesh, bvh);
    EXPECT_EQ(bvh.optimize(mesh), mesh.triangleCount());
    expectValid(mesh, bvh);
    expectMatchesBruteForce(mesh, bvh, 5u, 200);
}

TEST(ChunkedBvh, ParallelBuildAndRefitEqualSerial)
{
    core::JobSystem jobs(4);
    geo::Mesh mesh = geo::makeIcosphere(7);
    geo::ChunkedBvh serial;
    serial.build(mesh, 9); // 640 chunks
    geo::ChunkedBvh parallel;
    parallel.build(mesh, 9, &jobs);
    ASSERT_TRUE(serial.sameBounds(parallel)) << "parallel build differs";
    ASSERT_EQ(serial.nodeCount(), parallel.nodeCount());

    const glm::vec3 centers[] = {{0, 0, 1}, {0.3f, 0.1f, 0.95f}, {0, 0, 1}};
    const glm::vec3 offsets[] = {{0, 0, 0.5f}, {0.2f, -0.2f, 0.1f}, {0, 0, -0.4f}};
    for (int step = 0; step < 3; ++step) {
        const std::vector<u32> moved = deformRegion(mesh, centers[step], 0.6f, offsets[step]);
        serial.refit(mesh, moved);
        parallel.refit(mesh, moved, &jobs);
        ASSERT_TRUE(serial.sameBounds(parallel)) << "after step " << step;
    }
    serial.optimize(mesh);
    parallel.optimize(mesh);
    EXPECT_TRUE(serial.sameBounds(parallel));
    expectValid(mesh, parallel);
}
