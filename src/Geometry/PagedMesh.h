#pragma once

#include "Core/Types.h"
#include "Geometry/Aabb.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/PagedArray.h"

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace se::geo {

/// Stable handle of a vertex / triangle. It is assigned when the element is
/// created and never changes: pages are never moved, compacted or renumbered.
/// The handle encodes where the element lives: `id >> pageShift` is the page,
/// `id & (pageCapacity - 1)` the slot inside it. Adjacency, undo records, dirty
/// sets, BVH leaves and GPU indices all store these handles.
using VertexId = u32;
using TriangleId = u32;

/// How the per-vertex adjacency lists are stored (see PagedAdjacency).
enum class AdjacencyLayout : u8 {
    /// One u32 offset per vertex and one u32 id per entry.
    Wide,
    /// One u32 header per vertex and, per entry, a signed 16-bit offset from a
    /// per-page anchor; rows that do not fit keep u32 ids. The default: about
    /// 58% of the wide size, same ids in the same order.
    Compact,
};

/// Whether the vertex -> neighbour lists are stored or derived on demand.
enum class NeighborStorage : u8 {
    /// A neighbour table built at construction (sorted, unique rows).
    Stored,
    /// No neighbour table: a vertex's neighbours are the other corners of its
    /// incident triangles, sorted and deduplicated when read. Same ids in the
    /// same order as the stored rows, so results are bit-identical.
    Derived,
};

/// Page capacities of a mesh, as powers of two, and the adjacency storage.
struct MeshLayout {
    u32 vertexPageShift = 10;   ///< 2^n vertices per vertex page
    u32 trianglePageShift = 12; ///< 2^n triangles per triangle page
    AdjacencyLayout adjacency = AdjacencyLayout::Compact;
    NeighborStorage neighbors = NeighborStorage::Stored;

    [[nodiscard]] u32 vertexPageCapacity() const noexcept { return 1u << vertexPageShift; }
    [[nodiscard]] u32 trianglePageCapacity() const noexcept { return 1u << trianglePageShift; }
};

/// Bytes held by a mesh, by kind.
struct MeshMemory {
    std::size_t positions = 0;
    std::size_t normals = 0;
    std::size_t triangles = 0;
    std::size_t adjacency = 0; ///< neighbours + incident triangles (the two fields below)
    std::size_t adjacencyNeighbors = 0;         ///< part of `adjacency`: vertex -> neighbour vertices
    std::size_t adjacencyIncidentTriangles = 0; ///< part of `adjacency`: vertex -> incident triangles
    std::size_t bookkeeping = 0; ///< page bounds, stale flags

    [[nodiscard]] std::size_t total() const noexcept { return positions + normals + triangles + adjacency + bookkeeping; }
};

/// One adjacency list: the ids of a vertex's neighbours or incident triangles,
/// in ascending order. Used like a span (range-for, size(), empty(), front()),
/// but elements come by value, because a compact row is decoded as it is read.
class AdjacencyRow {
public:
    class iterator {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = u32;
        using difference_type = std::ptrdiff_t;
        using reference = u32;
        using pointer = void;

        iterator() = default;

        [[nodiscard]] u32 operator*() const noexcept
        {
            return m_wideIds ? *m_wide : AdjacencyRow::decode(m_anchor, *m_narrow);
        }
        iterator& operator++() noexcept
        {
            if (m_wideIds) {
                ++m_wide;
            } else {
                ++m_narrow;
            }
            --m_remaining;
            return *this;
        }
        iterator operator++(int) noexcept
        {
            iterator copy = *this;
            ++*this;
            return copy;
        }
        /// Iterators of one row compare by position (entries left).
        friend bool operator==(const iterator& a, const iterator& b) noexcept { return a.m_remaining == b.m_remaining; }

    private:
        friend class AdjacencyRow;
        const u32* m_wide = nullptr;   // u32 rows: the current entry
        const u16* m_narrow = nullptr; // u16 rows: the current entry
        u32 m_anchor = 0;
        u32 m_remaining = 0;
        bool m_wideIds = false;
    };

    AdjacencyRow() = default;
    /// Row of `count` u32 ids.
    AdjacencyRow(const u32* ids, u32 count) noexcept : m_wide(ids), m_count(count), m_wideIds(true) {}
    /// Row of `count` signed 16-bit offsets from `anchor`.
    AdjacencyRow(const u16* offsets, u32 count, u32 anchor) noexcept : m_narrow(offsets), m_count(count), m_anchor(anchor) {}

    [[nodiscard]] u32 size() const noexcept { return m_count; }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] u32 front() const noexcept { return *begin(); }

    [[nodiscard]] iterator begin() const noexcept
    {
        iterator it;
        it.m_wide = m_wide;
        it.m_narrow = m_narrow;
        it.m_anchor = m_anchor;
        it.m_remaining = m_count;
        it.m_wideIds = m_wideIds;
        return it;
    }
    [[nodiscard]] iterator end() const noexcept { return {}; }

    /// Calls `f(id)` for every entry, in order: the same ids as a range-for,
    /// with the row's storage tested once instead of once per entry. Used by
    /// the per-vertex loops of sculpting.
    template <typename Function>
    void forEach(Function&& f) const
    {
        // An explicit flag rather than a null test: once row() is inlined the
        // compiler knows which branch a row takes and drops the other.
        if (m_wideIds) {
            for (u32 i = 0; i < m_count; ++i) {
                f(m_wide[i]);
            }
        } else {
            for (u32 i = 0; i < m_count; ++i) {
                f(decode(m_anchor, m_narrow[i]));
            }
        }
    }

    /// Value of a 16-bit entry: the anchor plus the entry read as a signed offset.
    [[nodiscard]] static u32 decode(u32 anchor, u16 offset) noexcept
    {
        return anchor + static_cast<u32>(static_cast<i32>(static_cast<std::int16_t>(offset)));
    }

private:
    const u32* m_wide = nullptr;
    const u16* m_narrow = nullptr;
    u32 m_count = 0;
    u32 m_anchor = 0;
    bool m_wideIds = false;
};

