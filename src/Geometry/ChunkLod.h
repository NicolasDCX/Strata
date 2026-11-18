#pragma once

#include "Core/JobSystem.h"
#include "Core/Types.h"
#include "Geometry/Mesh.h"

#include <span>
#include <vector>

namespace se::geo {

/// Coarse stand-ins for spatial chunks (opt-in coarse render fallback, `--lod`).
///
/// Chunks (2^chunkShift consecutive triangles, as in ChunkedBvh) are simplified
/// in groups of 2^groupShift consecutive chunks. Triangles are in Morton order,
/// so a group is a compact patch, like a chunk. A group is simplified on its own
/// by half-edge collapses: a vertex is merged into one of its neighbours, so
/// every coarse vertex is an original vertex id. After a sculpting edit the
/// coarse version of a group is brought up to date by copying the current
/// positions and normals of those ids; topology never changes.
///
/// Crack-free by construction: a vertex referenced by another group, or lying
/// on an edge with only one triangle in the group, is never removed, and no
/// collapse may delete a triangle that holds such a border edge. So every border
/// edge of the group survives unchanged, with the same orientation, and a
/// coarse group meets its neighbours (coarse or detailed) along exactly the
/// same edges, made of the same vertices. Borders between the chunks inside a
/// group are not kept: a group is drawn coarse as a whole or not at all.
///
/// Why groups: the locked border is what limits simplification. Per chunk
/// (groupShift 0) about 92% of the coarse vertices are border vertices; with 4
/// chunks per group the interior chunk borders are free to collapse, and the
/// same triangle budget needs about a quarter fewer vertices. The cost: the
/// topology is fixed at build time, and a group's few large interior
/// triangles follow strong later deformation much worse than single chunks,
/// so the renderer defaults to groupShift 0.
struct CoarseChunks {
    u32 chunkShift = 0;             ///< log2 of triangles per chunk
    u32 groupShift = 0;             ///< log2 of chunks per group
    std::vector<u32> vertexOffsets; ///< groupCount + 1, into `vertices`
    std::vector<VertexId> vertices; ///< per group, the original ids of its coarse vertices
    std::vector<u32> indexOffsets;  ///< groupCount + 1, into `indices`
    std::vector<u16> indices;       ///< per group, 3 per triangle, local to the group's vertex list
    u32 maxGroupVertices = 0;

    [[nodiscard]] u32 groupCount() const noexcept
    {
        return vertexOffsets.empty() ? 0u : static_cast<u32>(vertexOffsets.size() - 1);
    }
    [[nodiscard]] u32 groupOf(u32 chunk) const noexcept { return chunk >> groupShift; }
    [[nodiscard]] u32 firstChunk(u32 group) const noexcept { return group << groupShift; }
    [[nodiscard]] std::span<const VertexId> groupVertices(u32 group) const noexcept
    {
        return {vertices.data() + vertexOffsets[group], vertexOffsets[group + 1] - vertexOffsets[group]};
    }
    [[nodiscard]] std::span<const u16> groupIndices(u32 group) const noexcept
    {
        return {indices.data() + indexOffsets[group], indexOffsets[group + 1] - indexOffsets[group]};
    }
    [[nodiscard]] u32 groupTriangles(u32 group) const noexcept { return (indexOffsets[group + 1] - indexOffsets[group]) / 3u; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept
    {
        return (vertexOffsets.capacity() + vertices.capacity() + indexOffsets.capacity()) * sizeof(u32) +
               indices.capacity() * sizeof(u16);
    }
};

/// Simplifies every group of 2^groupShift chunks toward `targetRatio` of its
/// triangles (it stops earlier when no valid collapse is left; locked borders
/// bound the result). Uses the current positions for the error metric. Groups
/// are processed in parallel when `jobs` is given; the result does not depend
/// on it.
[[nodiscard]] CoarseChunks buildCoarseChunks(const Mesh& mesh, u32 chunkTriangleShift, u32 groupShift, float targetRatio,
                                             core::JobSystem* jobs = nullptr);

} // namespace se::geo
