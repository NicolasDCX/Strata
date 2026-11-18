#include "Geometry/Icosphere.h"
#include "Geometry/SpatialOrder.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace se;

namespace {

using Triangle = std::array<std::array<float, 3>, 3>;

/// The triangles of a mesh as position triples, each rotated to start at its
/// smallest corner (keeps the winding) and the list sorted: a description of
/// the surface that does not depend on vertex or triangle numbering.
std::vector<Triangle> canonicalTriangles(const geo::Mesh& mesh)
{
    std::vector<Triangle> triangles;
    triangles.reserve(mesh.triangleCount());
    for (u32 t = 0; t < mesh.triangleCount(); ++t) {
        Triangle triangle;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const glm::vec3 p = mesh.positions()[mesh.indices()[t * 3 + corner]];
            triangle[corner] = {p.x, p.y, p.z};
        }
        std::rotate(triangle.begin(), std::min_element(triangle.begin(), triangle.end()), triangle.end());
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

/// Mean index distance between the two ends of an edge: small when vertices
/// that are neighbours on the surface are neighbours in memory.
double meanEdgeIndexDistance(const geo::Mesh& mesh)
{
    double total = 0.0;
    std::size_t edges = 0;
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        for (const u32 neighbor : mesh.neighbors(vertex)) {
            total += std::abs(static_cast<double>(vertex) - static_cast<double>(neighbor));
            ++edges;
        }
    }
    return total / static_cast<double>(edges);
}

} // namespace

TEST(SpatialOrder, KeepsTheSurfaceAndItsOrientation)
{
    const geo::Mesh generated = geo::makeIcosphere(4, 1.0f, geo::VertexOrder::Generation);
    const geo::Mesh sorted = geo::makeIcosphere(4, 1.0f, geo::VertexOrder::Spatial);

    ASSERT_EQ(sorted.vertexCount(), generated.vertexCount());
    ASSERT_EQ(sorted.triangleCount(), generated.triangleCount());
    std::string reason;
    EXPECT_TRUE(sorted.validate(&reason)) << reason;
    // Same triangles, same winding: only the numbering differs.
    EXPECT_EQ(canonicalTriangles(sorted), canonicalTriangles(generated));
    EXPECT_NE(std::vector<u32>(sorted.indices().begin(), sorted.indices().end()),
              std::vector<u32>(generated.indices().begin(), generated.indices().end()));
}

TEST(SpatialOrder, IsDeterministic)
{
    const geo::Mesh a = geo::makeIcosphere(3);
    const geo::Mesh b = geo::makeIcosphere(3);
    EXPECT_TRUE(std::equal(a.positions().begin(), a.positions().end(), b.positions().begin()));
    EXPECT_TRUE(std::equal(a.indices().begin(), a.indices().end(), b.indices().begin()));
}

TEST(SpatialOrder, BringsSurfaceNeighboursCloseInIndex)
{
    const geo::Mesh generated = geo::makeIcosphere(6, 1.0f, geo::VertexOrder::Generation);
    const geo::Mesh sorted = geo::makeIcosphere(6, 1.0f, geo::VertexOrder::Spatial);
    const double before = meanEdgeIndexDistance(generated);
    const double after = meanEdgeIndexDistance(sorted);
    EXPECT_LT(after, before / 4.0) << "mean edge index distance: generated " << before << ", sorted " << after;
}

TEST(SpatialOrder, HandlesDegenerateInput)
{
    std::vector<glm::vec3> noPositions;
    std::vector<u32> noIndices;
    geo::sortSpatially(noPositions, noIndices);
    EXPECT_TRUE(noPositions.empty());

    // All vertices at one point: zero extent must not divide by zero.
    std::vector<glm::vec3> same(4, glm::vec3(2.0f, 2.0f, 2.0f));
    std::vector<u32> indices{0, 1, 2, 1, 2, 3};
    geo::sortSpatially(same, indices);
    ASSERT_EQ(same.size(), 4u);
    for (const glm::vec3& p : same) {
        EXPECT_EQ(p, glm::vec3(2.0f));
    }
    for (const u32 index : indices) {
        EXPECT_LT(index, 4u);
    }
}
