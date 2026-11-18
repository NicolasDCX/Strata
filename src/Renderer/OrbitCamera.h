#pragma once

#include "Geometry/Aabb.h"
#include "Geometry/Ray.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace se::render {

/// Camera orbiting a target point. Pure math: no dependency on Vulkan or Qt.
///
/// Conventions: right-handed world, +Y up. projection() targets Vulkan clip
/// space (depth 0..1, Y pointing down), so counter-clockwise world triangles
/// stay counter-clockwise for the rasterizer.
class OrbitCamera {
public:
    /// Rotates around the target. Arguments are pointer deltas in pixels.
    void orbit(float deltaX, float deltaY) noexcept;

    /// Moves the target in the view plane so the scene follows the pointer.
    void pan(float deltaX, float deltaY, float viewportHeight) noexcept;

    /// Positive steps move towards the target (one step = one wheel notch).
    void zoom(float steps) noexcept;

    /// Centres the target on the box and backs off until it fits the view.
    void frame(const geo::Aabb& bounds) noexcept;

    [[nodiscard]] glm::vec3 eye() const noexcept;
    [[nodiscard]] glm::vec3 target() const noexcept { return m_target; }
    [[nodiscard]] float distance() const noexcept { return m_distance; }

    [[nodiscard]] glm::mat4 view() const noexcept;
    [[nodiscard]] glm::mat4 projection(float aspect) const noexcept;

    /// World-space ray through a viewport position (origin top-left, Y down),
    /// consistent with view() and projection().
    [[nodiscard]] geo::Ray screenRay(float x, float y, float viewportWidth, float viewportHeight) const noexcept;

private:
    glm::vec3 m_target{0.0f};
    float m_distance = 3.6f;
    float m_yaw = 0.55f;    // radians around +Y
    float m_pitch = 0.30f;  // radians above the horizon
    float m_fovY = 0.7854f; // 45 degrees
};

} // namespace se::render
