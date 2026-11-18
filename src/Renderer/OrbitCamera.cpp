#include "Renderer/OrbitCamera.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace se::render {

namespace {

constexpr glm::vec3 kWorldUp(0.0f, 1.0f, 0.0f);
constexpr float kOrbitRadiansPerPixel = 0.008f;
constexpr float kMaxPitch = 1.5533f; // 89 degrees: keeps the view matrix well defined
constexpr float kZoomFactorPerStep = 0.88f;
constexpr float kMinDistance = 0.05f;
constexpr float kMaxDistance = 500.0f;

} // namespace

void OrbitCamera::orbit(float deltaX, float deltaY) noexcept
{
    // Dragging right spins the scene to the right, i.e. the camera to the left.
    m_yaw -= deltaX * kOrbitRadiansPerPixel;
    m_pitch = std::clamp(m_pitch + deltaY * kOrbitRadiansPerPixel, -kMaxPitch, kMaxPitch);
}

void OrbitCamera::pan(float deltaX, float deltaY, float viewportHeight) noexcept
{
    if (viewportHeight <= 0.0f) {
        return;
    }
    const glm::vec3 forward = glm::normalize(m_target - eye());
    const glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));
    const glm::vec3 up = glm::cross(right, forward);
    // World units covered by one pixel at the target's depth.
    const float worldPerPixel = 2.0f * m_distance * std::tan(m_fovY * 0.5f) / viewportHeight;
    m_target += (-right * deltaX + up * deltaY) * worldPerPixel;
}

void OrbitCamera::zoom(float steps) noexcept
{
    m_distance = std::clamp(m_distance * std::pow(kZoomFactorPerStep, steps), kMinDistance, kMaxDistance);
}

void OrbitCamera::frame(const geo::Aabb& bounds) noexcept
{
    if (!bounds.valid()) {
        return;
    }
    m_target = bounds.center();
    // Half of the largest side: tight for the rounded shapes sculpting produces.
    const glm::vec3 extent = bounds.extent();
    const float radius = std::max(std::max(extent.x, std::max(extent.y, extent.z)) * 0.5f, 1e-3f);
    m_distance = std::clamp(radius / std::sin(m_fovY * 0.5f) * 1.25f, kMinDistance, kMaxDistance);
}

glm::vec3 OrbitCamera::eye() const noexcept
{
    const float cosPitch = std::cos(m_pitch);
    const glm::vec3 offset(cosPitch * std::sin(m_yaw), std::sin(m_pitch), cosPitch * std::cos(m_yaw));
    return m_target + offset * m_distance;
}

glm::mat4 OrbitCamera::view() const noexcept
{
    return glm::lookAt(eye(), m_target, kWorldUp);
}

glm::mat4 OrbitCamera::projection(float aspect) const noexcept
{
    // Near/far follow the orbit distance so depth precision stays usable both
    // when zoomed in on a detail and when looking at the whole model.
    const float nearPlane = std::max(m_distance * 0.02f, 1e-3f);
    const float farPlane = m_distance * 20.0f + 100.0f;
    glm::mat4 projection = glm::perspective(m_fovY, std::max(aspect, 1e-3f), nearPlane, farPlane);
    projection[1][1] *= -1.0f; // Vulkan's clip-space Y points down
    return projection;
}

geo::Ray OrbitCamera::screenRay(float x, float y, float viewportWidth, float viewportHeight) const noexcept
{
    const glm::vec3 origin = eye();
    const glm::vec3 forward = glm::normalize(m_target - origin);
    const glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));
    const glm::vec3 up = glm::cross(right, forward);

    const float width = std::max(viewportWidth, 1.0f);
    const float height = std::max(viewportHeight, 1.0f);
    const float ndcX = 2.0f * x / width - 1.0f;
    const float ndcY = 1.0f - 2.0f * y / height;
    const float tanHalfFov = std::tan(m_fovY * 0.5f);

    geo::Ray ray;
    ray.origin = origin;
    ray.direction = glm::normalize(forward + right * (ndcX * tanHalfFov * width / height) + up * (ndcY * tanHalfFov));
    return ray;
}

} // namespace se::render
