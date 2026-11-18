#include "Geometry/PagedMesh.h"

#include "Core/Assert.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <numeric>
#include <utility>

namespace se::geo {

// --- PagedAdjacency ------------------------------------------------------------------

void PagedAdjacency::reset(u32 vertexCount, u32 pageShift)
{
    const u32 capacity = 1u << pageShift;
    m_shift = pageShift;
    m_mask = capacity - 1;
    const std::size_t pageCount = (static_cast<std::size_t>(vertexCount) + capacity - 1) >> pageShift;
    m_pages.assign(pageCount, nullptr);
    m_blocks.clear();
    m_blocks.resize(pageCount);
    m_allocatedWords = 0;
    m_wideRows = 0;
    m_compact = false;
}

void PagedAdjacency::assign(std::span<const u32> offsets, std::span<const u32> data, u32 pageShift)
{
    SE_ASSERT(!offsets.empty());
    const u32 vertexCount = static_cast<u32>(offsets.size() - 1);
    const u32 capacity = 1u << pageShift;
    reset(vertexCount, pageShift);
    const std::size_t pageCount = m_pages.size();

    for (std::size_t pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        const u32 first = static_cast<u32>(pageIndex) << pageShift;
        const u32 last = std::min(first + capacity, vertexCount);
        const u32 headerWords = last - first + 1;
        const u32 base = offsets[first];
        const u32 dataWords = offsets[last] - base;

        m_blocks[pageIndex] = std::make_unique_for_overwrite<u32[]>(headerWords + dataWords);
        u32* const block = m_blocks[pageIndex].get();
        for (u32 vertex = first; vertex <= last; ++vertex) {
            // Relative to the block start: the data follows the header.
            block[vertex - first] = headerWords + (offsets[vertex] - base);
        }
        std::copy(data.begin() + base, data.begin() + offsets[last], block + headerWords);
        m_pages[pageIndex] = block;
        m_allocatedWords += headerWords + dataWords;
    }
}

void PagedAdjacency::storeCompactPage(std::size_t pageIndex, std::span<const u32> rowStarts, std::span<const u32> entries)
{
    const u32 rowCount = static_cast<u32>(rowStarts.size() - 1);
    // Block layout in u32 words: [anchor][header per row][rows, as u16 words].
    const u32 headerWords = 1 + rowCount;

    // About the median entry (of every 8th): most rows then lie within 16 bits of it.
    u32 anchor = 0;
    if (!entries.empty()) {
        std::vector<u32> sample;
        sample.reserve(entries.size() / 8 + 1);
        for (std::size_t i = 0; i < entries.size(); i += 8) {
            sample.push_back(entries[i]);
        }
        const auto middle = sample.begin() + static_cast<std::ptrdiff_t>(sample.size() / 2);
        std::nth_element(sample.begin(), middle, sample.end());
        anchor = *middle;
    }
    const auto fits = [anchor](u32 value) {
        const i64 offset = static_cast<i64>(value) - static_cast<i64>(anchor);
        return offset >= -32768 && offset <= 32767;
    };

    std::vector<u32> headers(rowCount);
    std::vector<u16> words; // rows, from the end of the headers (an even u16 position)
    words.reserve(entries.size() + 8);
    const auto alignToU32 = [&words] {
        if (words.size() % 2 != 0) {
            words.push_back(0);
        }
    };
    const auto pushU32 = [&words](u32 value) {
        // Little-endian: the low half first, so the pair reads back as one u32.
        words.push_back(static_cast<u16>(value & 0xFFFFu));
        words.push_back(static_cast<u16>(value >> 16));
    };

    for (u32 row = 0; row < rowCount; ++row) {
        const std::span<const u32> ids = entries.subspan(rowStarts[row], rowStarts[row + 1] - rowStarts[row]);
        const u32 count = static_cast<u32>(ids.size());
        const bool wide = !std::all_of(ids.begin(), ids.end(), fits);
        u32 header = 0;
        if (wide || count >= kCountInRow) {
            alignToU32(); // u32 ids and an in-row count are read as u32
        }
        if (count >= kCountInRow) {
            pushU32(count);
            header = kCountInRow << kCountShift;
        } else {
            header = count << kCountShift;
        }
        const std::size_t start = std::size_t{2} * headerWords + words.size(); // u16 words from the block start
        SE_ASSERT(start <= kOffsetMask);
        header |= static_cast<u32>(start);
        if (wide) {
            header |= kWideBit;
            for (const u32 id : ids) {
                pushU32(id);
            }
            ++m_wideRows;
        } else {
            for (const u32 id : ids) {
                words.push_back(static_cast<u16>(id - anchor)); // two's complement of the signed offset
            }
        }
        headers[row] = header;
    }

    alignToU32();
    const std::size_t dataWords = words.size() / 2;
    m_blocks[pageIndex] = std::make_unique_for_overwrite<u32[]>(headerWords + dataWords);
    u32* const block = m_blocks[pageIndex].get();
    block[0] = anchor;
    std::copy(headers.begin(), headers.end(), block + 1);
    std::memcpy(block + headerWords, words.data(), words.size() * sizeof(u16));
    m_pages[pageIndex] = block;
    m_allocatedWords += headerWords + dataWords;
}

std::size_t PagedAdjacency::memoryBytes() const noexcept
{
    return m_allocatedWords * sizeof(u32) + m_pages.capacity() * sizeof(const u32*) +
           m_blocks.capacity() * sizeof(std::unique_ptr<u32[]>);
}

// --- PagedMesh -----------------------------------------------------------------------

PagedMesh::PagedMesh(std::vector<glm::vec3> positions, std::vector<u32> indices, const MeshLayout& layout)
    : m_layout(layout),
      m_positions(layout.vertexPageShift),
      m_normals(layout.vertexPageShift),
      m_triangles(layout.trianglePageShift)
{
    SE_ASSERT(indices.size() % 3 == 0);
    for (const u32 index : indices) {
        SE_ASSERT(index < positions.size());
    }
    const u32 vertexTotal = static_cast<u32>(positions.size());
    const u32 triangleTotal = static_cast<u32>(indices.size() / 3);

    m_positions.resize(vertexTotal);
    m_positions.copyIn(0, vertexTotal, positions.data());
    positions = {}; // the flat copy is no longer needed
    m_normals.resize(vertexTotal);

    m_triangles.resize(triangleTotal);
    static_assert(sizeof(Triangle) == 3 * sizeof(u32));
    m_triangles.copyIn(0, triangleTotal, reinterpret_cast<const Triangle*>(indices.data()));

    buildTopology(indices);
    indices = {};

    m_pageBounds.resize(vertexPageCount());
    m_pageBoundsStale.assign(vertexPageCount(), u8{1});

    recomputeNormals();
    recomputeBounds();
}

void PagedMesh::buildTopology(std::span<const u32> flatIndices)
{
    if (m_layout.adjacency == AdjacencyLayout::Compact) {
        buildCompactTopology(flatIndices);
        return;
    }
    const u32 vertexTotal = vertexCount();
    const u32 triangleTotal = triangleCount();

    // Both tables are first built as flat compressed rows (transient memory),
    // then split into pages.

    // vertex -> triangles, by counting sort.
    std::vector<u32> triangleOffsets(vertexTotal + 1, 0);
    for (const u32 index : flatIndices) {
        ++triangleOffsets[index + 1];
    }
    std::partial_sum(triangleOffsets.begin(), triangleOffsets.end(), triangleOffsets.begin());
    std::vector<u32> triangleData(flatIndices.size());
    {
        std::vector<u32> cursor(triangleOffsets.begin(), triangleOffsets.end() - 1);
        for (u32 triangle = 0; triangle < triangleTotal; ++triangle) {
            for (u32 corner = 0; corner < 3; ++corner) {
                triangleData[cursor[flatIndices[triangle * 3 + corner]]++] = triangle;
            }
        }
    }

    m_vertexTriangles.assign(triangleOffsets, triangleData, m_layout.vertexPageShift);
    if (!storesNeighbors()) {
        classifyFans(); // neighbours are derived on demand from the incident triangles
        return;
    }

    // vertex -> vertices: the other corners of every incident triangle, deduplicated.
    std::vector<u32> neighborOffsets(vertexTotal + 1, 0);
    std::vector<u32> neighborData;
    neighborData.reserve(flatIndices.size());
    std::vector<u32> ring;
    for (u32 vertex = 0; vertex < vertexTotal; ++vertex) {
        ring.clear();
        for (u32 i = triangleOffsets[vertex]; i < triangleOffsets[vertex + 1]; ++i) {
            const u32 triangle = triangleData[i];
            for (u32 corner = 0; corner < 3; ++corner) {
                const u32 other = flatIndices[triangle * 3 + corner];
                if (other != vertex) {
                    ring.push_back(other);
                }
            }
        }
        std::sort(ring.begin(), ring.end());
        ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
        neighborData.insert(neighborData.end(), ring.begin(), ring.end());
        neighborOffsets[vertex + 1] = static_cast<u32>(neighborData.size());
    }

    m_neighbors.assign(neighborOffsets, neighborData, m_layout.vertexPageShift);
}

void PagedMesh::buildCompactTopology(std::span<const u32> flatIndices)
{
    const u32 vertexTotal = vertexCount();
    const u32 triangleTotal = triangleCount();
    const u32 pageShift = m_layout.vertexPageShift;

    // vertex -> triangles: a flat counting sort (transient), encoded page by page.
    {
        std::vector<u32> triangleOffsets(vertexTotal + 1, 0);
        for (const u32 index : flatIndices) {
            ++triangleOffsets[index + 1];
        }
        std::partial_sum(triangleOffsets.begin(), triangleOffsets.end(), triangleOffsets.begin());
        std::vector<u32> triangleData(flatIndices.size());
        {
            std::vector<u32> cursor(triangleOffsets.begin(), triangleOffsets.end() - 1);
            for (u32 triangle = 0; triangle < triangleTotal; ++triangle) {
                for (u32 corner = 0; corner < 3; ++corner) {
                    triangleData[cursor[flatIndices[triangle * 3 + corner]]++] = triangle;
                }
            }
        }
        m_vertexTriangles.assignCompact(vertexTotal, pageShift, [&](u32 vertex, std::vector<u32>& out) {
            out.assign(triangleData.begin() + triangleOffsets[vertex], triangleData.begin() + triangleOffsets[vertex + 1]);
        });
    } // the flat table is freed before the neighbours are built
    if (!storesNeighbors()) {
        classifyFans(); // neighbours are derived on demand from the incident triangles
        return;
    }

    // vertex -> vertices: the other corners of every incident triangle,
    // deduplicated, read back from the compact table just built.
    m_neighbors.assignCompact(vertexTotal, pageShift, [&](u32 vertex, std::vector<u32>& ring) {
        ring.clear();
        for (const u32 triangle : m_vertexTriangles.row(vertex)) {
            for (u32 corner = 0; corner < 3; ++corner) {
                const u32 other = flatIndices[triangle * 3 + corner];
                if (other != vertex) {
                    ring.push_back(other);
                }
            }
        }
        std::sort(ring.begin(), ring.end());
        ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
    });
}

void PagedMesh::classifyFans()
{
    // A fan is regular when the corners after the vertex (one per triangle)
    // are all different and are the same set as the corners before it. Then
    // the other corners of the fan are exactly that set, so the fast
    // derivation (regularRing) gives the general result, and always will: the
    // topology never changes. Closed, consistently wound disks pass; borders,
    // flipped triangles and non-manifold fans take the general path.
    const u32 vertexTotal = vertexCount();
    m_regularFan.assign((static_cast<std::size_t>(vertexTotal) + 63) / 64, 0);
    for (u32 vertex = 0; vertex < vertexTotal; ++vertex) {
        const AdjacencyRow triangles = m_vertexTriangles.row(vertex);
        if (triangles.empty() || triangles.size() > kRegularFanMax) {
            continue;
        }
        u32 next[8];
        const u32 count = regularRing(triangles, vertex, next);
        u32 prev[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
        u32 slot = 0;
        u32 uses = 0;
        triangles.forEach([&](TriangleId triangle) {
            const Triangle& t = m_triangles[triangle];
            prev[slot++] = t[0] == vertex ? t[2] : (t[1] == vertex ? t[0] : t[1]);
            uses += static_cast<u32>(t[0] == vertex) + static_cast<u32>(t[1] == vertex) + static_cast<u32>(t[2] == vertex);
        });
        detail::sortEight(prev);
        bool regular = uses == count; // the vertex is exactly one corner of each triangle
        for (u32 i = 0; i < count; ++i) {
            regular = regular && next[i] == prev[i] && next[i] != vertex && (i == 0 || next[i - 1] < next[i]);
        }
        if (regular) {
            m_regularFan[vertex >> 6] |= u64{1} << (vertex & 63u);
        }
    }
}

u32 PagedMesh::generalFanCount() const noexcept
{
    if (storesNeighbors()) {
        return 0;
    }
    u32 regular = 0;
    for (const u64 word : m_regularFan) {
        regular += static_cast<u32>(std::popcount(word));
    }
    return vertexCount() - regular;
}

NeighborRing PagedMesh::neighbors(VertexId vertex) const
{
    if (storesNeighbors()) {
        return NeighborRing(m_neighbors.row(vertex));
    }
    std::vector<u32> ids(2 * static_cast<std::size_t>(m_vertexTriangles.row(vertex).size()));
    ids.resize(deriveNeighbors(vertex, ids.data()));
    return NeighborRing(std::span<const u32>(ids));
}

void PagedMesh::copyIndices(TriangleId first, u32 count, u32* destination) const noexcept
{
    m_triangles.copyOut(first, count, reinterpret_cast<Triangle*>(destination));
}

Aabb PagedMesh::vertexPageBounds(u32 page) const
{
    if (m_pageBoundsStale[page] != 0) {
        Aabb bounds;
        for (const glm::vec3& position : m_positions.page(page)) {
            bounds.expand(position);
        }
        m_pageBounds[page] = bounds;
        m_pageBoundsStale[page] = 0;
    }
    return m_pageBounds[page];
}

void PagedMesh::invalidatePageBounds(const DirtyVertexSet& modified)
{
    // `modified` may use any page size: map its vertex runs onto our pages.
    modified.forEachRun([this](u32 first, u32 count) {
        const u32 lastPage = (first + count - 1) >> m_layout.vertexPageShift;
        for (u32 page = first >> m_layout.vertexPageShift; page <= lastPage; ++page) {
            m_pageBoundsStale[page] = 1;
        }
    });
}

void PagedMesh::recomputeBounds() noexcept
{
    Aabb bounds;
    for (std::size_t page = 0; page < m_positions.pageCount(); ++page) {
        for (const glm::vec3& position : m_positions.page(page)) {
            bounds.expand(position);
        }
    }
    m_bounds = bounds;
}

void PagedMesh::recomputeNormals() noexcept
{
    for (std::size_t page = 0; page < m_normals.pageCount(); ++page) {
        std::ranges::fill(m_normals.page(page), glm::vec3(0.0f));
    }
    const u32 triangleTotal = triangleCount();
    for (u32 id = 0; id < triangleTotal; ++id) {
        const Triangle& corners = m_triangles[id];
        // The cross product length is twice the triangle area: area weighting for free.
        const glm::vec3 faceNormal = glm::cross(m_positions[corners[1]] - m_positions[corners[0]],
                                                m_positions[corners[2]] - m_positions[corners[0]]);
        m_normals[corners[0]] += faceNormal;
        m_normals[corners[1]] += faceNormal;
        m_normals[corners[2]] += faceNormal;
    }
    for (std::size_t page = 0; page < m_normals.pageCount(); ++page) {
        for (glm::vec3& normal : m_normals.page(page)) {
            const float lengthSquared = glm::dot(normal, normal);
            normal = lengthSquared > 1e-30f ? normal / std::sqrt(lengthSquared) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }
}

void PagedMesh::recomputeNormals(std::span<const VertexId> vertices) noexcept
{
    // Views hold the page tables in locals, which keeps them in registers
    // across the loop instead of being reloaded through `this`.
    const PagedArrayView<const glm::vec3> positions = m_positions.view();
    const PagedArrayView<const Triangle> triangles = m_triangles.view();
    const PagedArrayView<glm::vec3> normals = m_normals.view();

    for (const VertexId vertex : vertices) {
        glm::vec3 sum(0.0f);
        m_vertexTriangles.row(vertex).forEach([&](TriangleId id) {
            const Triangle& corners = triangles[id];
            const glm::vec3& p0 = positions[corners[0]];
            sum += glm::cross(positions[corners[1]] - p0, positions[corners[2]] - p0);
        });
        const float lengthSquared = glm::dot(sum, sum);
        if (lengthSquared > 1e-30f) {
            normals[vertex] = sum / std::sqrt(lengthSquared);
        }
        // Otherwise keep the previous normal: a fully collapsed fan has no orientation.
    }
}

void PagedMesh::computeNormals(std::span<const VertexId> vertices, PagedArrayView<const glm::vec3> positions,
                               std::span<glm::vec3> out) const noexcept
{
    const PagedArrayView<const Triangle> triangles = m_triangles.view();
    const PagedArrayView<const glm::vec3> normals = m_normals.view();

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const VertexId vertex = vertices[i];
        glm::vec3 sum(0.0f);
        m_vertexTriangles.row(vertex).forEach([&](TriangleId id) {
            const Triangle& corners = triangles[id];
            const glm::vec3& p0 = positions[corners[0]];
            sum += glm::cross(positions[corners[1]] - p0, positions[corners[2]] - p0);
        });
        const float lengthSquared = glm::dot(sum, sum);
        out[i] = lengthSquared > 1e-30f ? sum / std::sqrt(lengthSquared) : normals[vertex];
    }
}

bool PagedMesh::validate(std::string* reason) const
{
    const auto fail = [reason](std::string message) {
        if (reason != nullptr) {
            *reason = std::move(message);
        }
        return false;
    };

    if (m_normals.size() != m_positions.size()) {
        return fail("normal count differs from position count");
    }
    const u32 vertexTotal = vertexCount();
    const u32 triangleTotal = triangleCount();
    for (u32 id = 0; id < triangleTotal; ++id) {
        const Triangle& corners = m_triangles[id];
        if (corners[0] >= vertexTotal || corners[1] >= vertexTotal || corners[2] >= vertexTotal) {
            return fail(std::format("triangle {} references a vertex out of range", id));
        }
        if (corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2]) {
            return fail(std::format("triangle {} repeats a vertex", id));
        }
    }
    const u32 expectedVertexPages = (vertexTotal + m_layout.vertexPageCapacity() - 1) >> m_layout.vertexPageShift;
    if (vertexPageCount() != expectedVertexPages || m_normals.pageCount() != expectedVertexPages) {
        return fail("vertex page count does not match the vertex count");
    }
    return true;
}

MeshMemory PagedMesh::memory() const noexcept
{
    MeshMemory memory;
    memory.positions = m_positions.memoryBytes();
    memory.normals = m_normals.memoryBytes();
    memory.triangles = m_triangles.memoryBytes();
    memory.adjacencyNeighbors = m_neighbors.memoryBytes() + m_regularFan.capacity() * sizeof(u64);
    memory.adjacencyIncidentTriangles = m_vertexTriangles.memoryBytes();
    memory.adjacency = memory.adjacencyNeighbors + memory.adjacencyIncidentTriangles;
    memory.bookkeeping = m_pageBounds.capacity() * sizeof(Aabb) + m_pageBoundsStale.capacity();
    return memory;
}

} // namespace se::geo
