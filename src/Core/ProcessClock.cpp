#include "Core/ProcessClock.h"

#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace se::core {

namespace {
const auto g_moduleStart = std::chrono::steady_clock::now();
} // namespace

double processUptimeMs() noexcept
{
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user) != 0) {
        FILETIME now{};
        GetSystemTimePreciseAsFileTime(&now);
        const auto ticks = [](const FILETIME& t) {
            return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        return static_cast<double>(ticks(now) - ticks(creation)) / 10000.0; // 100 ns units
    }
#endif
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_moduleStart).count();
}

} // namespace se::core
