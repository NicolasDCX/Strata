#include "Geometry/Intersect.h"

#include <algorithm>
#include <cmath>

namespace se::geo {

bool intersectRayTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1,
                          const glm::vec3& v2, float& t, float& u, float& v) noexcept
{
    const glm::vec3 edge1 = v1 - v0;
    const glm::vec3 edge2 = v2 - v0;
    const glm::vec3 p = glm::cross(ray.direction, edge2);
    const float det = glm::dot(edge1, p);

    // Scale the parallel/degenerate threshold with the triangle size so that
    // very small triangles of dense meshes are not rejected.
    const float scale = glm::dot(edge1, edge1) * glm::dot(edge2, edge2);
    if (det * det <= 1e-14f * scale) {
        return false;
    }

    const float invDet = 1.0f / det;
    const glm::vec3 s = ray.origin - v0;
    const float bu = glm::dot(s, p) * invDet;
    if (bu < 0.0f || bu > 1.0f) {
        return false;
    }

    const glm::vec3 q = glm::cross(s, edge1);
    const float bv = glm::dot(ray.direction, q) * invDet;
    if (bv < 0.0f || bu + bv > 1.0f) {
        return false;
    }

    const float distance = glm::dot(edge2, q) * invDet;
    if (distance < 0.0f) {
        return false;
    }

    t = distance;
    u = bu;
    v = bv;
    return true;
}

bool intersectRayAabb(const glm::vec3& origin, const glm::vec3& invDirection, const Aabb& box,
                      float tMax, float& tNear) noexcept
{
    float tEnter = 0.0f;
    float tExit = tMax;
    for (int axis = 0; axis < 3; ++axis) {
        const float t0 = (box.min[axis] - origin[axis]) * invDirection[axis];
        const float t1 = (box.max[axis] - origin[axis]) * invDirection[axis];
        // std::min/max keep the first argument when the second is NaN (0 * inf),
        // which makes an axis-parallel ray lying on a slab boundary count as inside.
        tEnter = std::max(tEnter, std::min(t0, t1));
        tExit = std::min(tExit, std::max(t0, t1));
    }
    if (tEnter > tExit) {
        return false;
    }
    tNear = tEnter;
    return true;
}

} // namespace se::geo
