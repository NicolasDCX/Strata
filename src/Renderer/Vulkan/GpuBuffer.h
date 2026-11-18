#pragma once

#include "Core/Error.h"
#include "Renderer/Vulkan/VulkanContext.h"

#include <utility>

namespace se::render {

/// RAII owner of one VkBuffer and its dedicated VkDeviceMemory.
///
/// Move-only. Host-visible buffers can be created persistently mapped. One
/// allocation per buffer is fine for the handful of buffers the prototype
/// needs; a sub-allocator (e.g. VMA) is the planned replacement once the
/// renderer manages many paged vertex buffers.
class GpuBuffer {
public:
    GpuBuffer() noexcept = default;
    ~GpuBuffer() { reset(); }

    GpuBuffer(GpuBuffer&& other) noexcept { *this = std::move(other); }
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;

    /// `persistentlyMapped` requires VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT in `memoryProperties`.
    [[nodiscard]] static core::Result<GpuBuffer> create(const VulkanContext& context, VkDeviceSize size,
                                                        VkBufferUsageFlags usage,
                                                        VkMemoryPropertyFlags memoryProperties,
                                                        bool persistentlyMapped);

    /// Destroys the buffer and frees its memory. Safe to call repeatedly.
    void reset() noexcept;

    [[nodiscard]] VkBuffer handle() const noexcept { return m_buffer; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return m_size; }
    /// Start of the mapped range, or nullptr when the buffer is not mapped.
    [[nodiscard]] void* mappedData() const noexcept { return m_mapped; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_buffer != VK_NULL_HANDLE; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_functions = nullptr;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkDeviceSize m_size = 0;
    void* m_mapped = nullptr;
};

} // namespace se::render
