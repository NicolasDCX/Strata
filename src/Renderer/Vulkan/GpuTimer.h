#pragma once

#include "Renderer/Vulkan/VulkanContext.h"

#include <optional>
#include <vector>

namespace se::render {

/// Measures how long the GPU spends on a frame, with timestamp queries.
///
/// CPU-side timing only covers recording commands; with vertical sync the time
/// between frames says nothing about cost either. Timestamps written at the
/// start and end of a frame's command buffer give the GPU's own duration.
///
/// Each frame slot has its own pair of queries. A slot's result is read when
/// the slot is used again, at which point its previous frame has finished, so
/// reading never waits.
class GpuTimer {
public:
    GpuTimer() noexcept = default;
    ~GpuTimer() { destroy(); }
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    /// Returns false (and stays disabled) when the queue family has no timestamps.
    bool create(const VulkanContext& context, u32 queueFamilyIndex, int frameSlots);
    void destroy() noexcept;

    [[nodiscard]] bool available() const noexcept { return m_pool != VK_NULL_HANDLE; }

    /// Call first in a frame, outside a render pass. Returns the GPU time in
    /// milliseconds of the frame that last used `frameSlot`, if there was one.
    std::optional<double> beginFrame(VkCommandBuffer commandBuffer, int frameSlot);
    /// Call last in a frame, outside a render pass.
    void endFrame(VkCommandBuffer commandBuffer, int frameSlot);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_functions = nullptr;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    double m_nanosecondsPerTick = 1.0;
    u64 m_validMask = ~0ull;
    std::vector<u8> m_slotWritten; // per frame slot: queries hold a finished frame
};

} // namespace se::render
