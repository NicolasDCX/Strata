#include "Core/JobSystem.h"
#include "Geometry/Bvh.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

using namespace se;

namespace {

void expectMatchesBruteForce(const geo::Mesh& mesh, const geo::Bvh& bvh, unsigned seed, int rayCount)
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
        }
    }
    // The ray generator must exercise both outcomes for the test to mean anything.
    EXPECT_GT(hits, rayCount / 10);
    EXPECT_LT(hits, rayCount);
}

} // namespace

TEST(Bvh, EmptyMeshNeverHits)
{
    const geo::Mesh mesh;
    geo::Bvh bvh;
    bvh.build(mesh);
    EXPECT_TRUE(bvh.empty());
    EXPECT_FALSE(bvh.raycast(mesh, {{0, 0, 5}, {0, 0, -1}}).has_value());
    std::vector<u32> triangles;
    bvh.querySphere({0, 0, 0}, 1.0f, triangles);
    EXPECT_TRUE(triangles.empty());
}

TEST(Bvh, EveryTriangleIsStoredExactlyOnce)
{
    const geo::Mesh mesh = geo::makeIcosphere(3);
    geo::Bvh bvh;
    bvh.build(mesh);

    std::vector<u32> counts(mesh.triangleCount(), 0);
    for (const geo::Bvh::Node& node : bvh.nodes()) {
        if (!node.isLeaf()) {
            continue;
        }
        ASSERT_LE(node.triangleCount, geo::Bvh::kMaxLeafTriangles);
        for (u32 k = 0; k < node.triangleCount; ++k) {
            ++counts[bvh.triangleIndices()[node.leftOrFirst + k]];
        }
    }
    EXPECT_TRUE(std::all_of(counts.begin(), counts.end(), [](u32 c) { return c == 1; }));
}

TEST(Bvh, NodeBoundsEncloseTheirTriangles)
{
    const geo::Mesh mesh = geo::makeIcosphere(3);
    geo::Bvh bvh;
    bvh.build(mesh);
    const auto positions = mesh.positions();
    const auto indices = mesh.indices();
    for (const geo::Bvh::Node& node : bvh.nodes()) {
        if (node.isLeaf()) {
            for (u32 k = 0; k < node.triangleCount; ++k) {
                const u32 triangle = bvh.triangleIndices()[node.leftOrFirst + k];
                for (int corner = 0; corner < 3; ++corner) {
                    ASSERT_TRUE(node.bounds.contains(positions[indices[triangle * 3 + corner]]));
                }
            }
        } else {
            for (u32 child = node.leftOrFirst; child < node.leftOrFirst + 2; ++child) {
                ASSERT_GT(child, static_cast<u32>(&node - bvh.nodes().data())); // children after parent
                ASSERT_TRUE(node.bounds.contains(bvh.nodes()[child].bounds.min));
                ASSERT_TRUE(node.bounds.contains(bvh.nodes()[child].bounds.max));
            }
        }
    }
}

TEST(Bvh, RaycastHitsSphereAtExpectedPoint)
{
    const geo::Mesh mesh = geo::makeIcosphere(5);
    geo::Bvh bvh;
    bvh.build(mesh);

    const geo::Ray ray{{0, 0, 5}, {0, 0, -1}};
    const std::optional<geo::RayHit> hit = bvh.raycast(mesh, ray);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance, 4.0f, 2e-3f); // faceted sphere: slightly inside the unit sphere
    EXPECT_NEAR(hit->position.z, 1.0f, 2e-3f);
    EXPECT_NEAR(hit->position.x, 0.0f, 1e-5f);
    EXPECT_GT(hit->normal.z, 0.99f); // faces the ray origin
    ASSERT_LT(hit->triangle, mesh.triangleCount());

    EXPECT_FALSE(bvh.raycast(mesh, {{0, 3, 5}, {0, 0, -1}}).has_value());
    EXPECT_FALSE(bvh.raycast(mesh, ray, 3.5f).has_value()) << "maxDistance must be honoured";
}

