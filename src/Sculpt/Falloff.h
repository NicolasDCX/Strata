#pragma once

namespace se::sculpt {

/// Smooth brush falloff. Input is (distance / radius)^2 in [0, 1]; output is 1 at
/// the centre and reaches 0 with zero slope at the rim, so dabs leave no crease.
[[nodiscard]] constexpr float smoothFalloff(float normalizedDistanceSquared) noexcept
{
    const float k = 1.0f - normalizedDistanceSquared;
    return k <= 0.0f ? 0.0f : k * k;
}

} // namespace se::sculpt
