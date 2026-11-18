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

    [[nodiscard]] std::string_view name() const noexcept override { return "Smooth"; }
    [[nodiscard]] StrokeMode mode() const noexcept override { return StrokeMode::Dabs; }
    void apply(const DabContext& ctx) const override;
};

} // namespace se::sculpt
