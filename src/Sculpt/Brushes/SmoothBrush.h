#pragma once

#include "Sculpt/Brush.h"

namespace se::sculpt {

/// Smooths the surface under the brush. Adapts to the mesh density:
///
/// - Sparse region (few vertices per kernel cell): classic Laplacian relaxation,
///   each vertex moves towards the average of its edge neighbours.
/// - Dense region: edge-neighbour averaging only reaches a few edges, which on a
///   dense mesh is a tiny distance, so broad shapes would barely change. Here
///   the brush first filters at a fixed fraction of its own radius: vertices are
///   binned into a grid, and each vertex moves along the locally averaged normal
///   towards the locally averaged surface. The reach of that filter is set in
///   world units, so it behaves the same whatever the triangle count. One
///   Laplacian pass then removes the remaining fine noise.
///
/// Known limitations: smoothing flattens curvature at the scale of the brush (a
/// sphere slowly shrinks under it), and the dense-mode filter averages in space,
/// so two surfaces closer together than the kernel can influence each other when
/// they face the same way.
class SmoothBrush final : public Brush {
public:
    /// Laplacian passes performed by one dab at strength 1 in sparse regions.
    static constexpr int kMaxPassesPerDab = 4;
    /// Kernel cell size of the dense-mode filter, as a fraction of the brush radius.
    static constexpr float kKernelFraction = 0.3f;
    /// Average vertices per occupied kernel cell from which a region counts as dense.
    static constexpr float kDenseVerticesPerCell = 12.0f;
    /// With helper threads, dabs of at least this many vertices bin them in
    /// parallel: each cell still sums its vertices in the order of the list.
    static constexpr std::size_t kParallelBinMinVertices = 32768;
    /// Vertices per block of the parallel binning (one cell histogram each).
    static constexpr std::size_t kBinBlock = 16384;
    /// Bin in parallel when allowed (default). Off: always the serial loop.
    /// Same results bit for bit; a process-wide switch for tests and benchmarks.
    static void setParallelBinning(bool enabled) noexcept;
    [[nodiscard]] static bool parallelBinning() noexcept;
    /// Dense-mode blend of the cell averages. On (default): quadratic B-spline
    /// weights over the 3x3x3 nearest cells, continuous in value and slope.
    /// Off: the former trilinear blend of the 2x2x2 nearest cells, whose slope
    /// jumps on every plane through cell centres; on a curved surface each cell
    /// then becomes a shallow pillow with a crease along its border, and as the
    /// grid follows the dab centre, a stroke stacks those creases into angular
    /// bands. A process-wide switch for comparisons.
    static void setQuadraticKernel(bool enabled) noexcept;
    [[nodiscard]] static bool quadraticKernel() noexcept;

    [[nodiscard]] std::string_view name() const noexcept override { return "Smooth"; }
    [[nodiscard]] StrokeMode mode() const noexcept override { return StrokeMode::Dabs; }
    void apply(const DabContext& ctx) const override;

    /// Profiling: wall time of each stage, summed over the dabs since
    /// resetSteps(): clear the grid, bin the vertices, count occupied cells,
    /// kernel targets, kernel commit, Laplacian targets, Laplacian commit.
    /// Written by the applying thread only.
    struct Steps {
        double ms[7] = {};
        u64 calls = 0;
        u64 denseCalls = 0;
        u64 laplacianPasses = 0;
        u64 parallelBins = 0;
    };
    [[nodiscard]] static Steps steps() noexcept;
    static void resetSteps() noexcept;
};

} // namespace se::sculpt
