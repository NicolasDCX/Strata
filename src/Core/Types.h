#pragma once

#include <cstddef>
#include <cstdint>

namespace se {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

/// Sentinel for "no vertex / no triangle / no node".
inline constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

} // namespace se