/// The neighbours of one vertex, as returned by PagedMesh::neighbors(): a view
/// of the stored row, or (derived neighbours) the ids themselves, held inline.
/// A value type: it stays valid when the mesh is read again. For hot loops use
/// PagedMesh::forEachNeighbor(), which builds nothing.
class NeighborRing {
public:
    static constexpr u32 kInline = 32;

    NeighborRing() = default;
    explicit NeighborRing(AdjacencyRow stored) noexcept : m_stored(stored) {}
    /// Takes ids already sorted and unique.
    explicit NeighborRing(std::span<const u32> ids) : m_derived(true), m_count(static_cast<u32>(ids.size()))
    {
        if (ids.size() <= kInline) {
            std::copy(ids.begin(), ids.end(), m_inline.begin());
        } else {
            m_heap.assign(ids.begin(), ids.end());
        }
    }

    [[nodiscard]] AdjacencyRow row() const noexcept
    {
        return m_derived ? AdjacencyRow(m_count <= kInline ? m_inline.data() : m_heap.data(), m_count) : m_stored;
    }
    [[nodiscard]] u32 size() const noexcept { return m_derived ? m_count : m_stored.size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] u32 front() const noexcept { return row().front(); }
    [[nodiscard]] AdjacencyRow::iterator begin() const noexcept { return row().begin(); }
    [[nodiscard]] AdjacencyRow::iterator end() const noexcept { return {}; }
    template <typename Function>
    void forEach(Function&& f) const
    {
        row().forEach(f);
    }

private:
    AdjacencyRow m_stored;
    bool m_derived = false;
    u32 m_count = 0;
    std::array<u32, kInline> m_inline{};
    std::vector<u32> m_heap;
};

