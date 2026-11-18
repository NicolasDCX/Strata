#pragma once

#include "Core/JobSystem.h"
#include "Core/Types.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/Mesh.h"

#include <span>
#include <vector>

namespace se::sculpt {

/// Recomputes the normals invalidated by moving a set of vertices.
///
/// Moving a vertex changes the face normal of every triangle around it, and
/// therefore the vertex normal of every vertex of those triangles: the set to
/// refresh is the moved vertices plus their one-ring. Scratch storage is kept
/// between calls so the steady state does not allocate.
class NormalUpdater {
public:
    /// Marks in `dirty` every vertex whose position or normal may have changed.
    /// `dirty` must be sized for `mesh`. With `jobs`, normals are recomputed in
    /// parallel (each one depends only on positions, so the result is identical).
    void update(geo::Mesh& mesh, std::span<const u32> movedVertices, geo::DirtyVertexSet& dirty,
                core::JobSystem* jobs = nullptr);

    /// The same update when the affected vertices are already known: `affected`
    /// lists each vertex once, the moved vertices and their one-ring
    /// (geo::EditRegion::vertices()). Marks them in `dirty` and recomputes their
    /// normals; the result equals update() for the same moved vertices.
    void updateVertices(geo::Mesh& mesh, std::span<const u32> affected, geo::DirtyVertexSet& dirty,
                        core::JobSystem* jobs = nullptr);

private:
    std::vector<u32> m_stamp;   // per vertex: generation in which it was queued
    std::vector<u32> m_pending; // vertices whose normal is recomputed this call
    u32 m_generation = 0;
};

} // namespace se::sculpt
