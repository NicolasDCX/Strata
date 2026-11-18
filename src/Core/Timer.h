#pragma once

#include <chrono>

namespace se::core {

/// Monotonic stopwatch used for the measurements shown in the status bar and benchmark.
class Stopwatch {
public:
    Stopwatch() noexcept : m_start(Clock::now()) {}

    void reset() noexcept { m_start = Clock::now(); }

    [[nodiscard]] double elapsedMs() const noexcept
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_start;
};

} // namespace se::core
