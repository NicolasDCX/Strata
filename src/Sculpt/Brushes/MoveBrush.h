#pragma once

#include "Sculpt/Brush.h"

namespace se::sculpt {

/// Drags the region grabbed at the start of the stroke along with the pointer.
/// At strength 1 the brush centre follows the pointer exactly.
class MoveBrush final : public Brush {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "Move"; }
    [[nodiscard]] StrokeMode mode() const noexcept override { return StrokeMode::Grab; }
    void apply(const DabContext& ctx) const override;
};

} // namespace se::sculpt