/// Per-vertex lists (neighbour vertices, incident triangles) stored page by
/// page, aligned with the vertex pages: the lists of the vertices of page p are
/// in adjacency page p. Entries are global ids, so a vertex's neighbours can be
/// in any page; nothing special happens at page boundaries.
///
/// Two layouts (AdjacencyLayout), chosen per table at construction. Both give
/// back exactly the ids that were stored, in the same order.
/// - Wide: per page, (vertices + 1) u32 offsets followed by u32 entries.
///   28 bytes per vertex of valence 6.
/// - Compact: per page, one u32 anchor (the median entry of the page), one u32
///   header per vertex, then the rows. Header bits 0-23: row start in u16
///   words from the block start; bit 24: the row holds u32 ids; bits 25-31:
///   entry count, 127 meaning that the count is in the u32 just before the
///   row. A row whose entries all lie within [anchor - 32768, anchor + 32767]
///   is stored as 16-bit offsets from the anchor; any other row as u32 ids, so
///   no id is ever out of reach. Spatial order (SpatialOrder.h) keeps the ids
///   of neighbouring elements close: on the icosphere about 1 row in 40 needs
///   u32 ids. 16 bytes per vertex of valence 6 (28 for a u32 row). Entries are
///   independent of each other, so decoding is a sign extension and an add,
///   with no dependency from one entry to the next.
class PagedAdjacency {
public:
    /// Splits flat compressed rows (`offsets` has vertexCount + 1 entries) into pages.
    void assign(std::span<const u32> offsets, std::span<const u32> data, u32 pageShift);

    /// Compact layout, built one page at a time with no flat copy of the whole
    /// table. `rowOf(vertex, out)` replaces `out` with that vertex's entries.
    template <typename RowFunction>
    void assignCompact(u32 vertexCount, u32 pageShift, RowFunction&& rowOf);

    [[nodiscard]] AdjacencyRow row(VertexId vertex) const noexcept
    {
        const u32* const block = m_pages[vertex >> m_shift];
        const u32 slot = vertex & m_mask;
        if (!m_compact) {
            // One page block holds [offsets | data]; `block` points at the offsets.
            const u32 begin = block[slot];
            return {block + begin, block[slot + 1] - begin};
        }
        const u32 header = block[1 + slot];
        const u32 start = header & kOffsetMask; // in u16 words
        u32 count = header >> kCountShift;
        if ((header & kWideBit) != 0) [[unlikely]] {
            const u32* const ids = block + start / 2; // u32 rows start on an even word
            if (count == kCountInRow) [[unlikely]] {
                count = ids[-1];
            }
            return {ids, count};
        }
        const u16* const offsets = reinterpret_cast<const u16*>(block) + start;
        if (count == kCountInRow) [[unlikely]] {
            count = block[start / 2 - 1];
        }
        return {offsets, count, block[0]};
    }

    [[nodiscard]] AdjacencyLayout layout() const noexcept { return m_compact ? AdjacencyLayout::Compact : AdjacencyLayout::Wide; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;
    /// Rows stored as u32 ids although the layout is compact.
    [[nodiscard]] std::size_t wideRows() const noexcept { return m_wideRows; }

private:
    static constexpr u32 kOffsetMask = (1u << 24) - 1;
    static constexpr u32 kWideBit = 1u << 24;
    static constexpr u32 kCountShift = 25;
    static constexpr u32 kCountInRow = 0x7F;

    void reset(u32 vertexCount, u32 pageShift);
    /// Encodes one compact page: row i of the page is entries [rowStarts[i], rowStarts[i + 1]).
    void storeCompactPage(std::size_t pageIndex, std::span<const u32> rowStarts, std::span<const u32> entries);

    // Each page is one allocation, addressed through m_pages. Offsets inside a
    // block are relative to its start, so a lookup needs the block pointer and
    // nothing else.
    std::vector<const u32*> m_pages;
    std::vector<std::unique_ptr<u32[]>> m_blocks;
    std::size_t m_allocatedWords = 0;
    std::size_t m_wideRows = 0;
    u32 m_shift = 0;
    u32 m_mask = 0;
    bool m_compact = false;
};

template <typename RowFunction>
void PagedAdjacency::assignCompact(u32 vertexCount, u32 pageShift, RowFunction&& rowOf)
{
    reset(vertexCount, pageShift);
    m_compact = true;
    std::vector<u32> rowStarts;
    std::vector<u32> entries;
    std::vector<u32> row;
    const u32 capacity = 1u << pageShift;
    for (std::size_t pageIndex = 0; pageIndex < m_pages.size(); ++pageIndex) {
        const u32 first = static_cast<u32>(pageIndex) << pageShift;
        const u32 last = std::min(first + capacity, vertexCount);
        rowStarts.assign(1, 0u);
        entries.clear();
        for (u32 vertex = first; vertex < last; ++vertex) {
            rowOf(vertex, row);
            entries.insert(entries.end(), row.begin(), row.end());
            rowStarts.push_back(static_cast<u32>(entries.size()));
        }
        storeCompactPage(pageIndex, rowStarts, entries);
    }
}

/// Flat, read-only view of the triangle corners: element 3t + k is corner k of
/// triangle t. Kept so that code written against an index array keeps working;
/// performance-sensitive code uses PagedMesh::triangle() instead.
class TriangleIndexView {
public:
    using iterator = ViewIterator<TriangleIndexView>;

