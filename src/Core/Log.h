#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace se::core {

enum class LogLevel { Debug, Info, Warning, Error };

/// Messages below this level are dropped. Defaults to Info.
void setLogLevel(LogLevel level) noexcept;

/// Writes one line to stderr. Thread-safe.
void logMessage(LogLevel level, std::string_view message);

template <typename... Args>
void logDebug(std::format_string<Args...> fmt, Args&&... args)
{
    logMessage(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args)
{
    logMessage(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void logWarning(std::format_string<Args...> fmt, Args&&... args)
{
    logMessage(LogLevel::Warning, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void logError(std::format_string<Args...> fmt, Args&&... args)
{
    logMessage(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace se::core
