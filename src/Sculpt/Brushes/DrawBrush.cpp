#include "Sculpt/Brushes/DrawBrush.h"

namespace se::sculpt {

void DrawBrush::apply(const DabContext& ctx) const
{
    // Using one direction for the whole dab (instead of each vertex's own normal)
    // keeps the stroke from ballooning sideways on curved surfaces.
    const glm::vec3 offset = ctx.normal * (ctx.direction * ctx.strength * ctx.radius * kGainPerDab);
    core::parallelFor(ctx.jobs, ctx.vertices.size(), kBrushParallelGrain, [&ctx, offset](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            ctx.positions[ctx.vertices[i]] += offset * ctx.weights[i];
        }
    });
}

} // namespace se::sculpt
