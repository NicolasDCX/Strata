#pragma once

#include <cstddef>

namespace se::bench {

/// Heap bytes currently allocated through operator new (se_bench only).
[[nodiscard]] std::size_t heapBytesInUse() noexcept;
/// Highest heapBytesInUse() since the last resetHeapPeak().
[[nodiscard]] std::size_t heapPeakBytes() noexcept;
void resetHeapPeak() noexcept;

} // namespace se::bench