    TriangleIndexView() = default;
    explicit TriangleIndexView(PagedArrayView<const std::array<u32, 3>> triangles) : m_triangles(triangles) {}

    [[nodiscard]] const u32& operator[](std::size_t index) const noexcept { return m_triangles[index / 3][index % 3]; }
    [[nodiscard]] std::size_t size() const noexcept { return m_triangles.size() * 3; }
    [[nodiscard]] iterator begin() const { return iterator(*this, 0); }
    [[nodiscard]] iterator end() const { return iterator(*this, size()); }

private:
    PagedArrayView<const std::array<u32, 3>> m_triangles;
};

/// Indexed triangle mesh with fixed topology and editable vertex positions,
/// stored in fixed-capacity pages. This is the single authoritative copy of the
/// geometry; everything else (BVH, GPU buffers, undo) refers to it by id.
///
/// Storage
/// - Vertex pages: positions and normals, two parallel paged arrays with the
///   same page layout (page p of both arrays describes the same vertices).
/// - Triangle pages: three vertex ids per triangle.
/// - Adjacency pages: per vertex page, the neighbours and incident triangles of
///   its vertices.
/// Vertex and triangle pages have independent capacities and no fixed relation
/// to each other, to spatial chunks (see ChunkedBvh) or to GPU pages.
///
/// What paging provides today: stable ids, allocation in page-sized steps with
/// no mesh-sized contiguous block, and a unit for dirty tracking, bounds and
/// uploads. What it prepares: pages are independent allocations addressed
/// through a page table, which is the hook for loading and evicting parts of a
/// mesh later. Nothing is streamed yet.
///
/// The class knows nothing about rendering or brushes.
class PagedMesh {
public:
    using Triangle = std::array<VertexId, 3>;

    PagedMesh() = default;

    /// Builds the paged mesh from flat arrays (which are consumed), then adjacency,
    /// normals and bounds. `indices.size()` must be a multiple of 3 and every
    /// index must be valid. Vertex i becomes VertexId i.
    PagedMesh(std::vector<glm::vec3> positions, std::vector<u32> indices, const MeshLayout& layout = {});

    [[nodiscard]] u32 vertexCount() const noexcept { return static_cast<u32>(m_positions.size()); }
    [[nodiscard]] u32 triangleCount() const noexcept { return static_cast<u32>(m_triangles.size()); }
    [[nodiscard]] const MeshLayout& layout() const noexcept { return m_layout; }

