#pragma once

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <limits>

namespace se::geo {

/// Axis-aligned bounding box. Default constructed boxes are empty (inverted).
struct Aabb {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    void expand(const glm::vec3& point) noexcept
    {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void expand(const Aabb& other) noexcept
    {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    [[nodiscard]] bool operator==(const Aabb& other) const noexcept { return min == other.min && max == other.max; }

    [[nodiscard]] bool valid() const noexcept
    {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    [[nodiscard]] glm::vec3 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 extent() const noexcept { return max - min; }

    [[nodiscard]] bool contains(const glm::vec3& point) const noexcept
    {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    /// Squared distance from a point to the box (0 when the point is inside).
    [[nodiscard]] float distanceSquaredTo(const glm::vec3& point) const noexcept
    {
        const glm::vec3 clamped = glm::clamp(point, min, max);
        const glm::vec3 d = point - clamped;
        return glm::dot(d, d);
    }
};

} // namespace se::geo
