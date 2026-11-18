#pragma once

#include "Sculpt/Brush.h"

namespace se::sculpt {

/// Raises (or lowers, when inverted) the surface along the average normal under the brush.
class DrawBrush final : public Brush {
public:
    /// Displacement of the brush centre for one dab at strength 1, as a fraction of the radius.
    static constexpr float kGainPerDab = 0.15f;

    [[nodiscard]] std::string_view name() const noexcept override { return "Draw"; }
    [[nodiscard]] StrokeMode mode() const noexcept override { return StrokeMode::Dabs; }
    void apply(const DabContext& ctx) const override;
};

} // namespace se::sculpt