    // --- Element access by id ------------------------------------------------
    [[nodiscard]] PagedArrayView<const glm::vec3> positions() const noexcept { return m_positions.view(); }
    [[nodiscard]] PagedArrayView<const glm::vec3> normals() const noexcept { return m_normals.view(); }
    /// Editing access. After writing, the caller recomputes the normals of the
    /// touched region and calls invalidatePageBounds().
    [[nodiscard]] PagedArrayView<glm::vec3> mutablePositions() noexcept { return m_positions.view(); }
    /// Publishing access to normals computed elsewhere (computeNormals()). The
    /// caller keeps them consistent with the positions.
    [[nodiscard]] PagedArrayView<glm::vec3> mutableNormals() noexcept { return m_normals.view(); }

    [[nodiscard]] const Triangle& triangle(TriangleId id) const noexcept { return m_triangles[id]; }
    [[nodiscard]] PagedArrayView<const Triangle> triangles() const noexcept { return m_triangles.view(); }
    [[nodiscard]] TriangleIndexView indices() const noexcept { return TriangleIndexView(m_triangles.view()); }

    /// Vertices sharing an edge with `vertex` (sorted, unique), stored or
    /// derived. Convenient, not fast: per-vertex loops use forEachNeighbor().
    [[nodiscard]] NeighborRing neighbors(VertexId vertex) const;
    /// Calls `f(neighbour)` for the neighbours of `vertex` in ascending order
    /// (the order of neighbors()) and returns how many there were. With
    /// derived neighbours the ring is built on the stack from the incident
    /// triangles: no allocation unless the vertex has more than
    /// kDerivedInline candidate corners. Safe to call from several threads.
    template <typename Function>
    u32 forEachNeighbor(VertexId vertex, Function&& f) const;
    /// Derived neighbours: vertices whose ring takes the general derivation
    /// (border, irregular or high-valence fans). 0 with stored neighbours.
    [[nodiscard]] u32 generalFanCount() const noexcept;
    /// True when the neighbour table is stored (NeighborStorage::Stored).
    [[nodiscard]] bool storesNeighbors() const noexcept { return m_layout.neighbors == NeighborStorage::Stored; }
    /// Triangles that reference `vertex` (ascending).
    [[nodiscard]] AdjacencyRow incidentTriangles(VertexId vertex) const noexcept { return m_vertexTriangles.row(vertex); }
    /// Adjacency rows kept as u32 ids in the compact layout (see PagedAdjacency).
    [[nodiscard]] std::size_t wideAdjacencyRows() const noexcept
    {
        return m_neighbors.wideRows() + m_vertexTriangles.wideRows();
    }

    // --- Pages -----------------------------------------------------------------
    [[nodiscard]] u32 vertexPageCount() const noexcept { return static_cast<u32>(m_positions.pageCount()); }
    [[nodiscard]] u32 trianglePageCount() const noexcept { return static_cast<u32>(m_triangles.pageCount()); }
    [[nodiscard]] u32 vertexPageOf(VertexId vertex) const noexcept { return vertex >> m_layout.vertexPageShift; }
    [[nodiscard]] u32 trianglePageOf(TriangleId triangle) const noexcept { return triangle >> m_layout.trianglePageShift; }

    [[nodiscard]] std::span<const glm::vec3> positionPage(u32 page) const noexcept { return m_positions.page(page); }
    [[nodiscard]] std::span<const glm::vec3> normalPage(u32 page) const noexcept { return m_normals.page(page); }
    [[nodiscard]] std::span<const Triangle> trianglePage(u32 page) const noexcept { return m_triangles.page(page); }

    /// Copy a range of ids to contiguous memory (used to fill GPU buffers).
    void copyPositions(VertexId first, u32 count, glm::vec3* destination) const noexcept
    {
        m_positions.copyOut(first, count, destination);
    }
    void copyNormals(VertexId first, u32 count, glm::vec3* destination) const noexcept
    {
        m_normals.copyOut(first, count, destination);
    }
    /// Writes 3 * count vertex ids.
    void copyIndices(TriangleId first, u32 count, u32* destination) const noexcept;

    /// A dirty set sized for this mesh, one bit per vertex page.
    [[nodiscard]] DirtyVertexSet makeDirtySet() const { return DirtyVertexSet(vertexCount(), m_layout.vertexPageShift); }

