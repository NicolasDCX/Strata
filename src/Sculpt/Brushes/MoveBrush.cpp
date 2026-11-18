#include "Sculpt/Brushes/MoveBrush.h"

namespace se::sculpt {

void MoveBrush::apply(const DabContext& ctx) const
{
    // Always applied to the positions captured when the stroke started, so the
    // result depends only on the current pointer position, not on its history.
    const glm::vec3 offset = ctx.dragDelta * ctx.strength;
    core::parallelFor(ctx.jobs, ctx.vertices.size(), kBrushParallelGrain, [&ctx, offset](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            ctx.positions[ctx.vertices[i]] = ctx.originalPositions[i] + offset * ctx.weights[i];
        }
    });
}

} // namespace se::sculpt
