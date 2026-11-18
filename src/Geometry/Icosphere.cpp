#include "Geometry/Icosphere.h"

#include "Geometry/SpatialOrder.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace se::geo {

namespace {

constexpr std::array<u32, 60> kIcosahedronIndices = {
    0, 11, 5,  0, 5,  1, 0, 1, 7, 0, 7,  10, 0, 10, 11, 1, 5, 9, 5, 11, 4,  11, 10, 2, 10, 7, 6, 7, 1, 8,
    3, 9,  4,  3, 4,  2, 3, 2, 6, 3, 6,  8,  3, 8,  9,  4, 9, 5, 2, 4,  11, 6,  2,  10, 8, 6, 7, 9, 8, 1,
};

} // namespace

Mesh makeIcosphere(int subdivisions, float radius, VertexOrder order, const MeshLayout& layout)
{
    subdivisions = std::clamp(subdivisions, 0, kMaxIcosphereSubdivisions);

    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<glm::vec3> positions = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
        {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1},
    };
    for (glm::vec3& position : positions) {
        position = glm::normalize(position);
    }
    positions.reserve(static_cast<std::size_t>(icosphereVertexCount(subdivisions)));

    std::vector<u32> indices(kIcosahedronIndices.begin(), kIcosahedronIndices.end());

    // Every edge is shared by two triangles; the cache guarantees both get the
    // same midpoint vertex so the result stays watertight.
    std::unordered_map<u64, u32> midpointCache;
    const auto midpoint = [&positions, &midpointCache](u32 a, u32 b) {
        const u64 key = (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
        if (const auto found = midpointCache.find(key); found != midpointCache.end()) {
            return found->second;
        }
        const u32 index = static_cast<u32>(positions.size());
        positions.push_back(glm::normalize(positions[a] + positions[b]));
        midpointCache.emplace(key, index);
        return index;
    };

    for (int level = 0; level < subdivisions; ++level) {
        std::vector<u32> next;
        next.reserve(indices.size() * 4);
        midpointCache.clear();
        midpointCache.reserve(indices.size() / 2);
        for (std::size_t i = 0; i < indices.size(); i += 3) {
            const u32 a = indices[i];
            const u32 b = indices[i + 1];
            const u32 c = indices[i + 2];
            const u32 ab = midpoint(a, b);
            const u32 bc = midpoint(b, c);
            const u32 ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        indices = std::move(next);
    }

    for (glm::vec3& position : positions) {
        position *= radius;
    }
    if (order == VertexOrder::Spatial) {
        sortSpatially(positions, indices);
    }
    return Mesh(std::move(positions), std::move(indices), layout);
}

} // namespace se::geo
