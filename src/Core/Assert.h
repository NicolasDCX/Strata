#pragma once

namespace se::core {

[[noreturn]] void assertFail(const char* expression, const char* file, int line) noexcept;

} // namespace se::core

/// Always-on invariant check. Keep it out of per-vertex loops.
#define SE_ASSERT(condition)                                              \
    do {                                                                  \
        if (!(condition)) [[unlikely]] {                                  \
            ::se::core::assertFail(#condition, __FILE__, __LINE__);       \
        }                                                                 \
    } while (false)
