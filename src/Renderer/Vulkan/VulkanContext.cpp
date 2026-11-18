#include "Renderer/Vulkan/VulkanContext.h"

namespace se::render {

core::Result<u32> VulkanContext::findMemoryType(u32 typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memory{};
    instanceFunctions->vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        const bool allowed = (typeBits & (1u << i)) != 0;
        if (allowed && (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return core::makeError("no Vulkan memory type satisfies the requested properties");
}

std::string_view vulkanResultName(VkResult result) noexcept
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    default: return "VkResult (unlisted)";
    }
}

} // namespace se::render
