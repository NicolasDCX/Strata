#pragma once

#include "Core/JobSystem.h"
#include "Core/Types.h"
#include "Geometry/Mesh.h"

#include <algorithm>
#include <span>
#include <vector>

namespace se::render {

/// A contiguous range of the index buffer, in indices (3 per triangle).
struct IndexRun {
    u32 firstIndex = 0;
    u32 indexCount = 0;

    friend bool operator==(const IndexRun&, const IndexRun&) = default;
};

/// What the renderer needs to know about spatial chunks: for every chunk, its
/// triangle range and the GPU vertex pages its triangles reference.
///
/// A chunk is a run of 2^chunkTriangleShift consecutive triangle ids (the same
/// definition as geo::ChunkedBvh). Its triangles reference vertices by global
/// id, and those vertices can be in any GPU vertex page. A page used by several
/// chunks appears in each of their lists; it exists once on the GPU. Nothing is
/// duplicated at chunk borders.
///
/// Topology is fixed, so the map is built once. No Vulkan code.
class ChunkPageMap {
public:
    /// `gpuPageShift`: 2^n vertices per GPU vertex page.
    void build(const geo::Mesh& mesh, u32 chunkTriangleShift, u32 gpuPageShift, core::JobSystem* jobs = nullptr)
    {
        m_chunkShift = std::min(chunkTriangleShift, 31u);
        m_pageShift = gpuPageShift;
        m_triangleCount = mesh.triangleCount();
        m_vertexPageCount = (mesh.vertexCount() + (1u << gpuPageShift) - 1u) >> gpuPageShift;
        const u32 capacity = chunkTriangleCapacity();
        const u32 chunkTotal = static_cast<u32>((static_cast<u64>(m_triangleCount) + capacity - 1u) >> m_chunkShift);

        // Per chunk: the sorted, unique pages of its triangles' corners.
        std::vector<std::vector<u32>> lists(chunkTotal);
        core::parallelFor(jobs, chunkTotal, 16, [&](std::size_t begin, std::size_t end) {
            for (std::size_t chunk = begin; chunk < end; ++chunk) {
                std::vector<u32>& pages = lists[chunk];
                const u32 first = static_cast<u32>(chunk) << m_chunkShift;
                const u32 last = std::min(m_triangleCount, first + capacity);
                pages.reserve(static_cast<std::size_t>(last - first) * 3);
                for (u32 triangle = first; triangle < last; ++triangle) {
                    for (const geo::VertexId vertex : mesh.triangle(triangle)) {
                        pages.push_back(vertex >> gpuPageShift);
                    }
                }
                std::sort(pages.begin(), pages.end());
                pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
            }
        });

        m_offsets.assign(static_cast<std::size_t>(chunkTotal) + 1, 0);
        m_pages.clear();
        for (u32 chunk = 0; chunk < chunkTotal; ++chunk) {
            m_pages.insert(m_pages.end(), lists[chunk].begin(), lists[chunk].end());
            m_offsets[chunk + 1] = static_cast<u32>(m_pages.size());
        }
    }

    [[nodiscard]] u32 chunkCount() const noexcept { return m_offsets.empty() ? 0u : static_cast<u32>(m_offsets.size() - 1); }
    [[nodiscard]] u32 chunkTriangleShift() const noexcept { return m_chunkShift; }
    [[nodiscard]] u32 chunkTriangleCapacity() const noexcept { return 1u << m_chunkShift; }
    [[nodiscard]] u32 gpuPageShift() const noexcept { return m_pageShift; }
    [[nodiscard]] u32 vertexPageCount() const noexcept { return m_vertexPageCount; }
    [[nodiscard]] u32 triangleCount() const noexcept { return m_triangleCount; }

    [[nodiscard]] u32 firstTriangle(u32 chunk) const noexcept { return chunk << m_chunkShift; }
    [[nodiscard]] u32 chunkTriangles(u32 chunk) const noexcept
    {
        return std::min(chunkTriangleCapacity(), m_triangleCount - firstTriangle(chunk));
    }
    /// The chunk's range in an index buffer laid out in triangle id order.
    [[nodiscard]] IndexRun indexRun(u32 chunk) const noexcept { return {firstTriangle(chunk) * 3u, chunkTriangles(chunk) * 3u}; }

    /// GPU vertex pages referenced by the chunk's triangles (sorted, unique).
    [[nodiscard]] std::span<const u32> vertexPages(u32 chunk) const noexcept
    {
        return {m_pages.data() + m_offsets[chunk], m_offsets[chunk + 1] - m_offsets[chunk]};
    }

    /// Sum of all chunks' page list lengths; exceeds vertexPageCount() by the
    /// number of times pages are shared between chunks.
    [[nodiscard]] std::size_t pageReferenceCount() const noexcept { return m_pages.size(); }

    [[nodiscard]] std::size_t memoryBytes() const noexcept { return (m_pages.capacity() + m_offsets.capacity()) * sizeof(u32); }

private:
    std::vector<u32> m_offsets; // chunkCount + 1
    std::vector<u32> m_pages;
    u32 m_chunkShift = 12;
    u32 m_pageShift = 10;
    u32 m_triangleCount = 0;
    u32 m_vertexPageCount = 0;
};

/// Sorts index ranges by start and merges each one that begins exactly where
/// the previous one ends, so adjacent ranges are drawn with one call. Ranges
/// must not overlap. Drawing order becomes buffer order.
inline void mergeIndexRuns(std::vector<IndexRun>& runs)
{
    if (runs.size() < 2) {
        return;
    }
    std::sort(runs.begin(), runs.end(), [](const IndexRun& a, const IndexRun& b) { return a.firstIndex < b.firstIndex; });
    std::size_t last = 0;
    for (std::size_t i = 1; i < runs.size(); ++i) {
        if (runs[last].firstIndex + runs[last].indexCount == runs[i].firstIndex) {
            runs[last].indexCount += runs[i].indexCount;
        } else {
            runs[++last] = runs[i];
        }
    }
    runs.resize(last + 1);
}

} // namespace se::render
