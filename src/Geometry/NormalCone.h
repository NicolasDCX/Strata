#pragma once

#include "Core/Timer.h"
#include "Core/Types.h"
#include "Geometry/Aabb.h"
#include "Geometry/Mesh.h"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>

namespace se::geo {

/// Cone that contains the geometric (winding) normal of every triangle of a
/// group of triangles: every normal n satisfies dot(n, axis) >= cosAngle * |n|.
///
/// Used for back-face culling of whole chunks: if every possible normal in the
/// cone faces away from the eye at every point of the chunk's bounds, the
/// rasterizer would discard every triangle of the chunk, so the chunk can be
/// skipped without changing a pixel (see coneFacesAway).
struct NormalCone {
    glm::vec3 axis{0.0f};
    float cosAngle = -1.0f; ///< cos of the half-angle; not usable() means "cannot reject anything"
    float sinAngle = 1.0f;

    /// Cones wider than about 84 degrees can never reject a chunk and are not kept.
    static constexpr float kMinCos = 0.1f;
    [[nodiscard]] bool usable() const noexcept { return cosAngle > kMinCos; }

    friend bool operator==(const NormalCone&, const NormalCone&) = default;
};

/// Normal cone of triangles [first, first + count). Deterministic (a sequential
/// loop over the triangles in order), so serial and parallel callers agree bit
/// for bit. Degenerate triangles (zero area) produce no fragments and are
/// ignored. Returns an unusable cone when the normals spread too widely.
[[nodiscard]] inline NormalCone computeNormalCone(const Mesh& mesh, u32 first, u32 count)
{
    const auto positions = mesh.positions();
    const auto faceNormal = [&](u32 triangle) {
        const auto& t = mesh.triangle(triangle);
        return glm::cross(positions[t[1]] - positions[t[0]], positions[t[2]] - positions[t[0]]);
    };

    // Axis: mean of the unit face normals.
    glm::vec3 sum(0.0f);
    for (u32 t = first; t < first + count; ++t) {
        const glm::vec3 n = faceNormal(t);
        const float length = glm::length(n);
        if (length > 0.0f) {
            sum += n / length;
        }
    }
    NormalCone cone;
    const float sumLength = glm::length(sum);
    if (!(sumLength > 1e-6f)) {
        return cone;
    }
    cone.axis = sum / sumLength;

    // Half-angle: the widest normal from the axis. Stops early once the cone is
    // too wide to be of use.
    float minCos = 1.0f;
    for (u32 t = first; t < first + count && minCos > NormalCone::kMinCos; ++t) {
        const glm::vec3 n = faceNormal(t);
        const float length = glm::length(n);
        if (length > 0.0f) {
            minCos = std::min(minCos, glm::dot(cone.axis, n) / length);
        }
    }
    // Widen slightly to cover rounding in the normal computations.
    minCos -= 1e-4f;
    if (!(minCos > NormalCone::kMinCos)) {
        return NormalCone{};
    }
    cone.cosAngle = minCos;
    cone.sinAngle = std::sqrt(std::max(0.0f, 1.0f - minCos * minCos));
    return cone;
}

/// Widens `cone`, keeping its axis, so that it also holds the face normals of
/// the triangles that `forEachTriangle(visit)` passes to `visit(triangleId)`.
/// Each normal is tested exactly as computeNormalCone() tests it, with the same
/// rounding margin, so the result holds those normals and every normal the
/// input cone held. Returns an unusable cone (it rejects nothing) when the
/// input is unusable or the result would be too wide to be of use.
template <class ForEachTriangle>
[[nodiscard]] NormalCone widenNormalCone(const NormalCone& cone, const Mesh& mesh, ForEachTriangle&& forEachTriangle)
{
    if (!cone.usable()) {
        return NormalCone{};
    }
    const auto positions = mesh.positions();
    const auto triangles = mesh.triangles();
    float minCos = 1.0f;
    forEachTriangle([&](u32 triangle) {
        const auto& t = triangles[triangle];
        const glm::vec3 n = glm::cross(positions[t[1]] - positions[t[0]], positions[t[2]] - positions[t[0]]);
        const float length = glm::length(n);
        if (length > 0.0f) {
            minCos = std::min(minCos, glm::dot(cone.axis, n) / length);
        }
    });
    minCos -= 1e-4f;
    if (!(minCos < cone.cosAngle)) {
        return cone; // every normal passed is already inside
    }
    if (!(minCos > NormalCone::kMinCos)) {
        return NormalCone{};
    }
    NormalCone widened = cone;
    widened.cosAngle = minCos;
    widened.sinAngle = std::sqrt(std::max(0.0f, 1.0f - minCos * minCos));
    return widened;
}

/// Where computeNormalConeTimed() spent its time (diagnostics).
struct NormalConeTimes {
    double firstPassMs = 0.0;  ///< face normals and the axis
    double secondPassMs = 0.0; ///< the half-angle (stops early once the cone is too wide)
    u32 secondPassTriangles = 0;
};

/// computeNormalCone() with its two passes timed into `times` and, when
/// `faces` is given (at least `count` entries), the face normals reused: the
/// first pass stores each face normal and its length there, and the second
/// reads them back instead of reading the triangle and its three positions
/// again and recomputing the cross product. The values, the order of the
/// accumulation and the early stop are those of computeNormalCone(), so the
/// cone is the same bit for bit.
[[nodiscard]] inline NormalCone computeNormalConeTimed(const Mesh& mesh, u32 first, u32 count, glm::vec4* faces,
                                                       NormalConeTimes& times)
{
    const auto positions = mesh.positions();
    const auto faceNormal = [&](u32 triangle) {
        const auto& t = mesh.triangle(triangle);
        return glm::cross(positions[t[1]] - positions[t[0]], positions[t[2]] - positions[t[0]]);
    };

    core::Stopwatch timer;
    glm::vec3 sum(0.0f);
    if (faces != nullptr) {
        for (u32 t = first; t < first + count; ++t) {
            const glm::vec3 n = faceNormal(t);
            const float length = glm::length(n);
            faces[t - first] = glm::vec4(n, length);
            if (length > 0.0f) {
                sum += n / length;
            }
        }
    } else {
        for (u32 t = first; t < first + count; ++t) {
            const glm::vec3 n = faceNormal(t);
            const float length = glm::length(n);
            if (length > 0.0f) {
                sum += n / length;
            }
        }
    }
    NormalCone cone;
    const float sumLength = glm::length(sum);
    times.firstPassMs += timer.elapsedMs();
    if (!(sumLength > 1e-6f)) {
        return cone;
    }
    cone.axis = sum / sumLength;

    timer.reset();
    float minCos = 1.0f;
    u32 visited = 0;
    if (faces != nullptr) {
        for (u32 t = first; t < first + count && minCos > NormalCone::kMinCos; ++t) {
            const glm::vec4 face = faces[t - first];
            const glm::vec3 n(face);
            const float length = face.w;
            if (length > 0.0f) {
                minCos = std::min(minCos, glm::dot(cone.axis, n) / length);
            }
            ++visited;
        }
    } else {
        for (u32 t = first; t < first + count && minCos > NormalCone::kMinCos; ++t) {
            const glm::vec3 n = faceNormal(t);
            const float length = glm::length(n);
            if (length > 0.0f) {
                minCos = std::min(minCos, glm::dot(cone.axis, n) / length);
            }
            ++visited;
        }
    }
    times.secondPassMs += timer.elapsedMs();
    times.secondPassTriangles += visited;
    minCos -= 1e-4f;
    if (!(minCos > NormalCone::kMinCos)) {
        return NormalCone{};
    }
    cone.cosAngle = minCos;
    cone.sinAngle = std::sqrt(std::max(0.0f, 1.0f - minCos * minCos));
    return cone;
}

/// Conservative back-face test for a whole chunk. True only when, for every
/// normal n in `cone` and every point p in `bounds`, dot(n, p - eye) is
/// positive by a margin, i.e. every triangle faces away from the eye and back-
/// face culling (counter-clockwise front faces) would discard it. False
/// whenever the cone is unusable or the bounds are empty: the chunk is drawn.
[[nodiscard]] inline bool coneFacesAway(const NormalCone& cone, const Aabb& bounds, const glm::vec3& eye) noexcept
{
    if (!cone.usable() || !bounds.valid()) {
        return false;
    }
    const glm::vec3 center = bounds.center();
    const float radius = 0.5f * glm::length(bounds.max - bounds.min);
    const glm::vec3 toCenter = center - eye;
    const float distanceSq = glm::dot(toCenter, toCenter);
    // min over n in the cone of dot(n, toCenter) = |toCenter| cos(theta + alpha),
    // with cos(theta) = dot(axis, toCenter) / |toCenter|; expanded to avoid acos.
    // The worst point of the bounding sphere then subtracts the radius (|n| = 1).
    // Margin: 1e-3 of the distance, far above float rounding in the GPU's
    // transform, so a chunk with any triangle near edge-on is always drawn.
    const float along = glm::dot(cone.axis, toCenter);
    const float across = std::sqrt(std::max(0.0f, distanceSq - along * along));
    const float minDot = along * cone.cosAngle - across * cone.sinAngle;
    return minDot > radius + 1e-3f * std::sqrt(distanceSq);
}

/// Test support: true when every non-degenerate face normal of triangles
/// [first, first + count) lies inside `cone` (always true for an unusable cone).
[[nodiscard]] inline bool coneContainsNormals(const NormalCone& cone, const Mesh& mesh, u32 first, u32 count)
{
    if (!cone.usable()) {
        return true;
    }
    const auto positions = mesh.positions();
    for (u32 t = first; t < first + count; ++t) {
        const auto& tri = mesh.triangle(t);
        const glm::vec3 n = glm::cross(positions[tri[1]] - positions[tri[0]], positions[tri[2]] - positions[tri[0]]);
        const float length = glm::length(n);
        if (length > 0.0f && glm::dot(cone.axis, n) / length < cone.cosAngle) {
            return false;
        }
    }
    return true;
}

} // namespace se::geo
