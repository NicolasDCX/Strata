#include "Sculpt/NormalUpdater.h"

#include <algorithm>

namespace se::sculpt {

void NormalUpdater::update(geo::Mesh& mesh, std::span<const u32> movedVertices, geo::DirtyVertexSet& dirty,
                           core::JobSystem* jobs)
{
    if (movedVertices.empty()) {
        return;
    }

    if (m_stamp.size() != mesh.vertexCount()) {
        m_stamp.assign(mesh.vertexCount(), 0);
        m_generation = 0;
    }
    if (++m_generation == 0) { // wrapped: old stamps could alias the new generation
        std::fill(m_stamp.begin(), m_stamp.end(), 0u);
        m_generation = 1;
    }

    m_pending.clear();
    const auto queue = [this, &dirty](u32 vertex) {
        if (m_stamp[vertex] != m_generation) {
            m_stamp[vertex] = m_generation;
            m_pending.push_back(vertex);
            dirty.mark(vertex);
        }
    };
    for (const u32 vertex : movedVertices) {
        queue(vertex);
        mesh.forEachNeighbor(vertex, queue);
    }

    const std::span<const u32> pending(m_pending);
    core::parallelFor(jobs, pending.size(), 4096, [&mesh, pending](std::size_t begin, std::size_t end) {
        mesh.recomputeNormals(pending.subspan(begin, end - begin));
    });
}

void NormalUpdater::updateVertices(geo::Mesh& mesh, std::span<const u32> affected, geo::DirtyVertexSet& dirty,
                                   core::JobSystem* jobs)
{
    for (const u32 vertex : affected) {
        dirty.mark(vertex);
    }
    core::parallelFor(jobs, affected.size(), 4096, [&mesh, affected](std::size_t begin, std::size_t end) {
        mesh.recomputeNormals(affected.subspan(begin, end - begin));
    });
}

} // namespace se::sculpt