TEST(Bvh, RaycastFromInsideHitsTheFarSide)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    geo::Bvh bvh;
    bvh.build(mesh);
    const std::optional<geo::RayHit> hit = bvh.raycast(mesh, {{0, 0, 0}, {1, 0, 0}});
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance, 1.0f, 5e-3f);
}

TEST(Bvh, MatchesBruteForceOnRandomRays)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    geo::Bvh bvh;
    bvh.build(mesh);
    expectMatchesBruteForce(mesh, bvh, 1234u, 400);
}

TEST(Bvh, RefitKeepsQueriesCorrectAfterDeformation)
{
    geo::Mesh mesh = geo::makeIcosphere(4);
    geo::Bvh bvh;
    bvh.build(mesh);

    // Deform a large region well outside the original bounds.
    for (glm::vec3& position : mesh.mutablePositions()) {
        if (position.y > 0.3f) {
            position += glm::vec3(0.4f, 0.9f * position.y, -0.2f);
        }
    }

    // Without refit the old boxes no longer contain the geometry: this ray hits
    // the moved cap, which now lies outside the stale root box.
    const geo::Ray capRay{{0.4f, 5.0f, -0.2f}, {0, -1, 0}};
    ASSERT_TRUE(test::bruteForceRaycast(mesh, capRay).has_value());

    bvh.refit(mesh);
    const std::optional<geo::RayHit> capHit = bvh.raycast(mesh, capRay);
    ASSERT_TRUE(capHit.has_value());
    EXPECT_NEAR(capHit->distance, *test::bruteForceRaycast(mesh, capRay), 1e-4f);

    expectMatchesBruteForce(mesh, bvh, 99u, 400);

    // A fresh build over the deformed mesh must agree with the refitted tree.
    geo::Bvh rebuilt;
    rebuilt.build(mesh);
    expectMatchesBruteForce(mesh, rebuilt, 99u, 400);
}

TEST(Bvh, SphereQueryReturnsEveryTriangleTouchingTheSphere)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    geo::Bvh bvh;
    bvh.build(mesh);

    const glm::vec3 center(0.0f, 0.0f, 1.0f);
    const float radius = 0.3f;
    std::vector<u32> found;
    bvh.querySphere(center, radius, found);
    std::sort(found.begin(), found.end());
    ASSERT_TRUE(std::adjacent_find(found.begin(), found.end()) == found.end()) << "duplicates";

    // Conservative superset: every triangle with a vertex inside must be reported.
    const auto positions = mesh.positions();
    const auto indices = mesh.indices();
    u32 required = 0;
    for (u32 triangle = 0; triangle < mesh.triangleCount(); ++triangle) {
        bool inside = false;
        for (int corner = 0; corner < 3; ++corner) {
            inside = inside || glm::distance(positions[indices[triangle * 3 + corner]], center) < radius;
        }
        if (inside) {
            ++required;
            ASSERT_TRUE(std::binary_search(found.begin(), found.end(), triangle)) << "missing triangle " << triangle;
        }
    }
    EXPECT_GT(required, 0u);
    // ...and it must actually cull: far fewer than the whole mesh.
    EXPECT_LT(found.size(), mesh.triangleCount() / 4);

    std::vector<u32> none;
    bvh.querySphere({10, 10, 10}, 0.5f, none);
    EXPECT_TRUE(none.empty());
}

