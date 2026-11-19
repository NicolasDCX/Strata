#pragma once

#include "Core/JobSystem.h"
#include "Core/Types.h"
#include "Geometry/Mesh.h"

#include <glm/vec3.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace se::sculpt {

/// User-facing parameters shared by every brush.
struct BrushSettings {
    float radius = 0.25f;   ///< World-space radius of influence.
    float strength = 0.5f;  ///< 0..1, meaning is brush specific.
    bool invert = false;    ///< Flips the direction of brushes that have one.
};

/// How the stroke processor feeds a brush.
enum class StrokeMode {
    /// The brush is stamped repeatedly along the stroke at a fixed spacing; each
    /// dab gathers the vertices under the cursor again (Draw, Smooth).
    Dabs,
    /// Vertices are gathered once where the stroke starts and then follow the
    /// pointer; each update is applied to the original positions (Move).
    Grab,
};

/// Elements per parallel chunk in brush loops. Dabs touching fewer vertices
/// than this run on the calling thread, where threading would only add overhead
/// (measured: waking helpers for a dab of ~2,000 vertices made it slower).
inline constexpr std::size_t kBrushParallelGrain = 4096;

/// Reusable working memory lent to brushes by the stroke processor. A brush may
/// resize the arrays freely; capacity is kept between dabs, so the steady state
/// does not allocate.
struct DabScratch {
    std::vector<glm::vec3> points;
    std::vector<glm::vec3> vectors;
    std::vector<float> scalars;
    std::vector<u32> indices;
    std::vector<u32> counts;

    [[nodiscard]] std::size_t memoryBytes() const noexcept
    {
        return (points.capacity() + vectors.capacity()) * sizeof(glm::vec3) + scalars.capacity() * sizeof(float) +
               (indices.capacity() + counts.capacity()) * sizeof(u32);
    }
};

/// Everything a brush needs for one application. Built by StrokeProcessor.
///
/// `positions` is not the mesh: it is the dab's staging view. Every vertex page
/// that holds one of `vertices` is a private copy; all other pages are the
/// mesh's. A brush reads and writes it like the mesh (including several passes
/// that read each other's results) while other threads keep reading the mesh as
/// it was. The stroke processor then computes the normals from the staged
/// positions and publishes positions, normals and BVH together.
struct DabContext {
    const geo::Mesh& mesh;                 ///< Topology and current normals (read only, unchanged during apply()).
    geo::PagedArrayView<glm::vec3> positions; ///< Staging view by vertex id; write only the affected vertices.
    std::span<const u32> vertices;         ///< Affected vertex indices.
    std::span<const float> weights;        ///< Falloff in (0, 1], parallel to `vertices`.
    glm::vec3 center{0.0f};                ///< Brush centre on the surface.
    glm::vec3 normal{0.0f, 1.0f, 0.0f};    ///< Weighted average surface normal under the brush.
    float radius = 0.0f;
    float strength = 0.0f;
    float direction = 1.0f;                ///< +1 or -1 (inverted).
    /// Grab mode only: total pointer displacement since the stroke began and the
    /// positions the affected vertices had at that moment (parallel to `vertices`).
    glm::vec3 dragDelta{0.0f};
    std::span<const glm::vec3> originalPositions;
    DabScratch& scratch;
    /// Optional thread pool for the per-vertex loops (see core::parallelFor).
    /// Null means single threaded. Results must not depend on it.
    core::JobSystem* jobs = nullptr;
};

/// A sculpting brush. One virtual call per dab; the per-vertex loop lives inside
/// apply() so it stays free of indirect calls.
///
/// Threading: `ctx.vertices` holds each vertex once, so a loop that writes only
/// positions[vertices[i]] for the i of its own chunk can run in parallel without
/// locks. Brushes are stateless and const; all working memory is in `ctx.scratch`.
///
/// New brushes are added by deriving from this class and registering the
/// instance in BrushLibrary. Nothing in the Geometry module has to change.
class Brush {
public:
    virtual ~Brush() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual StrokeMode mode() const noexcept = 0;

    /// Deforms `ctx.positions` for the vertices listed in `ctx.vertices`.
    /// Must not touch any other vertex and must not modify topology.
    virtual void apply(const DabContext& ctx) const = 0;

protected:
    Brush() = default;
    Brush(const Brush&) = default;
    Brush& operator=(const Brush&) = default;
};

} // namespace se::sculpt
