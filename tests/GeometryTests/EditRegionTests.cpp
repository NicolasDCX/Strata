// EditRegion collects, in one walk, what an edit invalidates. Its lists must
// be exactly what the normal updater and the BVH refit used to find on their
// own: the same triangles in the same order, and the same vertex set.

#include "Core/JobSystem.h"
#include "Geometry/ChunkedBvh.h"
#include "Geometry/EditRegion.h"
#include "Geometry/Icosphere.h"
#include "TestUtils.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

using namespace se;

namespace {

/// Triangles of the moved vertices in first-occurrence order, each once.
std::vector<u32> expectedTriangles(const geo::Mesh& mesh, const std::vector<u32>& moved)
{
    std::vector<u8> seen(mesh.triangleCount(), 0);
    std::vector<u32> triangles;
    for (const u32 vertex : moved) {
        for (const u32 triangle : mesh.incidentTriangles(vertex)) {
            if (seen[triangle] == 0) {
                seen[triangle] = 1;
                triangles.push_back(triangle);
            }
        }
    }
    return triangles;
}

/// Moved vertices plus their neighbours, sorted.
std::vector<u32> expectedVertexSet(const geo::Mesh& mesh, const std::vector<u32>& moved)
{
    std::vector<u32> vertices(moved);
    for (const u32 vertex : moved) {
        for (const u32 neighbor : mesh.neighbors(vertex)) {
            vertices.push_back(neighbor);
        }
    }
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    return vertices;
}

std::vector<u32> movedNear(const geo::Mesh& mesh, const glm::vec3& center, float radius, u32 seed)
{
    std::vector<u32> moved;
    const auto positions = mesh.positions();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        if (glm::distance(positions[vertex], center) < radius) {
            moved.push_back(vertex);
        }
    }
    // The brush gathers in BVH order, not by id: any order must work.
    std::mt19937 rng(seed);
    std::shuffle(moved.begin(), moved.end(), rng);
    return moved;
}

void expectRegion(const geo::Mesh& mesh, geo::EditRegion& region, const std::vector<u32>& moved)
{
    region.collect(mesh, moved);
    const std::vector<u32> triangles(region.triangles().begin(), region.triangles().end());
    ASSERT_EQ(triangles, expectedTriangles(mesh, moved));

    const std::vector<u32> vertices(region.vertices().begin(), region.vertices().end());
    ASSERT_GE(vertices.size(), moved.size());
    ASSERT_TRUE(std::equal(moved.begin(), moved.end(), vertices.begin())) << "moved vertices come first, in order";
    std::vector<u32> sorted(vertices);
    std::sort(sorted.begin(), sorted.end());
    ASSERT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end()) << "each vertex once";
    ASSERT_EQ(sorted, expectedVertexSet(mesh, moved));
}

} // namespace

TEST(EditRegion, MatchesSeparateWalksForBothLayouts)
{
    for (const geo::AdjacencyLayout adjacency : {geo::AdjacencyLayout::Wide, geo::AdjacencyLayout::Compact}) {
        const geo::Mesh mesh = geo::makeIcosphere(6, 1.0f, geo::VertexOrder::Spatial,
                                                  {.vertexPageShift = 8, .trianglePageShift = 9, .adjacency = adjacency});
        geo::EditRegion region;
        // Reused scratch: each call must be independent of the one before.
        expectRegion(mesh, region, movedNear(mesh, {0, 0, 1}, 0.3f, 1u));
        expectRegion(mesh, region, movedNear(mesh, {0.6f, 0.6f, 0.5f}, 0.5f, 2u));
        expectRegion(mesh, region, {});
        EXPECT_TRUE(region.triangles().empty());
        EXPECT_TRUE(region.vertices().empty());
        expectRegion(mesh, region, {7u});
        expectRegion(mesh, region, movedNear(mesh, {0, 0, 1}, 0.3f, 3u));
    }
}

TEST(EditRegion, ScratchIsKeptBetweenCalls)
{
    const geo::Mesh mesh = geo::makeIcosphere(6);
    geo::EditRegion region;
    const std::vector<u32> moved = movedNear(mesh, {0, 0, 1}, 0.4f, 4u);
    region.collect(mesh, moved);
    const std::size_t bytes = region.memoryBytes();
    EXPECT_GE(bytes, (mesh.vertexCount() + mesh.triangleCount()) / 8);
    region.collect(mesh, movedNear(mesh, {1, 0, 0}, 0.2f, 5u));
    region.collect(mesh, moved);
    EXPECT_EQ(region.memoryBytes(), bytes) << "a smaller or equal region must not reallocate";
}

TEST(EditRegion, RefitFromTrianglesEqualsRefitFromVertices)
{
    core::JobSystem jobs(4);
    geo::Mesh mesh = geo::makeIcosphere(6);
    geo::ChunkedBvh fromVertices;
    fromVertices.build(mesh, 8);
    geo::ChunkedBvh fromTriangles = fromVertices;
    geo::ChunkedBvh parallel = fromVertices;
    geo::EditRegion region;

    const glm::vec3 centers[] = {{0, 0, 1}, {0.3f, 0.1f, 0.95f}, {-1, 0, 0}};
    const glm::vec3 offsets[] = {{0, 0, 0.5f}, {0.2f, -0.2f, 0.1f}, {-0.3f, 0.2f, 0}};
    for (int step = 0; step < 3; ++step) {
        const std::vector<u32> moved = movedNear(mesh, centers[step], 0.35f, 10u + step);
        ASSERT_FALSE(moved.empty());
        const auto positions = mesh.mutablePositions();
        for (const u32 vertex : moved) {
            positions[vertex] += offsets[step] * (1.0f - glm::distance(positions[vertex], centers[step]) / 0.35f);
        }
        fromVertices.refit(mesh, moved);
        region.collect(mesh, moved);
        fromTriangles.refitTriangles(mesh, region.triangles());
        parallel.refitTriangles(mesh, region.triangles(), &jobs);
        ASSERT_TRUE(fromTriangles.sameBounds(fromVertices)) << "after step " << step;
        ASSERT_TRUE(parallel.sameBounds(fromVertices)) << "after step " << step;

        geo::ChunkedBvh full = fromTriangles;
        full.refit(mesh);
        ASSERT_TRUE(fromTriangles.sameBounds(full)) << "after step " << step;
    }
}