namespace {

/// Checks every structural invariant the BVH relies on.
void expectStructurallyValid(const geo::Mesh& mesh, const geo::Bvh& bvh)
{
    const auto positions = mesh.positions();
    const auto indices = mesh.indices();
    std::vector<u32> stored(mesh.triangleCount(), 0);

    ASSERT_EQ(bvh.parentOf(0), kInvalidIndex);
    for (u32 index = 0; index < bvh.nodeCount(); ++index) {
        const geo::Bvh::Node& node = bvh.nodes()[index];
        if (node.isLeaf()) {
            geo::Aabb exact;
            for (u32 k = 0; k < node.triangleCount; ++k) {
                const u32 triangle = bvh.triangleIndices()[node.leftOrFirst + k];
                ++stored[triangle];
                ASSERT_EQ(bvh.leafOf(triangle), index);
                for (int corner = 0; corner < 3; ++corner) {
                    exact.expand(positions[indices[triangle * 3 + corner]]);
                }
            }
            ASSERT_TRUE(node.bounds == exact) << "leaf " << index << " box is not tight";
        } else {
            const u32 left = node.leftOrFirst;
            ASSERT_GT(left, index);
            ASSERT_EQ(bvh.parentOf(left), index);
            ASSERT_EQ(bvh.parentOf(left + 1), index);
            geo::Aabb merged = bvh.nodes()[left].bounds;
            merged.expand(bvh.nodes()[left + 1].bounds);
            ASSERT_TRUE(node.bounds == merged) << "inner node " << index << " is not the union of its children";
        }
    }
    ASSERT_TRUE(std::all_of(stored.begin(), stored.end(), [](u32 c) { return c == 1; }));
}

/// Pushes the vertices near `center` outwards and returns their indices.
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

TEST(Bvh, FreshBuildIsStructurallyValid)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    geo::Bvh bvh;
    bvh.build(mesh);
    expectStructurallyValid(mesh, bvh);
}

TEST(Bvh, IncrementalRefitEqualsFullRefit)
{
    geo::Mesh mesh = geo::makeIcosphere(5);
    geo::Bvh incremental;
    incremental.build(mesh);

    // Several overlapping edits, some growing and some shrinking the boxes.
    const glm::vec3 centers[] = {{0, 0, 1}, {0.2f, 0.1f, 0.97f}, {-1, 0, 0}, {0, 0, 1}};
    const glm::vec3 offsets[] = {{0, 0, 0.5f}, {0.3f, 0, 0}, {-0.4f, 0.2f, 0}, {0, 0, -0.45f}};
    for (int step = 0; step < 4; ++step) {
        const std::vector<u32> moved = deformRegion(mesh, centers[step], 0.35f, offsets[step]);
        ASSERT_FALSE(moved.empty());
        incremental.refit(mesh, moved);

        geo::Bvh full = incremental; // same topology
        full.refit(mesh);
        ASSERT_EQ(full.nodeCount(), incremental.nodeCount());
        for (u32 index = 0; index < full.nodeCount(); ++index) {
            ASSERT_TRUE(full.nodes()[index].bounds == incremental.nodes()[index].bounds)
                << "node " << index << " after step " << step;
        }
    }
    expectStructurallyValid(mesh, incremental);
    expectMatchesBruteForce(mesh, incremental, 321u, 300);
}

TEST(Bvh, IncrementalRefitWithNoMovedVerticesIsANoOp)
{
    const geo::Mesh mesh = geo::makeIcosphere(3);
    geo::Bvh bvh;
    bvh.build(mesh);
    const std::vector<geo::Bvh::Node> before = bvh.nodes();
    bvh.refit(mesh, {});
    for (u32 index = 0; index < bvh.nodeCount(); ++index) {
        ASSERT_TRUE(before[index].bounds == bvh.nodes()[index].bounds);
    }
    EXPECT_EQ(bvh.optimize(mesh), 0u);
}

TEST(Bvh, OptimizeRepartitionsOnlyTouchedBlocks)
{
    geo::Mesh mesh = geo::makeIcosphere(6); // 81,920 triangles, 40 blocks
    geo::Bvh bvh;
    bvh.build(mesh);
    EXPECT_EQ(bvh.optimize(mesh), 0u) << "nothing was refitted yet";

    // A strong local deformation that scrambles the spatial order of the region.
    const std::vector<u32> moved = deformRegion(mesh, {0, 0, 1}, 0.3f, {0.9f, -0.6f, 0.4f});
    bvh.refit(mesh, moved);
    const std::size_t nodesBefore = bvh.nodeCount();

    const u32 rebuilt = bvh.optimize(mesh);
    EXPECT_GT(rebuilt, 0u);
    EXPECT_LT(rebuilt, mesh.triangleCount() / 4) << "optimize must stay local";
    EXPECT_EQ(bvh.nodeCount(), nodesBefore) << "in-place rebuild must reuse the same node slots";

    expectStructurallyValid(mesh, bvh);
    expectMatchesBruteForce(mesh, bvh, 77u, 300);
    EXPECT_EQ(bvh.optimize(mesh), 0u) << "a second optimize has nothing left to do";

    // Sphere queries and further incremental refits keep working on the optimized tree.
    const std::vector<u32> movedAgain = deformRegion(mesh, {0.9f, -0.6f, 1.4f}, 0.4f, {0, 0.3f, 0});
    bvh.refit(mesh, movedAgain);
    expectStructurallyValid(mesh, bvh);
    expectMatchesBruteForce(mesh, bvh, 78u, 300);
}

