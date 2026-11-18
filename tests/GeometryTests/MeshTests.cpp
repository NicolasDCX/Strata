#include "Geometry/Icosphere.h"
#include "Geometry/Mesh.h"
#include "Geometry/DirtyVertexSet.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace se;

namespace {

/// Two triangles sharing the edge 1-2:   0 --- 1
///                                       |  /  |
///                                       2 --- 3
geo::Mesh makeQuad()
{
    return geo::Mesh({{0, 1, 0}, {1, 1, 0}, {0, 0, 0}, {1, 0, 0}}, {0, 2, 1, 1, 2, 3});
}

} // namespace

TEST(Mesh, BoundsEncloseAllVertices)
{
    const geo::Mesh mesh = geo::makeIcosphere(3, 1.5f);
    const geo::Aabb& bounds = mesh.bounds();
    ASSERT_TRUE(bounds.valid());
    for (const glm::vec3& position : mesh.positions()) {
        ASSERT_TRUE(bounds.contains(position));
    }
    // An icosphere contains the axis-aligned extremes only approximately.
    for (int axis = 0; axis < 3; ++axis) {
        EXPECT_NEAR(bounds.max[axis], 1.5f, 0.05f);
        EXPECT_NEAR(bounds.min[axis], -1.5f, 0.05f);
    }
}

TEST(Mesh, BoundsFollowEditedPositions)
{
    geo::Mesh mesh = makeQuad();
    mesh.mutablePositions()[3] = {5.0f, -2.0f, 3.0f};
    mesh.recomputeBounds();
    EXPECT_FLOAT_EQ(mesh.bounds().max.x, 5.0f);
    EXPECT_FLOAT_EQ(mesh.bounds().min.y, -2.0f);
    EXPECT_FLOAT_EQ(mesh.bounds().max.z, 3.0f);
}

TEST(Mesh, EmptyMeshIsValidAndHasEmptyBounds)
{
    const geo::Mesh mesh;
    EXPECT_EQ(mesh.vertexCount(), 0u);
    EXPECT_EQ(mesh.triangleCount(), 0u);
    EXPECT_FALSE(mesh.bounds().valid());
}

TEST(Mesh, AdjacencyOfQuad)
{
    const geo::Mesh mesh = makeQuad();
    const auto asVector = [](const auto& s) { return std::vector<u32>(s.begin(), s.end()); };
    EXPECT_EQ(asVector(mesh.neighbors(0)), (std::vector<u32>{1, 2}));
    EXPECT_EQ(asVector(mesh.neighbors(1)), (std::vector<u32>{0, 2, 3}));
    EXPECT_EQ(asVector(mesh.neighbors(2)), (std::vector<u32>{0, 1, 3}));
    EXPECT_EQ(asVector(mesh.neighbors(3)), (std::vector<u32>{1, 2}));

    EXPECT_EQ(mesh.incidentTriangles(0).size(), 1u);
    EXPECT_EQ(mesh.incidentTriangles(1).size(), 2u);
    EXPECT_EQ(mesh.incidentTriangles(2).size(), 2u);
    EXPECT_EQ(mesh.incidentTriangles(3).size(), 1u);
}

TEST(Mesh, AdjacencyIsSymmetricAndMatchesIcosphereValence)
{
    const geo::Mesh mesh = geo::makeIcosphere(3);
    u32 valenceFive = 0;
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        const auto ring = mesh.neighbors(vertex);
        // The 12 original icosahedron corners have 5 neighbours, all others 6.
        ASSERT_TRUE(ring.size() == 5 || ring.size() == 6);
        valenceFive += ring.size() == 5 ? 1u : 0u;
        ASSERT_EQ(ring.size(), mesh.incidentTriangles(vertex).size());
        ASSERT_TRUE(std::is_sorted(ring.begin(), ring.end()));
        for (const u32 neighbor : ring) {
            ASSERT_NE(neighbor, vertex);
            const auto back = mesh.neighbors(neighbor);
            ASSERT_TRUE(std::binary_search(back.begin(), back.end(), vertex));
        }
    }
    EXPECT_EQ(valenceFive, 12u);
}

