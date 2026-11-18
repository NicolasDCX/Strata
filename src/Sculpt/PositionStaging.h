#pragma once

#include "Core/Types.h"
#include "Geometry/Mesh.h"
#include "Geometry/PagedArray.h"

#include <glm/vec3.hpp>

#include <memory>
#include <span>
#include <vector>

namespace se::sculpt {

/// Proposed vertex positions without touching the mesh: a private page table
/// in which every vertex page holding one of the staged vertices points to a
/// copy of that page, and every other page to the mesh's own page. Writes
/// through view() change only the copies, and reads through it see the
/// proposed positions of staged pages and the current positions elsewhere.
///
/// Used by StrokeProcessor for every dab (the brushes write the view), and by
/// the controller's undo and redo (the history entry's positions are written
/// to the view, and the normals are computed from it before the lock).
/// One writer; nothing here is visible to readers of the mesh.
class PositionStaging {
public:
    explicit PositionStaging(geo::Mesh& mesh);

    /// Copies the vertex pages holding `vertices` and points the table at the
    /// copies. Must be followed by release() before the next stage().
    void stage(std::span<const u32> vertices);
    /// Points the table back at the mesh's pages. The copies are kept for reuse.
    void release();

    [[nodiscard]] geo::PagedArrayView<glm::vec3> view() noexcept;
    [[nodiscard]] geo::PagedArrayView<const glm::vec3> view() const noexcept;
    /// Pages copied by the current stage() call.
    [[nodiscard]] std::size_t stagedPageCount() const noexcept { return m_stagedPages.size(); }
    /// Copies kept for reuse, the page table and the per-page flags.
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

private:
    geo::Mesh& m_mesh;
    std::vector<glm::vec3*> m_table;                  // per vertex page: staging copy, or the mesh's page
    std::vector<u32> m_stagedPages;                   // pages copied by the current stage()
    std::vector<u8> m_pageStaged;                     // per vertex page: 1 while staged
    std::vector<std::unique_ptr<glm::vec3[]>> m_pool; // staging pages, reused
};

} // namespace se::sculpt
