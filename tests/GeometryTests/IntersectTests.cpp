#include "Geometry/Intersect.h"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <limits>

using namespace se;

namespace {

const glm::vec3 kA(0.0f, 0.0f, 0.0f);
const glm::vec3 kB(1.0f, 0.0f, 0.0f);
const glm::vec3 kC(0.0f, 1.0f, 0.0f);

struct TriangleHit {
    bool hit = false;
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

TriangleHit cast(const glm::vec3& origin, const glm::vec3& direction)
{
    TriangleHit result;
    result.hit = geo::intersectRayTriangle({origin, glm::normalize(direction)}, kA, kB, kC, result.t, result.u, result.v);
    return result;
}

} // namespace

TEST(RayTriangle, HitsInteriorWithCorrectDistanceAndBarycentrics)
{
    const TriangleHit result = cast({0.25f, 0.25f, 2.0f}, {0, 0, -1});
    ASSERT_TRUE(result.hit);
    EXPECT_NEAR(result.t, 2.0f, 1e-6f);
    EXPECT_NEAR(result.u, 0.25f, 1e-6f);
    EXPECT_NEAR(result.v, 0.25f, 1e-6f);
}

TEST(RayTriangle, IsDoubleSided)
{
    EXPECT_TRUE(cast({0.25f, 0.25f, -2.0f}, {0, 0, 1}).hit);
}

TEST(RayTriangle, MissesOutsideTheTriangle)
{
    EXPECT_FALSE(cast({0.8f, 0.8f, 2.0f}, {0, 0, -1}).hit);
    EXPECT_FALSE(cast({-0.1f, 0.5f, 2.0f}, {0, 0, -1}).hit);
    EXPECT_FALSE(cast({0.5f, -0.1f, 2.0f}, {0, 0, -1}).hit);
}

TEST(RayTriangle, IgnoresTrianglesBehindTheOrigin)
{
    EXPECT_FALSE(cast({0.25f, 0.25f, 2.0f}, {0, 0, 1}).hit);
}

TEST(RayTriangle, ParallelRayMisses)
{
    EXPECT_FALSE(cast({-1.0f, 0.25f, 0.0f}, {1, 0, 0}).hit);
    EXPECT_FALSE(cast({-1.0f, 0.25f, 0.5f}, {1, 0, 0}).hit);
}

TEST(RayTriangle, ObliqueHit)
{
    const glm::vec3 origin(2.0f, 2.0f, 2.0f);
    const glm::vec3 target(0.2f, 0.3f, 0.0f);
    const TriangleHit result = cast(origin, target - origin);
    ASSERT_TRUE(result.hit);
    EXPECT_NEAR(result.t, glm::distance(origin, target), 1e-5f);
}

TEST(RayTriangle, DegenerateTriangleMisses)
{
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    const geo::Ray ray{{0.5f, 0.0f, 1.0f}, {0, 0, -1}};
    EXPECT_FALSE(geo::intersectRayTriangle(ray, kA, kB, kB * 2.0f, t, u, v));
}

TEST(RayTriangle, TinyTriangleIsStillHit)
{
    // Triangles of dense sculpt meshes are far smaller than any fixed epsilon.
    const float s = 1e-4f;
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    const geo::Ray ray{{0.25f * s, 0.25f * s, 1.0f}, {0, 0, -1}};
    EXPECT_TRUE(geo::intersectRayTriangle(ray, kA * s, kB * s, kC * s, t, u, v));
    EXPECT_NEAR(t, 1.0f, 1e-6f);
}

TEST(RayAabb, HitMissAndInside)
{
    const geo::Aabb box{{-1, -1, -1}, {1, 1, 1}};
    const float inf = std::numeric_limits<float>::max();
    float tNear = 0.0f;

    const glm::vec3 towards(0, 0, -1);
    EXPECT_TRUE(geo::intersectRayAabb({0, 0, 5}, 1.0f / towards, box, inf, tNear));
    EXPECT_NEAR(tNear, 4.0f, 1e-6f);

    // Pointing away.
    const glm::vec3 away(0, 0, 1);
    EXPECT_FALSE(geo::intersectRayAabb({0, 0, 5}, 1.0f / away, box, inf, tNear));

    // Passing beside the box.
    EXPECT_FALSE(geo::intersectRayAabb({3, 0, 5}, 1.0f / towards, box, inf, tNear));

    // Starting inside: entry distance is 0.
    EXPECT_TRUE(geo::intersectRayAabb({0, 0, 0}, 1.0f / towards, box, inf, tNear));
    EXPECT_FLOAT_EQ(tNear, 0.0f);

    // Box beyond the allowed distance.
    EXPECT_FALSE(geo::intersectRayAabb({0, 0, 5}, 1.0f / towards, box, 3.0f, tNear));
}
