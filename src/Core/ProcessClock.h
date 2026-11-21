#pragma once

namespace se::core {

/// Milliseconds since this process was created (by the operating system's
/// process creation time, so it includes loading the executable and its DLLs).
/// Falls back to the time since this module's static initialisation where the
/// creation time is unavailable.
[[nodiscard]] double processUptimeMs() noexcept;

} // namespace se::core
