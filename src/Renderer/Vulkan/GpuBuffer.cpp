#include "Renderer/Vulkan/GpuBuffer.h"

#include <format>
#include <utility>

namespace se::render {

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept
{
    if (this != &other) {
        reset();
        m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
        m_functions = std::exchange(other.m_functions, nullptr);
        m_buffer = std::exchange(other.m_buffer, VK_NULL_HANDLE);
        m_memory = std::exchange(other.m_memory, VK_NULL_HANDLE);
        m_size = std::exchange(other.m_size, 0);
        m_mapped = std::exchange(other.m_mapped, nullptr);
    }
    return *this;
}

core::Result<GpuBuffer> GpuBuffer::create(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                                          VkMemoryPropertyFlags memoryProperties, bool persistentlyMapped)
{
    if (!context.valid() || size == 0) {
        return core::makeError("GpuBuffer::create called with an invalid context or zero size");
    }
    QVulkanDeviceFunctions* df = context.deviceFunctions;

    // Fill the RAII object as we go so every early return cleans up after itself.
    GpuBuffer buffer;
    buffer.m_device = context.device;
    buffer.m_functions = df;
    buffer.m_size = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = df->vkCreateBuffer(context.device, &bufferInfo, nullptr, &buffer.m_buffer);
    if (result != VK_SUCCESS) {
        return core::makeError(std::format("vkCreateBuffer failed: {}", vulkanResultName(result)));
    }

    VkMemoryRequirements requirements{};
    df->vkGetBufferMemoryRequirements(context.device, buffer.m_buffer, &requirements);
    const core::Result<u32> memoryType = context.findMemoryType(requirements.memoryTypeBits, memoryProperties);
    if (!memoryType) {
        return std::unexpected(memoryType.error());
    }

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = *memoryType;
    result = df->vkAllocateMemory(context.device, &allocateInfo, nullptr, &buffer.m_memory);
    if (result != VK_SUCCESS) {
        return core::makeError(std::format("vkAllocateMemory ({} bytes) failed: {}", requirements.size,
                                           vulkanResultName(result)));
    }

    result = df->vkBindBufferMemory(context.device, buffer.m_buffer, buffer.m_memory, 0);
    if (result != VK_SUCCESS) {
        return core::makeError(std::format("vkBindBufferMemory failed: {}", vulkanResultName(result)));
    }

    if (persistentlyMapped) {
        result = df->vkMapMemory(context.device, buffer.m_memory, 0, VK_WHOLE_SIZE, 0, &buffer.m_mapped);
        if (result != VK_SUCCESS) {
            return core::makeError(std::format("vkMapMemory failed: {}", vulkanResultName(result)));
        }
    }
    return buffer;
}

void GpuBuffer::reset() noexcept
{
    if (m_functions != nullptr && m_device != VK_NULL_HANDLE) {
        // Freeing the memory implicitly unmaps it.
        if (m_buffer != VK_NULL_HANDLE) {
            m_functions->vkDestroyBuffer(m_device, m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            m_functions->vkFreeMemory(m_device, m_memory, nullptr);
        }
    }
    m_device = VK_NULL_HANDLE;
    m_functions = nullptr;
    m_buffer = VK_NULL_HANDLE;
    m_memory = VK_NULL_HANDLE;
    m_size = 0;
    m_mapped = nullptr;
}

} // namespace se::render
