// Counts heap bytes in use and their peak, for the construction-memory figures of
// `se_bench adjacency`. Replaces the global allocation functions of se_bench only
// (the engine itself is not affected). Every block carries a 16-byte header with
// its size, so the counts are exact whatever the allocator rounds up to.

#include "AllocTracker.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::size_t> g_current{0};
std::atomic<std::size_t> g_peak{0};

constexpr std::size_t kHeader = 16;

void* allocate(std::size_t size)
{
    void* const raw = std::malloc(size + kHeader);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    *static_cast<std::size_t*>(raw) = size;
    const std::size_t now = g_current.fetch_add(size, std::memory_order_relaxed) + size;
    std::size_t peak = g_peak.load(std::memory_order_relaxed);
    while (now > peak && !g_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
    return static_cast<char*>(raw) + kHeader;
}

void release(void* pointer) noexcept
{
    if (pointer == nullptr) {
        return;
    }
    void* const raw = static_cast<char*>(pointer) - kHeader;
    g_current.fetch_sub(*static_cast<std::size_t*>(raw), std::memory_order_relaxed);
    std::free(raw);
}

void* allocateAligned(std::size_t size, std::size_t alignment)
{
    // Over-allocate, align, and keep the pointer returned by allocate() just below.
    char* const base = static_cast<char*>(allocate(size + alignment + sizeof(void*)));
    std::size_t address = reinterpret_cast<std::size_t>(base + sizeof(void*));
    address = (address + alignment - 1) & ~(alignment - 1);
    reinterpret_cast<void**>(address)[-1] = base;
    return reinterpret_cast<void*>(address);
}

void releaseAligned(void* pointer) noexcept
{
    if (pointer != nullptr) {
        release(static_cast<void**>(pointer)[-1]);
    }
}

} // namespace

namespace se::bench {

std::size_t heapBytesInUse() noexcept { return g_current.load(std::memory_order_relaxed); }
std::size_t heapPeakBytes() noexcept { return g_peak.load(std::memory_order_relaxed); }
void resetHeapPeak() noexcept { g_peak.store(g_current.load(std::memory_order_relaxed), std::memory_order_relaxed); }

} // namespace se::bench

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment) { return allocateAligned(size, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer) noexcept { release(pointer); }
void operator delete[](void* pointer) noexcept { release(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { releaseAligned(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { releaseAligned(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { releaseAligned(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { releaseAligned(pointer); }
