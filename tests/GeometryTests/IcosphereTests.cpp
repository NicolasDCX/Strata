#include "Geometry/Icosphere.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <utility>

using namespace se;

TEST(Icosphere, CountsMatchClosedForm)
{
    for (int level = 0; level <= 5; ++level) {
        const geo::Mesh mesh = geo::makeIcosphere(level);
        EXPECT_EQ(mesh.triangleCount(), geo::icosphereTriangleCount(level)) << "level " << level;
        EXPECT_EQ(mesh.vertexCount(), geo::icosphereVertexCount(level)) << "level " << level;
    }
}

TEST(Icosphere, IndicesAreValid)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    std::string reason;
    EXPECT_TRUE(mesh.validate(&reason)) << reason;
    EXPECT_EQ(mesh.indices().size() % 3, 0u);
    for (const u32 index : mesh.indices()) {
        ASSERT_LT(index, mesh.vertexCount());
    }
}

TEST(Icosphere, VerticesLieOnTheSphere)
{
    const float radius = 2.5f;
    const geo::Mesh mesh = geo::makeIcosphere(4, radius);
    for (const glm::vec3& position : mesh.positions()) {
        ASSERT_NEAR(glm::length(position), radius, 1e-4f);
    }
}

TEST(Icosphere, TrianglesFaceOutwardAndAreNotDegenerate)
{
    const geo::Mesh mesh = geo::makeIcosphere(3);
    const auto positions = mesh.positions();
    const auto indices = mesh.indices();
    for (u32 triangle = 0; triangle < mesh.triangleCount(); ++triangle) {
        const glm::vec3& a = positions[indices[triangle * 3 + 0]];
        const glm::vec3& b = positions[indices[triangle * 3 + 1]];
        const glm::vec3& c = positions[indices[triangle * 3 + 2]];
        const glm::vec3 faceNormal = glm::cross(b - a, c - a);
        ASSERT_GT(glm::length(faceNormal), 1e-6f) << "degenerate triangle " << triangle;
        ASSERT_GT(glm::dot(faceNormal, a + b + c), 0.0f) << "inward facing triangle " << triangle;
    }
}

TEST(Icosphere, NormalsAreRadial)
{
    const geo::Mesh mesh = geo::makeIcosphere(4);
    const auto positions = mesh.positions();
    const auto normals = mesh.normals();
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        ASSERT_NEAR(glm::length(normals[vertex]), 1.0f, 1e-4f);
        ASSERT_GT(glm::dot(normals[vertex], glm::normalize(positions[vertex])), 0.999f);
    }
}

TEST(Icosphere, IsAClosedManifold)
{
    // Every undirected edge must be shared by exactly two triangles, and the
    // Euler characteristic of a sphere is V - E + F = 2.
    const geo::Mesh mesh = geo::makeIcosphere(3);
    const auto indices = mesh.indices();
    std::multiset<std::pair<u32, u32>> edges;
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
            const u32 a = indices[i + k];
            const u32 b = indices[i + (k + 1) % 3];
            edges.emplace(std::min(a, b), std::max(a, b));
        }
    }
    std::set<std::pair<u32, u32>> uniqueEdges(edges.begin(), edges.end());
    for (const auto& edge : uniqueEdges) {
        ASSERT_EQ(edges.count(edge), 2u);
    }
    const long long euler = static_cast<long long>(mesh.vertexCount()) - static_cast<long long>(uniqueEdges.size()) +
                            static_cast<long long>(mesh.triangleCount());
    EXPECT_EQ(euler, 2);
}

TEST(Icosphere, SubdivisionLevelIsClamped)
{
    EXPECT_EQ(geo::makeIcosphere(-3).triangleCount(), 20u);
}