TEST(Mesh, IncidentTrianglesReferenceTheVertex)
{
    const geo::Mesh mesh = geo::makeIcosphere(2);
    const auto indices = mesh.indices();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        for (const u32 triangle : mesh.incidentTriangles(vertex)) {
            const std::array<u32, 3> corners = {indices[triangle * 3], indices[triangle * 3 + 1], indices[triangle * 3 + 2]};
            ASSERT_TRUE(std::find(corners.begin(), corners.end(), vertex) != corners.end());
        }
    }
}

TEST(Mesh, PartialNormalUpdateMatchesFullRecompute)
{
    geo::Mesh mesh = geo::makeIcosphere(3);
    const u32 moved = 17;
    mesh.mutablePositions()[moved] *= 1.4f;

    // Partial: the moved vertex and its one-ring.
    std::vector<u32> affected{moved};
    const auto ring = mesh.neighbors(moved);
    affected.insert(affected.end(), ring.begin(), ring.end());
    mesh.recomputeNormals(affected);
    const std::vector<glm::vec3> partial(mesh.normals().begin(), mesh.normals().end());

    mesh.recomputeNormals();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        ASSERT_NEAR(glm::distance(partial[vertex], mesh.normals()[vertex]), 0.0f, 1e-5f) << "vertex " << vertex;
    }
}

TEST(Mesh, FlatQuadNormalsPointAlongZ)
{
    const geo::Mesh mesh = makeQuad();
    for (const glm::vec3& normal : mesh.normals()) {
        EXPECT_NEAR(normal.z, 1.0f, 1e-6f);
    }
}

TEST(DirtyVertexSet, MarksWholePagesAndReportsRuns)
{
    const u32 page = geo::DirtyVertexSet::kPageSize;
    geo::DirtyVertexSet set(page * 5 + 100); // 6 pages, the last one partial
    EXPECT_TRUE(set.empty());
    EXPECT_EQ(set.pageCount(), 6u);
    EXPECT_EQ(set.dirtyVertexCount(), 0u);

    set.mark(7);
    set.mark(9); // same page: counted once
    EXPECT_EQ(set.dirtyPageCount(), 1u);
    EXPECT_TRUE(set.contains(0));
    EXPECT_TRUE(set.contains(page - 1));
    EXPECT_FALSE(set.contains(page));

    set.mark(page * 2 + 5);
    set.mark(page * 3);     // adjacent to the previous page: one run
    set.mark(page * 5 + 99); // last, partial page

    std::vector<std::pair<u32, u32>> runs;
    set.forEachRun([&runs](u32 first, u32 count) { runs.emplace_back(first, count); });
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0], (std::pair<u32, u32>{0u, page}));
    EXPECT_EQ(runs[1], (std::pair<u32, u32>{page * 2, page * 2}));
    EXPECT_EQ(runs[2], (std::pair<u32, u32>{page * 5, 100u})) << "runs are clipped to the vertex count";
    EXPECT_EQ(set.dirtyVertexCount(), page * 3 + 100);

    set.clear();
    EXPECT_TRUE(set.empty());
    EXPECT_FALSE(set.contains(7));
}

TEST(DirtyVertexSet, MergeAndMarkAll)
{
    const u32 page = geo::DirtyVertexSet::kPageSize;
    geo::DirtyVertexSet a(page * 200); // spans several 64-page words
    geo::DirtyVertexSet b(page * 200);
    a.mark(0);
    a.mark(page * 70);
    b.mark(page * 70); // overlap must not be double counted
    b.mark(page * 199);
    a.merge(b);
    EXPECT_EQ(a.dirtyPageCount(), 3u);
    EXPECT_TRUE(a.contains(page * 199));

    geo::DirtyVertexSet unsized;
    unsized.merge(geo::DirtyVertexSet(page)); // merging an empty set leaves it unsized
    EXPECT_EQ(unsized.vertexCount(), 0u);
    unsized.merge(a); // adopts the size of the first non-empty set
    EXPECT_EQ(unsized.vertexCount(), page * 200);
    EXPECT_EQ(unsized.dirtyPageCount(), 3u);

    geo::DirtyVertexSet all(page * 3 + 1);
    all.markAll();
    EXPECT_EQ(all.dirtyPageCount(), 4u);
    EXPECT_EQ(all.dirtyVertexCount(), page * 3 + 1);
    u32 runCount = 0;
    all.forEachRun([&runCount](u32, u32) { ++runCount; });
    EXPECT_EQ(runCount, 1u);

    geo::DirtyVertexSet none(0);
    none.markAll();
    EXPECT_TRUE(none.empty());
}