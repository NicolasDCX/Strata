#pragma once

#include "Core/Assert.h"
#include "Core/Types.h"

#include <algorithm>
#include <bit>
#include <vector>

namespace se::geo {

/// Set of vertices that changed, tracked per page of 2^pageShift consecutive
/// vertex ids. This is the contract between mesh editing and whoever mirrors
/// the mesh elsewhere (today: the GPU vertex page pool).
///
/// The page size is a parameter. A set created for a mesh uses the mesh's
/// vertex page size, so "dirty page" and "geometry page" are the same thing;
/// the renderer maps the runs to its own GPU page size (see ResidentMeshBuffers::markDirty).
///
/// Granularity is a page, not a vertex: marking is one bit test and the set
/// costs one bit per page. Marking, merging into an already sized set and
/// clearing never allocate, so the set can be used in the per-dab path.
class DirtyVertexSet {
public:
    /// Defaults, used when no page size is given.
    static constexpr u32 kPageShift = 10;
    static constexpr u32 kPageSize = 1u << kPageShift;

    DirtyVertexSet() = default;
    explicit DirtyVertexSet(u32 vertexCount, u32 pageShift = kPageShift) { reset(vertexCount, pageShift); }

    /// Sizes the set for a mesh and clears it.
    void reset(u32 vertexCount, u32 pageShift = kPageShift)
    {
        SE_ASSERT(pageShift < 31);
        m_vertexCount = vertexCount;
        m_shift = pageShift;
        m_words.assign((pageCount() + 63u) / 64u, 0ull);
        m_dirtyPages = 0;
    }

    [[nodiscard]] u32 vertexCount() const noexcept { return m_vertexCount; }
    [[nodiscard]] u32 pageShift() const noexcept { return m_shift; }
    [[nodiscard]] u32 pageSize() const noexcept { return 1u << m_shift; }
    [[nodiscard]] u32 pageCount() const noexcept { return (m_vertexCount + pageSize() - 1u) >> m_shift; }
    [[nodiscard]] u32 dirtyPageCount() const noexcept { return m_dirtyPages; }
    [[nodiscard]] bool empty() const noexcept { return m_dirtyPages == 0; }

    /// Marks the page containing `vertex`. `vertex` must be below vertexCount().
    void mark(u32 vertex) noexcept { markPage(vertex >> m_shift); }

    /// Marks every page overlapping vertices [first, first + count).
    void markRange(u32 first, u32 count) noexcept
    {
        if (count == 0) {
            return;
        }
        const u32 lastPage = (first + count - 1u) >> m_shift;
        for (u32 page = first >> m_shift; page <= lastPage; ++page) {
            markPage(page);
        }
    }

    void markAll() noexcept
    {
        for (u32 page = 0; page < pageCount(); ++page) {
            markPage(page);
        }
    }

    [[nodiscard]] bool contains(u32 vertex) const noexcept
    {
        return vertex < m_vertexCount && pageDirty(vertex >> m_shift);
    }
    [[nodiscard]] bool pageDirty(u32 page) const noexcept { return (m_words[page >> 6] >> (page & 63u)) & 1ull; }

    /// Adds everything marked in `other`. An empty set with a different size or
    /// page size adopts the other's; otherwise both must describe the same
    /// mesh with the same page size.
    void merge(const DirtyVertexSet& other)
    {
        if (other.empty()) {
            return;
        }
        if (m_vertexCount != other.m_vertexCount || m_shift != other.m_shift) {
            SE_ASSERT(empty());
            reset(other.m_vertexCount, other.m_shift);
        }
        for (std::size_t i = 0; i < m_words.size(); ++i) {
            const u64 added = other.m_words[i] & ~m_words[i];
            m_words[i] |= added;
            m_dirtyPages += static_cast<u32>(std::popcount(added));
        }
    }

    void clear() noexcept
    {
        if (m_dirtyPages != 0) {
            std::fill(m_words.begin(), m_words.end(), 0ull);
            m_dirtyPages = 0;
        }
    }

    /// Calls `visit(firstVertex, vertexCount)` for every maximal run of
    /// consecutive dirty pages, in increasing order. Runs are clipped to the mesh.
    template <typename Visitor>
    void forEachRun(Visitor&& visit) const
    {
        const u32 pages = pageCount();
        u32 page = 0;
        while (page < pages) {
            if ((page & 63u) == 0 && m_words[page >> 6] == 0) {
                page += 64; // skip a whole clean word
                continue;
            }
            if (!pageDirty(page)) {
                ++page;
                continue;
            }
            u32 end = page + 1;
            while (end < pages && pageDirty(end)) {
                ++end;
            }
            const u32 first = page << m_shift;
            const u32 last = std::min(end << m_shift, m_vertexCount);
            visit(first, last - first);
            page = end;
        }
    }

    /// Calls `visit(page)` for every dirty page, in increasing order.
    template <typename Visitor>
    void forEachPage(Visitor&& visit) const
    {
        for (std::size_t word = 0; word < m_words.size(); ++word) {
            u64 bits = m_words[word];
            while (bits != 0) {
                visit(static_cast<u32>(word * 64 + static_cast<std::size_t>(std::countr_zero(bits))));
                bits &= bits - 1;
            }
        }
    }

    /// Number of vertices covered by the dirty pages.
    [[nodiscard]] u32 dirtyVertexCount() const noexcept
    {
        u32 total = 0;
        forEachRun([&total](u32, u32 count) { total += count; });
        return total;
    }

private:
    void markPage(u32 page) noexcept
    {
        u64& word = m_words[page >> 6];
        const u64 bit = 1ull << (page & 63u);
        if ((word & bit) == 0) {
            word |= bit;
            ++m_dirtyPages;
        }
    }

    std::vector<u64> m_words; // one bit per page
    u32 m_vertexCount = 0;
    u32 m_shift = kPageShift;
    u32 m_dirtyPages = 0;
};

} // namespace se::geo
