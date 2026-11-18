#pragma once

#include "Geometry/Aabb.h"
#include "Geometry/Ray.h"

namespace se::geo {

/// Moller-Trumbore ray/triangle test. Double sided. On success `t` receives the
/// distance along the ray and (u, v) the barycentric coordinates of v1 and v2.
[[nodiscard]] bool intersectRayTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1,
                                        const glm::vec3& v2, float& t, float& u, float& v) noexcept;

/// Slab test. `invDirection` is 1 / ray.direction (infinities are fine).
/// Returns true when the ray enters the box before `tMax`; `tNear` is the entry distance.
[[nodiscard]] bool intersectRayAabb(const glm::vec3& origin, const glm::vec3& invDirection,
                                    const Aabb& box, float tMax, float& tNear) noexcept;

} // namespace se::geo