TEST(Bvh, OptimizeAfterFullRefitCoversTheWholeTree)
{
    geo::Mesh mesh = geo::makeIcosphere(4);
    geo::Bvh bvh;
    bvh.build(mesh);
    for (glm::vec3& position : mesh.mutablePositions()) {
        position.y *= 1.5f;
    }
    bvh.refit(mesh);
    EXPECT_EQ(bvh.optimize(mesh), mesh.triangleCount());
    expectStructurallyValid(mesh, bvh);
    expectMatchesBruteForce(mesh, bvh, 5u, 200);
}
TEST(Bvh, OptimizeCanBeSpreadOverSeveralCalls)
{
    geo::Mesh mesh = geo::makeIcosphere(6);
    geo::Bvh stepped;
    stepped.build(mesh);
    EXPECT_FALSE(stepped.needsOptimize());

    const std::vector<u32> moved = deformRegion(mesh, {0, 0, 1}, 0.6f, {0.5f, -0.3f, 0.4f});
    stepped.refit(mesh, moved);
    EXPECT_TRUE(stepped.needsOptimize());

    geo::Bvh atOnce = stepped;
    const u32 total = atOnce.optimize(mesh);
    EXPECT_FALSE(atOnce.needsOptimize());
    ASSERT_GT(total, 3 * geo::Bvh::kOptimizeBlockTriangles) << "test needs several blocks";

    // One block per call. The tree must answer queries correctly after every step.
    u32 steppedTotal = 0;
    int steps = 0;
    while (stepped.needsOptimize()) {
        const u32 done = stepped.optimize(mesh, 1);
        ASSERT_GT(done, 0u);
        ASSERT_LE(done, geo::Bvh::kOptimizeBlockTriangles);
        steppedTotal += done;
        ++steps;
        ASSERT_LT(steps, 1000);
        if (steps % 3 == 1) {
            expectStructurallyValid(mesh, stepped);
            expectMatchesBruteForce(mesh, stepped, 900u + static_cast<unsigned>(steps), 60);
        }
    }
    EXPECT_GT(steps, 3);
    EXPECT_EQ(steppedTotal, total);
    EXPECT_EQ(stepped.optimize(mesh, 1), 0u);

    // Stepping must end in exactly the tree the single call produces.
    ASSERT_EQ(stepped.nodeCount(), atOnce.nodeCount());
    EXPECT_EQ(stepped.triangleIndices(), atOnce.triangleIndices());
    for (u32 index = 0; index < stepped.nodeCount(); ++index) {
        ASSERT_TRUE(stepped.nodes()[index].bounds == atOnce.nodes()[index].bounds);
    }
}

TEST(Bvh, EditsBetweenOptimizeStepsAreNotLost)
{
    geo::Mesh mesh = geo::makeIcosphere(6);
    geo::Bvh bvh;
    bvh.build(mesh);

    bvh.refit(mesh, deformRegion(mesh, {0, 0, 1}, 0.5f, {0.3f, 0.2f, 0.4f}));
    bvh.optimize(mesh, 1); // partial
    // A new edit somewhere else while the first region is only partly optimized.
    bvh.refit(mesh, deformRegion(mesh, {-1, 0, 0}, 0.5f, {-0.4f, 0.3f, 0.0f}));
    expectStructurallyValid(mesh, bvh);
    expectMatchesBruteForce(mesh, bvh, 41u, 200);

    while (bvh.needsOptimize()) {
        bvh.optimize(mesh, 1);
    }
    expectStructurallyValid(mesh, bvh);
    expectMatchesBruteForce(mesh, bvh, 42u, 200);
}

