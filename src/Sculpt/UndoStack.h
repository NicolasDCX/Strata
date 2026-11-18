#pragma once

#include "Core/Types.h"
#include "Geometry/Mesh.h"

#include <glm/vec3.hpp>

#include <span>
#include <vector>

namespace se::sculpt {

/// Sparse record of one completed stroke: only the vertices the stroke touched,
/// with their positions before and after. Normals are derived data and are
/// recomputed when a delta is applied.
struct StrokeDelta {
    std::vector<u32> vertices;
    std::vector<glm::vec3> before;
    std::vector<glm::vec3> after;

    [[nodiscard]] bool empty() const noexcept { return vertices.empty(); }
    [[nodiscard]] std::size_t memoryBytes() const noexcept
    {
        return vertices.capacity() * sizeof(u32) + (before.capacity() + after.capacity()) * sizeof(glm::vec3);
    }
};

/// Linear undo/redo history of strokes. One entry per completed stroke.
class UndoStack {
public:
    explicit UndoStack(std::size_t maxEntries = 128) : m_maxEntries(maxEntries) {}

    /// Records a finished stroke. Discards the redo history.
    void push(StrokeDelta delta);

    /// Restores the positions from before the most recent stroke.
    /// Returns the vertices that were written (empty when there is nothing to undo).
    /// The span stays valid until the next call that modifies the stack.
    std::span<const u32> undo(geo::Mesh& mesh);

    /// Re-applies the most recently undone stroke. Same contract as undo().
    std::span<const u32> redo(geo::Mesh& mesh);

    /// Vertices the next history operation will write, without advancing the cursor.
    [[nodiscard]] std::span<const u32> undoVertices() const noexcept
    {
        if (!canUndo()) {
            return {};
        }
        return m_entries[m_cursor - 1].vertices;
    }

    [[nodiscard]] std::span<const u32> redoVertices() const noexcept
    {
        if (!canRedo()) {
            return {};
        }
        return m_entries[m_cursor].vertices;
    }

    /// The positions the next undo / redo will write, parallel to undoVertices()
    /// / redoVertices().
    [[nodiscard]] std::span<const glm::vec3> undoPositions() const noexcept
    {
        if (!canUndo()) {
            return {};
        }
        return m_entries[m_cursor - 1].before;
    }
    [[nodiscard]] std::span<const glm::vec3> redoPositions() const noexcept
    {
        if (!canRedo()) {
            return {};
        }
        return m_entries[m_cursor].after;
    }

    [[nodiscard]] bool canUndo() const noexcept { return m_cursor > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return m_cursor < m_entries.size(); }
    [[nodiscard]] std::size_t undoCount() const noexcept { return m_cursor; }
    [[nodiscard]] std::size_t redoCount() const noexcept { return m_entries.size() - m_cursor; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

    void clear() noexcept;

private:
    std::vector<StrokeDelta> m_entries; // [0, m_cursor) can be undone, [m_cursor, size) redone
    std::size_t m_cursor = 0;
    std::size_t m_maxEntries;
};

} // namespace se::sculpt
