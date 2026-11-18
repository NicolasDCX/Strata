#include "Core/Log.h"

#include "Core/Assert.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace se::core {

namespace {

std::atomic<LogLevel> g_minLevel{LogLevel::Info};
std::mutex g_logMutex;

constexpr std::string_view levelTag(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info ";
    case LogLevel::Warning: return "warn ";
    case LogLevel::Error: return "error";
    }
    return "?";
}

} // namespace

void setLogLevel(LogLevel level) noexcept
{
    g_minLevel.store(level, std::memory_order_relaxed);
}

void logMessage(LogLevel level, std::string_view message)
{
    if (level < g_minLevel.load(std::memory_order_relaxed)) {
        return;
    }
    const std::string_view tag = levelTag(level);
    const std::scoped_lock lock(g_logMutex);
    std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(tag.size()), tag.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

void assertFail(const char* expression, const char* file, int line) noexcept
{
    std::fprintf(stderr, "[fatal] assertion failed: %s (%s:%d)\n", expression, file, line);
    std::fflush(stderr);
    std::abort();
}

} // namespace se::core