TEST(Bvh, ParallelRefitEqualsSerialRefit)
{
    core::JobSystem jobs(4);
    geo::Mesh mesh = geo::makeIcosphere(7);
    geo::Bvh serial;
    serial.build(mesh);
    geo::Bvh parallel = serial;

    const glm::vec3 centers[] = {{0, 0, 1}, {0.3f, 0.1f, 0.95f}, {0, 0, 1}};
    const glm::vec3 offsets[] = {{0, 0, 0.5f}, {0.2f, -0.2f, 0.1f}, {0, 0, -0.4f}};
    for (int step = 0; step < 3; ++step) {
        const std::vector<u32> moved = deformRegion(mesh, centers[step], 0.6f, offsets[step]);
        ASSERT_GT(moved.size(), 4096u) << "needs enough leaves to split across threads";
        serial.refit(mesh, moved);
        parallel.refit(mesh, moved, &jobs);
        for (u32 index = 0; index < serial.nodeCount(); ++index) {
            ASSERT_TRUE(serial.nodes()[index].bounds == parallel.nodes()[index].bounds)
                << "node " << index << " after step " << step;
        }
    }
    expectStructurallyValid(mesh, parallel);
}

namespace {

/// The builder's recursion, written out: exact median split, leaves of up to kMaxLeafTriangles.
u32 referenceNodeCount(u32 count)
{
    if (count == 0) {
        return 0;
    }
    return count <= geo::Bvh::kMaxLeafTriangles ? 1u : 1u + referenceNodeCount(count / 2) + referenceNodeCount(count - count / 2);
}

/// The first `triangleCount` triangles of `source` (all vertices kept).
geo::Mesh truncatedMesh(const geo::Mesh& source, u32 triangleCount)
{
    std::vector<glm::vec3> positions(source.vertexCount());
    source.copyPositions(0, source.vertexCount(), positions.data());
    std::vector<u32> indices;
    indices.reserve(static_cast<std::size_t>(triangleCount) * 3);
    for (u32 t = 0; t < triangleCount; ++t) {
        for (const u32 corner : source.triangle(t)) {
            indices.push_back(corner);
        }
    }
    return geo::Mesh(std::move(positions), std::move(indices));
}

/// Counts around every split boundary of trees up to 2 chunks of 4,096: 1 to
/// 70, then 2^k - 1, 2^k, 2^k + 1 and 5 * 2^k - 1, 5 * 2^k, 5 * 2^k + 1 (leaves
/// of 4 make 4 * 2^k and 5 * 2^k the sizes where the shape changes).
std::vector<u32> boundaryCounts()
{
    std::vector<u32> counts;
    for (u32 n = 1; n <= 70; ++n) {
        counts.push_back(n);
    }
    for (u32 k = 3; k <= 13; ++k) {
        for (const u32 base : {1u << k, 5u << (k - 2)}) {
            for (const u32 n : {base - 1, base, base + 1}) {
                if (n <= 8192) {
                    counts.push_back(n);
                }
            }
        }
    }
    for (const u32 n : {1000u, 1234u, 3001u, 4095u, 4097u, 6143u, 8191u}) {
        counts.push_back(n);
    }
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
    return counts;
}

} // namespace

TEST(Bvh, NodeCountForMatchesTheBuildRecursion)
{
    static_assert(geo::Bvh::nodeCountFor(0) == 0);
    static_assert(geo::Bvh::nodeCountFor(4) == 1);
    static_assert(geo::Bvh::nodeCountFor(5) == 3);
    static_assert(geo::Bvh::nodeCountFor(4096) == 2047);
    for (u32 n = 0; n <= 200000; ++n) {
        ASSERT_EQ(geo::Bvh::nodeCountFor(n), referenceNodeCount(n)) << n;
    }
    // Large counts, including the 84M-triangle sphere as a single tree.
    for (const u32 n : {1u << 20, (1u << 20) + 1, (5u << 20) - 1, 83886080u, 100000007u}) {
        EXPECT_EQ(geo::Bvh::nodeCountFor(n), referenceNodeCount(n)) << n;
    }
}

