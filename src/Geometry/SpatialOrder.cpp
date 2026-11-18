#include "Geometry/SpatialOrder.h"

#include "Geometry/Aabb.h"

#include <algorithm>
#include <bit>
#include <numeric>
#include <utility>

namespace se::geo {

namespace {

/// Below this many elements the order inside a block no longer matters: it is
/// smaller than any page or chunk the engine uses.
constexpr u32 kSmallestBlock = 32;

/// Orders `ids[first, first + count)` so that every aligned power-of-two block
/// of the range is spatially compact.
///
/// The range is split along the widest axis of its bounding box, and both
/// halves are ordered recursively (a k-d tree written out in leaf order). The
/// left half always receives a power-of-two number of elements. That makes
/// every block [k * 2^s, (k + 1) * 2^s) of the final order exactly one subtree,
/// i.e. the content of one box, for every s at once. So vertex pages and
/// spatial chunks of any power-of-two size are compact, whatever sizes are
/// chosen later.
///
/// (Space-filling curves were tried first. On a thin surface a Morton or
/// Hilbert curve leaves the surface and comes back somewhere else, so a run of
/// consecutive indices could hold two far-apart patches.)
void orderRange(std::vector<u32>& ids, const std::vector<glm::vec3>& points, u32 first, u32 count)
{
    while (count > kSmallestBlock) {
        Aabb bounds;
        for (u32 i = first; i < first + count; ++i) {
            bounds.expand(points[ids[i]]);
        }
        const glm::vec3 extent = bounds.extent();
        int axis = 0;
        if (extent.y > extent[axis]) {
            axis = 1;
        }
        if (extent.z > extent[axis]) {
            axis = 2;
        }

        // Largest power of two strictly below count (count / 2 when count is one).
        const u32 leftCount = std::bit_floor(count - 1);
        const auto begin = ids.begin() + first;
        std::nth_element(begin, begin + leftCount, begin + count, [&points, axis](u32 a, u32 b) {
            const float ka = points[a][axis];
            const float kb = points[b][axis];
            return ka < kb || (ka == kb && a < b); // total order: deterministic result
        });

        // Recurse into the smaller part, loop on the larger: bounded stack depth.
        const u32 rightCount = count - leftCount;
        if (leftCount <= rightCount) {
            orderRange(ids, points, first, leftCount);
            first += leftCount;
            count = rightCount;
        } else {
            orderRange(ids, points, first + leftCount, rightCount);
            count = leftCount;
        }
    }
}

std::vector<u32> spatialOrder(const std::vector<glm::vec3>& points)
{
    std::vector<u32> ids(points.size());
    std::iota(ids.begin(), ids.end(), 0u);
    orderRange(ids, points, 0, static_cast<u32>(ids.size()));
    return ids;
}

} // namespace

void sortSpatially(std::vector<glm::vec3>& positions, std::vector<u32>& indices)
{
    const u32 vertexCount = static_cast<u32>(positions.size());
    if (vertexCount < 2) {
        return;
    }

    // --- Vertices -------------------------------------------------------------
    // Their own k-d order, not the order of first use by the sorted triangles.
    // Both were measured (Phase 5): first use gives chunks no fewer distinct
    // vertex pages per view, and a brush dab then dirties 15 to 20% more pages.
    {
        const std::vector<u32> order = spatialOrder(positions);
        std::vector<glm::vec3> sortedPositions(vertexCount);
        std::vector<u32> newIndexOf(vertexCount);
        for (u32 rank = 0; rank < vertexCount; ++rank) {
            sortedPositions[rank] = positions[order[rank]];
            newIndexOf[order[rank]] = rank;
        }
        positions = std::move(sortedPositions);
        for (u32& index : indices) {
            index = newIndexOf[index];
        }
    }

    // --- Triangles, by centroid -------------------------------------------------
    const u32 triangleCount = static_cast<u32>(indices.size() / 3);
    std::vector<glm::vec3> centroids(triangleCount);
    for (u32 triangle = 0; triangle < triangleCount; ++triangle) {
        centroids[triangle] = (positions[indices[triangle * 3 + 0]] + positions[indices[triangle * 3 + 1]] +
                               positions[indices[triangle * 3 + 2]]) *
                              (1.0f / 3.0f);
    }
    const std::vector<u32> order = spatialOrder(centroids);

    std::vector<u32> sortedIndices(indices.size());
    for (u32 rank = 0; rank < triangleCount; ++rank) {
        const u32 source = order[rank];
        // Corner order is kept, so the winding does not change.
        sortedIndices[rank * 3 + 0] = indices[source * 3 + 0];
        sortedIndices[rank * 3 + 1] = indices[source * 3 + 1];
        sortedIndices[rank * 3 + 2] = indices[source * 3 + 2];
    }
    indices = std::move(sortedIndices);
}

} // namespace se::geo