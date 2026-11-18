#include "Sculpt/UndoStack.h"

#include "Core/Assert.h"

#include <utility>

namespace se::sculpt {

namespace {

void writePositions(geo::Mesh& mesh, std::span<const u32> vertices, std::span<const glm::vec3> values)
{
    const auto positions = mesh.mutablePositions();
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        positions[vertices[i]] = values[i];
    }
}

} // namespace

void UndoStack::push(StrokeDelta delta)
{
    SE_ASSERT(delta.vertices.size() == delta.before.size());
    SE_ASSERT(delta.vertices.size() == delta.after.size());
    if (delta.empty()) {
        return;
    }
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_entries.end());
    m_entries.push_back(std::move(delta));
    if (m_entries.size() > m_maxEntries) {
        m_entries.erase(m_entries.begin());
    }
    m_cursor = m_entries.size();
}

std::span<const u32> UndoStack::undo(geo::Mesh& mesh)
{
    if (!canUndo()) {
        return {};
    }
    const StrokeDelta& delta = m_entries[--m_cursor];
    writePositions(mesh, delta.vertices, delta.before);
    return delta.vertices;
}

std::span<const u32> UndoStack::redo(geo::Mesh& mesh)
{
    if (!canRedo()) {
        return {};
    }
    const StrokeDelta& delta = m_entries[m_cursor++];
    writePositions(mesh, delta.vertices, delta.after);
    return delta.vertices;
}

std::size_t UndoStack::memoryBytes() const noexcept
{
    std::size_t bytes = 0;
    for (const StrokeDelta& delta : m_entries) {
        bytes += delta.memoryBytes();
    }
    return bytes;
}

void UndoStack::clear() noexcept
{
    m_entries.clear();
    m_cursor = 0;
}

} // namespace se::sculpt
