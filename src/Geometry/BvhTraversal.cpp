#include "Geometry/Bvh.h"

#include "Geometry/Intersect.h"

#include <glm/geometric.hpp>

#include <array>
#include <utility>

namespace se::geo {

namespace {

// The builder produces a balanced tree, so depth <= log2(2^32) + 1. 64 is ample.
constexpr std::size_t kStackSize = 64;

} // namespace

std::optional<RayHit> Bvh::raycast(const Mesh& mesh, const Ray& ray, float maxDistance) const noexcept
{
    if (m_nodes.empty()) {
        return std::nullopt;
    }

    const PagedArrayView<const glm::vec3> positions = mesh.positions();
    const glm::vec3 invDirection = 1.0f / ray.direction;

    float closest = maxDistance;
    u32 closestTriangle = kInvalidIndex;

    std::array<u32, kStackSize> stack{};
    std::size_t stackSize = 0;

    float rootNear = 0.0f;
    if (intersectRayAabb(ray.origin, invDirection, m_nodes[0].bounds, closest, rootNear)) {
        stack[stackSize++] = 0;
    }

    while (stackSize > 0) {
        const Node& node = m_nodes[stack[--stackSize]];

        if (node.isLeaf()) {
            for (u32 k = 0; k < node.triangleCount; ++k) {
                const u32 triangle = m_triangles[node.leftOrFirst + k];
                float t = 0.0f;
                float u = 0.0f;
                float v = 0.0f;
                const Mesh::Triangle& corners = mesh.triangle(triangle);
                if (intersectRayTriangle(ray, positions[corners[0]], positions[corners[1]], positions[corners[2]], t, u,
                                         v) &&
                    t < closest) {
                    closest = t;
                    closestTriangle = triangle;
                }
            }
            continue;
        }

        // Visit the nearer child first so the farther one can often be culled.
        u32 nearChild = node.leftOrFirst;
        u32 farChild = node.leftOrFirst + 1;
        float nearDistance = 0.0f;
        float farDistance = 0.0f;
        bool hitNear = intersectRayAabb(ray.origin, invDirection, m_nodes[nearChild].bounds, closest, nearDistance);
        bool hitFar = intersectRayAabb(ray.origin, invDirection, m_nodes[farChild].bounds, closest, farDistance);
        if (hitNear && hitFar && farDistance < nearDistance) {
            std::swap(nearChild, farChild);
        }
        if (hitFar) {
            stack[stackSize++] = farChild;
        }
        if (hitNear) {
            stack[stackSize++] = nearChild;
        }
    }

    if (closestTriangle == kInvalidIndex) {
        return std::nullopt;
    }

    const Mesh::Triangle& hitCorners = mesh.triangle(closestTriangle);
    const glm::vec3& p0 = positions[hitCorners[0]];
    const glm::vec3& p1 = positions[hitCorners[1]];
    const glm::vec3& p2 = positions[hitCorners[2]];
    const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
    const float length = glm::length(faceNormal);

    RayHit hit;
    hit.distance = closest;
    hit.triangle = closestTriangle;
    hit.position = ray.at(closest);
    hit.normal = length > 0.0f ? faceNormal / length : -ray.direction;
    return hit;
}

void Bvh::querySphere(const glm::vec3& center, float radius, std::vector<u32>& outTriangles) const
{
    if (m_nodes.empty()) {
        return;
    }
    const float radiusSquared = radius * radius;

    std::array<u32, kStackSize> stack{};
    std::size_t stackSize = 0;
    stack[stackSize++] = 0;

    while (stackSize > 0) {
        const Node& node = m_nodes[stack[--stackSize]];
        if (node.bounds.distanceSquaredTo(center) > radiusSquared) {
            continue;
        }
        if (node.isLeaf()) {
            outTriangles.insert(outTriangles.end(), m_triangles.begin() + node.leftOrFirst,
                                m_triangles.begin() + node.leftOrFirst + node.triangleCount);
        } else {
            stack[stackSize++] = node.leftOrFirst;
            stack[stackSize++] = node.leftOrFirst + 1;
        }
    }
}

} // namespace se::geo