    /// Bounds of the vertices of one page. Kept lazily: recomputed on the first
    /// request after the page was invalidated. Not thread-safe against writers;
    /// call it under the same lock that protects the vertex data.
    [[nodiscard]] Aabb vertexPageBounds(u32 page) const;
    /// Tells the mesh which vertex pages were written (any page size).
    void invalidatePageBounds(const DirtyVertexSet& modified);

    // --- Whole mesh --------------------------------------------------------------
    /// Bounds as of construction or the last recomputeBounds().
    [[nodiscard]] const Aabb& bounds() const noexcept { return m_bounds; }
    void recomputeBounds() noexcept;

    /// Area-weighted vertex normals for the whole mesh.
    void recomputeNormals() noexcept;
    /// Area-weighted normals for the listed vertices only. Does not allocate.
    /// Safe to call from several threads for disjoint vertex lists.
    void recomputeNormals(std::span<const VertexId> vertices) noexcept;
    /// The normals recomputeNormals(vertices) would store if the positions were
    /// `positions` (a view with this mesh's vertex page layout, for example one
    /// whose edited pages are staging copies), written to `out` (parallel to
    /// `vertices`) without changing the mesh. A fully collapsed fan keeps the
    /// vertex's current normal, as recomputeNormals does. The arithmetic is the
    /// same, so the results are the same bit for bit. Safe to call from several
    /// threads for disjoint parts of `vertices` and `out`.
    void computeNormals(std::span<const VertexId> vertices, PagedArrayView<const glm::vec3> positions,
                        std::span<glm::vec3> out) const noexcept;

    /// Structural validation. Returns false and fills `reason` on the first problem.
    [[nodiscard]] bool validate(std::string* reason = nullptr) const;

    [[nodiscard]] MeshMemory memory() const noexcept;
    [[nodiscard]] std::size_t memoryBytes() const noexcept { return memory().total(); }

    /// Candidate corners (2 per incident triangle) derived on the stack.
    static constexpr u32 kDerivedInline = 64;
    /// Writes the sorted, unique neighbours of `vertex` to `out` (room for
    /// 2 * incidentTriangles(vertex).size() ids) and returns their count.
    u32 deriveNeighbors(VertexId vertex, u32* out) const noexcept;
    /// The general derivation: every other corner of every incident
    /// triangle, sorted and deduplicated by insertion. Any topology.
    u32 deriveNeighborsGeneral(VertexId vertex, u32* out) const noexcept;
    [[nodiscard]] bool hasRegularFan(VertexId vertex) const noexcept
    {
        return (m_regularFan[vertex >> 6] >> (vertex & 63u) & 1u) != 0;
    }
    /// The fast derivation for a regular fan of at most 8 triangles: `next`
    /// receives the ring sorted (unused slots ~0u); returns its size.
    u32 regularRing(AdjacencyRow triangles, VertexId vertex, u32 (&next)[8]) const noexcept;

private:
    void buildTopology(std::span<const u32> flatIndices);
    void buildCompactTopology(std::span<const u32> flatIndices);
    /// Derived neighbours: fills m_regularFan.
    void classifyFans();

    MeshLayout m_layout;
    PagedArray<glm::vec3> m_positions;
    PagedArray<glm::vec3> m_normals;
    PagedArray<Triangle> m_triangles;
    PagedAdjacency m_vertexTriangles;
    PagedAdjacency m_neighbors;
    // Derived neighbours: one bit per vertex, set when its triangle fan is a
    // closed, consistently wound disk of at most kRegularFanMax triangles.
    // Then each neighbour is the corner after the vertex in exactly one
    // incident triangle, which gives the ring without deduplication.
    static constexpr u32 kRegularFanMax = 8;
    std::vector<u64> m_regularFan;

