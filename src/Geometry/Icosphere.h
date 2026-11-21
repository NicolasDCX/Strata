#pragma once

#include "Geometry/Mesh.h"

namespace se::geo {

/// Upper bound on icosphere subdivision (level 10 is ~21M triangles, level 11
/// ~84M: about 5.5 GiB of mesh and BVH, 2 GiB of GPU pools).
inline constexpr int kMaxIcosphereSubdivisions = 11;

/// Number of triangles produced by `subdivisions` levels: 20 * 4^n.
[[nodiscard]] constexpr u64 icosphereTriangleCount(int subdivisions) noexcept
{
    return 20ull << (2 * subdivisions);
}

/// Number of vertices produced by `subdivisions` levels: 10 * 4^n + 2.
[[nodiscard]] constexpr u64 icosphereVertexCount(int subdivisions) noexcept
{
    return (10ull << (2 * subdivisions)) + 2ull;
}

enum class VertexOrder {
    /// Vertices and triangles in spatial order (see SpatialOrder.h).
    Spatial,
    /// The order subdivision happens to produce. Only useful for comparisons.
    Generation,
};

/// Bump whenever the subdivision or the spatial ordering changes the output:
/// it invalidates cached spheres (IcosphereCache.h).
inline constexpr u32 kIcosphereGeneratorVersion = 1;

/// The flat arrays makeIcosphere() builds its Mesh from: positions, and three
/// vertex indices per triangle, both in their final order.
void generateIcosphereArrays(int subdivisions, float radius, VertexOrder order, std::vector<glm::vec3>& positions,
                             std::vector<u32>& indices);

/// Sphere built by recursively subdividing an icosahedron and projecting the new
/// vertices onto the sphere. Triangles are near-uniform and wound counter-clockwise
/// when seen from outside. `subdivisions` is clamped to [0, kMaxIcosphereSubdivisions].
/// `layout` selects the page capacities of the resulting mesh.
[[nodiscard]] Mesh makeIcosphere(int subdivisions, float radius = 1.0f, VertexOrder order = VertexOrder::Spatial,
                                 const MeshLayout& layout = {});

} // namespace se::geo
