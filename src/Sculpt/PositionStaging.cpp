#include "Sculpt/PositionStaging.h"

#include "Core/Assert.h"

#include <cstring>

namespace se::sculpt {

PositionStaging::PositionStaging(geo::Mesh& mesh) : m_mesh(mesh), m_pageStaged(mesh.vertexPageCount(), u8{0})
{
    const auto live = m_mesh.mutablePositions();
    m_table.assign(live.table(), live.table() + m_mesh.vertexPageCount());
}

void PositionStaging::stage(std::span<const u32> vertices)
{
    SE_ASSERT(m_stagedPages.empty());
    const u32 shift = m_mesh.layout().vertexPageShift;
    for (const u32 vertex : vertices) {
        const u32 page = vertex >> shift;
        if (m_pageStaged[page] == 0) {
            m_pageStaged[page] = 1;
            m_stagedPages.push_back(page);
        }
    }
    const std::size_t capacity = m_mesh.layout().vertexPageCapacity();
    while (m_pool.size() < m_stagedPages.size()) {
        m_pool.push_back(std::make_unique_for_overwrite<glm::vec3[]>(capacity));
    }
    const auto live = m_mesh.positions();
    const std::size_t lastPage = m_mesh.vertexPageCount() - 1;
    const std::size_t lastPageSize = m_mesh.vertexCount() - lastPage * capacity;
    for (std::size_t i = 0; i < m_stagedPages.size(); ++i) {
        const u32 page = m_stagedPages[i];
        glm::vec3* copy = m_pool[i].get();
        std::memcpy(copy, live.table()[page], (page == lastPage ? lastPageSize : capacity) * sizeof(glm::vec3));
        m_table[page] = copy;
    }
}

void PositionStaging::release()
{
    const auto live = m_mesh.mutablePositions();
    for (const u32 page : m_stagedPages) {
        m_table[page] = live.table()[page];
        m_pageStaged[page] = 0;
    }
    m_stagedPages.clear();
}

geo::PagedArrayView<glm::vec3> PositionStaging::view() noexcept
{
    return {m_table.data(), m_mesh.layout().vertexPageShift, m_mesh.vertexCount()};
}

geo::PagedArrayView<const glm::vec3> PositionStaging::view() const noexcept
{
    return {const_cast<const glm::vec3* const*>(m_table.data()), m_mesh.layout().vertexPageShift, m_mesh.vertexCount()};
}

std::size_t PositionStaging::memoryBytes() const noexcept
{
    return m_pool.size() * m_mesh.layout().vertexPageCapacity() * sizeof(glm::vec3) +
           m_table.capacity() * sizeof(glm::vec3*) + m_stagedPages.capacity() * sizeof(u32) + m_pageStaged.capacity();
}

} // namespace se::sculpt