    Aabb m_bounds;
    mutable std::vector<Aabb> m_pageBounds;    // per vertex page, valid where not stale
    mutable std::vector<u8> m_pageBoundsStale; // per vertex page: cached bounds are out of date
};

namespace detail {
/// Sorts 8 values with a 19-comparator network (branchless min/max).
inline void sortEight(u32 (&v)[8]) noexcept
{
    const auto exchange = [&v](int i, int j) {
        const u32 a = v[i];
        const u32 b = v[j];
        v[i] = a < b ? a : b;
        v[j] = a < b ? b : a;
    };
    exchange(0, 2); exchange(1, 3); exchange(4, 6); exchange(5, 7);
    exchange(0, 4); exchange(1, 5); exchange(2, 6); exchange(3, 7);
    exchange(0, 1); exchange(2, 3); exchange(4, 5); exchange(6, 7);
    exchange(2, 4); exchange(3, 5);
    exchange(1, 4); exchange(3, 6);
    exchange(1, 2); exchange(3, 4); exchange(5, 6);
}
} // namespace detail

inline u32 PagedMesh::regularRing(AdjacencyRow triangles, VertexId vertex, u32 (&next)[8]) const noexcept
{
    // The corner after `vertex` in each triangle, sorted. Branchless: which
    // corner is `vertex` follows no pattern a predictor could learn.
    for (u32& id : next) {
        id = ~0u;
    }
    u32 count = 0;
    triangles.forEach([&](TriangleId triangle) {
        const Triangle& t = m_triangles[triangle];
        const u32 at0 = 0u - static_cast<u32>(t[0] == vertex);
        const u32 at1 = 0u - static_cast<u32>(t[1] == vertex);
        const u32 at2 = 0u - static_cast<u32>(t[2] == vertex);
        next[count++] = (at0 & t[1]) | (at1 & t[2]) | (at2 & t[0]);
    });
    detail::sortEight(next);
    return count;
}

inline u32 PagedMesh::deriveNeighbors(VertexId vertex, u32* out) const noexcept
{
    if (hasRegularFan(vertex)) {
        u32 next[8];
        const u32 count = regularRing(m_vertexTriangles.row(vertex), vertex, next);
        std::copy(next, next + count, out);
        return count;
    }
    return deriveNeighborsGeneral(vertex, out);
}

inline u32 PagedMesh::deriveNeighborsGeneral(VertexId vertex, u32* out) const noexcept
{
    // The other two corners of every incident triangle, kept sorted and
    // unique by insertion: a vertex of valence k gives 2k candidates (each
    // neighbour of an interior vertex twice), too few for a general sort.
    u32 count = 0;
    m_vertexTriangles.row(vertex).forEach([&](TriangleId triangle) {
        for (const VertexId corner : m_triangles[triangle]) {
            if (corner == vertex) {
                continue;
            }
            u32 slot = count;
            while (slot > 0 && out[slot - 1] > corner) {
                --slot;
            }
            if (slot > 0 && out[slot - 1] == corner) {
                continue;
            }
            for (u32 i = count; i > slot; --i) {
                out[i] = out[i - 1];
            }
            out[slot] = corner;
            ++count;
        }
    });
    return count;
}

template <typename Function>
u32 PagedMesh::forEachNeighbor(VertexId vertex, Function&& f) const
{
    if (storesNeighbors()) {
        const AdjacencyRow row = m_neighbors.row(vertex);
        row.forEach(f);
        return row.size();
    }
    const AdjacencyRow triangles = m_vertexTriangles.row(vertex);
    if (hasRegularFan(vertex)) [[likely]] {
        u32 next[8];
        const u32 count = regularRing(triangles, vertex, next);
        for (u32 i = 0; i < count; ++i) {
            f(next[i]);
        }
        return count;
    }
    const u32 candidates = 2 * triangles.size();
    u32 local[kDerivedInline];
    std::vector<u32> heap;
    u32* const ids = candidates <= kDerivedInline ? local : (heap.resize(candidates), heap.data());
    const u32 count = deriveNeighbors(vertex, ids);
    for (u32 i = 0; i < count; ++i) {
        f(ids[i]);
    }
    return count;
}

/// The engine's mesh type. The name is kept so that code written against the
/// earlier flat mesh reads the same.
using Mesh = PagedMesh;

} // namespace se::geo