TEST(Bvh, BuildReservesExactlyItsNodesForAnyRange)
{
    // 81,920 triangles: ranges of every boundary count at several offsets, and
    // a reused Bvh (any previous size) must give the tree a fresh one gives.
    const geo::Mesh mesh = geo::makeIcosphere(6);
    geo::Bvh reused;
    std::mt19937 rng(7);
    for (const u32 count : boundaryCounts()) {
        for (const u32 first : {0u, 4096u * 3, mesh.triangleCount() - count, static_cast<u32>(rng() % (mesh.triangleCount() - count))}) {
            geo::Bvh fresh;
            fresh.build(mesh, first, count);
            reused.build(mesh, first, count);
            const u32 expected = geo::Bvh::nodeCountFor(count);
            ASSERT_EQ(fresh.nodes().size(), expected) << count << " at " << first;
            ASSERT_EQ(fresh.nodes().capacity(), expected) << count << " at " << first;
            ASSERT_EQ(reused.nodes().size(), expected) << count << " at " << first;
            ASSERT_EQ(reused.nodes().capacity(), expected) << count << " at " << first;
            ASSERT_EQ(std::memcmp(fresh.nodes().data(), reused.nodes().data(), expected * sizeof(geo::Bvh::Node)), 0)
                << count << " at " << first;
            ASSERT_EQ(fresh.leafCount(), (expected + 1) / 2);
            // Every triangle of the range sits in a leaf of at most kMaxLeafTriangles that lists it.
            for (u32 t = first; t < first + count; ++t) {
                const geo::Bvh::Node& leaf = fresh.nodes()[fresh.leafOf(t)];
                ASSERT_TRUE(leaf.isLeaf());
                ASSERT_LE(leaf.triangleCount, geo::Bvh::kMaxLeafTriangles);
                ASSERT_EQ(reused.leafOf(t), fresh.leafOf(t));
            }
        }
    }
}

TEST(Bvh, PartialLastChunkHasExactNodeCountAndCorrectQueries)
{
    const geo::Mesh source = geo::makeIcosphere(5); // 20,480 triangles
    core::JobSystem jobs(3);
    for (const u32 triangles : {4096u * 3 + 1, 4096u * 4 - 1, 4096u * 2 + 5, 4096u + 4097u, 20480u - 3}) {
        const geo::Mesh mesh = truncatedMesh(source, triangles);
        for (core::JobSystem* threads : {static_cast<core::JobSystem*>(nullptr), &jobs}) {
            geo::ChunkedBvh bvh;
            bvh.build(mesh, 12, threads);
            ASSERT_EQ(bvh.chunkCount(), (triangles + 4095) / 4096);
            for (u32 c = 0; c < bvh.chunkCount(); ++c) {
                const u32 count = std::min(4096u, triangles - c * 4096u);
                ASSERT_EQ(bvh.chunk(c).nodes().size(), geo::Bvh::nodeCountFor(count)) << triangles << " chunk " << c;
                ASSERT_EQ(bvh.chunk(c).nodes().capacity(), geo::Bvh::nodeCountFor(count)) << triangles << " chunk " << c;
            }
            // Rays against brute force over the truncated (open) mesh.
            std::mt19937 rng(triangles);
            for (int i = 0; i < 200; ++i) {
                const geo::Ray ray = test::randomRayTowardsOrigin(rng);
                const std::optional<float> expected = test::bruteForceRaycast(mesh, ray);
                const std::optional<geo::RayHit> actual = bvh.raycast(mesh, ray);
                ASSERT_EQ(expected.has_value(), actual.has_value()) << triangles << " ray " << i;
                if (expected) {
                    ASSERT_NEAR(*expected, actual->distance, 1e-4f);
                }
            }
        }
    }
}
