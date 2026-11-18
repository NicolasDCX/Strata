#pragma once

#include "Geometry/Intersect.h"
#include "Geometry/Mesh.h"
#include "Geometry/Ray.h"

#include <glm/geometric.hpp>

#include <cstring>
#include <optional>
#include <random>

namespace se::test {

/// Reference raycast that tests every triangle. Used to validate the BVH.
inline std::optional<float> bruteForceRaycast(const geo::Mesh& mesh, const geo::Ray& ray)
{
    const auto positions = mesh.positions();
    const auto indices = mesh.indices();
    std::optional<float> closest;
    for (u32 triangle = 0; triangle < mesh.triangleCount(); ++triangle) {
        float t = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        if (geo::intersectRayTriangle(ray, positions[indices[triangle * 3 + 0]],
                                      positions[indices[triangle * 3 + 1]],
                                      positions[indices[triangle * 3 + 2]], t, u, v)) {
            if (!closest || t < *closest) {
                closest = t;
            }
        }
    }
    return closest;
}

/// True when two sequences have the same length and bit-identical elements.
/// Works for vectors, spans and paged views alike.
template <typename A, typename B>
bool bitwiseEqual(const A& a, const B& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    auto other = b.begin();
    for (const auto& element : a) {
        if (std::memcmp(&element, &*other, sizeof(element)) != 0) {
            return false;
        }
        ++other;
    }
    return true;
}

inline glm::vec3 randomUnitVector(std::mt19937& rng)
{
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    glm::vec3 v(0.0f);
    do {
        v = glm::vec3(gauss(rng), gauss(rng), gauss(rng));
    } while (glm::dot(v, v) < 1e-6f);
    return glm::normalize(v);
}

/// Ray starting outside the unit-ish sphere, aimed at a random point near the origin.
inline geo::Ray randomRayTowardsOrigin(std::mt19937& rng, float startDistance = 4.0f, float targetSpread = 1.3f)
{
    std::uniform_real_distribution<float> spread(-targetSpread, targetSpread);
    geo::Ray ray;
    ray.origin = randomUnitVector(rng) * startDistance;
    const glm::vec3 target(spread(rng), spread(rng), spread(rng));
    ray.direction = glm::normalize(target - ray.origin);
    return ray;
}

} // namespace se::test
