#pragma once

#include "Geometry/Aabb.h"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace se::render {

/// View frustum as six inward-facing planes, extracted from a view-projection
/// matrix that maps to Vulkan clip space (depth 0..1).
struct Frustum {
    /// A point p is inside when dot(plane.xyz, p) + plane.w >= 0 for all planes.
    std::array<glm::vec4, 6> planes{};

    [[nodiscard]] static Frustum fromViewProjection(const glm::mat4& m) noexcept
    {
        // GLM matrices are column-major: row i is (m[0][i], m[1][i], m[2][i], m[3][i]).
        const auto row = [&m](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
        const glm::vec4 x = row(0);
        const glm::vec4 y = row(1);
        const glm::vec4 z = row(2);
        const glm::vec4 w = row(3);
        Frustum frustum;
        frustum.planes = {
            w + x, // left:   -w <= x
            w - x, // right:   x <= w
            w + y, // one of the two vertical sides (which one depends on the Y flip)
            w - y, // the other
            z,     // near:    0 <= z
            w - z, // far:     z <= w
        };
        return frustum;
    }

    /// Conservative box test: false only when the box is entirely outside one
    /// plane. A box near a frustum corner can be reported visible although it is
    /// not; that costs a little drawing, never a missing object.
    [[nodiscard]] bool intersects(const geo::Aabb& box) const noexcept
    {
        if (!box.valid()) {
            return false;
        }
        for (const glm::vec4& plane : planes) {
            // The box corner furthest along the plane normal.
            const glm::vec3 corner(plane.x >= 0.0f ? box.max.x : box.min.x, plane.y >= 0.0f ? box.max.y : box.min.y,
                                   plane.z >= 0.0f ? box.max.z : box.min.z);
            if (plane.x * corner.x + plane.y * corner.y + plane.z * corner.z + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

} // namespace se::render
