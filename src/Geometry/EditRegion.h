#pragma once

#include "Core/JobSystem.h"
#include "Core/Types.h"
#include "Geometry/Mesh.h"

#include <span>
#include <vector>

namespace se::geo {

/// What one edit (a dab, an undo or redo step) invalidates, found with a
/// single walk over the incident-triangle rows of the moved vertices:
/// - triangles(): every triangle with a moved corner, each once, in the order
///   of first appearance when the moved vertices are visited in order and each
///   row in ascending order. That is the order in which ChunkedBvh::refit
///   would mark them, so a refit from this list is bit-identical to one from
///   the moved vertices.
/// - vertices(): the moved vertices, then every other corner of those
///   triangles (their one-ring), each once. These are the vertices whose normal
///   may have changed; it is the same set as moved + neighbours.
///
/// Both the normal update and the BVH refit read these lists, so the dab walks
/// the adjacency once instead of twice. Scratch storage (two bitsets, one bit
/// per vertex and per triangle, and the two lists) is kept between calls; the
/// bits are cleared after each collect(), so the steady state neither
/// allocates nor clears whole arrays.
///
/// With a job system that has helpers, edits of at least kParallelMinVertices
/// moved vertices are walked in parallel and give the same two lists in the
/// same order (see collectParallel). Smaller edits, and runs without helpers,
/// take the serial walk.
class EditRegion {
public:
    /// Moved vertices from which collect() walks in parallel (when given helpers).
    static constexpr std::size_t kParallelMinVertices = 16384;
    /// Moved vertices per task in the parallel triangle walk.
    static constexpr std::size_t kParallelVertexBlock = 2048;
    /// Region triangles per task in the parallel one-ring walk.
    static constexpr std::size_t kParallelTriangleBlock = 4096;

    /// Replaces the lists with those of `movedVertices`. Not thread-safe; a
    /// job system with helpers lets large edits run on them.
    void collect(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem* jobs = nullptr);

    /// Whether the last collect() took the parallel walk.
    [[nodiscard]] bool lastWasParallel() const noexcept { return m_lastParallel; }
    /// Bytes of the per-vertex order stamps of the parallel walk, summed over
    /// the threads that have run it (one array per thread, shared by every
    /// EditRegion on that thread, 4 bytes per vertex of the largest mesh).
    [[nodiscard]] static std::size_t parallelStampBytes() noexcept;

    [[nodiscard]] std::span<const TriangleId> triangles() const noexcept { return {m_triangles.data(), m_triangleCount}; }
    [[nodiscard]] std::span<const VertexId> vertices() const noexcept { return {m_vertices.data(), m_vertexCount}; }

    /// Bytes of scratch storage held (bitsets, list capacity and the parallel
    /// walk's per-block lists; not the per-thread stamps, see parallelStampBytes).
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

private:
    void collectSerial(const Mesh& mesh, std::span<const VertexId> movedVertices);
    void collectParallel(const Mesh& mesh, std::span<const VertexId> movedVertices, core::JobSystem& jobs);

    /// Per task of the parallel walk: what it found, in order.
    struct Block {
        std::vector<u32> moved;     ///< moved vertices at their first occurrence
        std::vector<u32> triangles; ///< triangles whose earliest moved corner is in this block
        std::vector<u32> ring;      ///< one-ring vertices first seen in this block's triangles
    };
    std::vector<Block> m_blocks;
    std::vector<std::size_t> m_offsets;
    bool m_lastParallel = false;

    std::vector<u64> m_triangleBits;
    std::vector<u64> m_vertexBits;
    std::vector<TriangleId> m_triangles;
    std::vector<VertexId> m_vertices;
    std::size_t m_triangleCount = 0;
    std::size_t m_vertexCount = 0;
};

} // namespace se::geo
