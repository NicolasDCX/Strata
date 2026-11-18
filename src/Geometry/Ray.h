#pragma once

#include "Core/Types.h"

#include <glm/vec3.hpp>

namespace se::geo {

/// Half line. `direction` is expected to be normalised so that `t` is a distance.
struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};

    [[nodiscard]] glm::vec3 at(float t) const noexcept { return origin + direction * t; }
};

/// Closest intersection of a ray with a mesh.
struct RayHit {
    float distance = 0.0f;       ///< Distance along the ray.
    u32 triangle = kInvalidIndex; ///< Index of the triangle that was hit.
    glm::vec3 position{0.0f};    ///< World-space hit position.
    glm::vec3 normal{0.0f};      ///< Geometric normal of the hit triangle (unit length).
};

} // namespace se::geo
