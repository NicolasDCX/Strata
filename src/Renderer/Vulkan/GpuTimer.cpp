#include "Renderer/Vulkan/GpuTimer.h"

#include <array>

namespace se::render {

bool GpuTimer::create(const VulkanContext& context, u32 queueFamilyIndex, int frameSlots)
{
    destroy();

    // The number of valid timestamp bits is a property of the queue family; 0 means unsupported.
    uint32_t familyCount = 0;
    context.instanceFunctions->vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    context.instanceFunctions->vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &familyCount,
                                                                        families.data());
    if (queueFamilyIndex >= familyCount || families[queueFamilyIndex].timestampValidBits == 0) {
        return false;
    }
    const uint32_t validBits = families[queueFamilyIndex].timestampValidBits;
    m_validMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1ull);

    VkPhysicalDeviceProperties properties{};
    context.instanceFunctions->vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
    m_nanosecondsPerTick = static_cast<double>(properties.limits.timestampPeriod);

    m_device = context.device;
    m_functions = context.deviceFunctions;
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = static_cast<uint32_t>(frameSlots) * 2u;
    if (m_functions->vkCreateQueryPool(m_device, &info, nullptr, &m_pool) != VK_SUCCESS) {
        m_pool = VK_NULL_HANDLE;
        return false;
    }
    m_slotWritten.assign(static_cast<std::size_t>(frameSlots), u8{0});
    return true;
}

void GpuTimer::destroy() noexcept
{
    if (m_functions != nullptr && m_pool != VK_NULL_HANDLE) {
        m_functions->vkDestroyQueryPool(m_device, m_pool, nullptr);
    }
    m_pool = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_functions = nullptr;
    m_slotWritten.clear();
}

std::optional<double> GpuTimer::beginFrame(VkCommandBuffer commandBuffer, int frameSlot)
{
    if (!available()) {
        return std::nullopt;
    }
    const uint32_t first = static_cast<uint32_t>(frameSlot) * 2u;
    std::optional<double> previous;
    if (m_slotWritten[static_cast<std::size_t>(frameSlot)] != 0) {
        // The frame that wrote these queries has completed (the window waited
        // for it before reusing the slot), so this does not block.
        std::array<u64, 2> ticks{};
        const VkResult result = m_functions->vkGetQueryPoolResults(m_device, m_pool, first, 2, sizeof(ticks), ticks.data(),
                                                                   sizeof(u64), VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS) {
            const u64 elapsed = (ticks[1] - ticks[0]) & m_validMask;
            previous = static_cast<double>(elapsed) * m_nanosecondsPerTick * 1e-6;
        }
    }
    // QVulkanWindow makes the frame wait for the swapchain image only at the
    // colour-output stage. A start timestamp at the top of the pipe can be
    // written before that wait ends, so the measured time could include it. The
    // start timestamp is therefore written at the colour-output stage, and a
    // barrier holds all later work until it is written, so the interval covers
    // only this frame's rendering. (Moving it made no measurable difference on
    // the test machine: the floor of about 4 ms seen for light views came from
    // the GPU running at power-saving clocks, see RenderSettings::drawRepeat.)
    m_functions->vkCmdResetQueryPool(commandBuffer, m_pool, first, 2);
    m_functions->vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, m_pool, first);
    m_functions->vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    return previous;
}

void GpuTimer::endFrame(VkCommandBuffer commandBuffer, int frameSlot)
{
    if (!available()) {
        return;
    }
    const uint32_t first = static_cast<uint32_t>(frameSlot) * 2u;
    m_functions->vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, first + 1);
    m_slotWritten[static_cast<std::size_t>(frameSlot)] = 1;
}

} // namespace se::render
