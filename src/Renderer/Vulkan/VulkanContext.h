#pragma once

#include "Core/Error.h"
#include "Core/Types.h"

#include <QVulkanFunctions>

#include <string_view>

namespace se::render {

/// Non-owning handles shared by the renderer's GPU objects. The device itself
/// is created and destroyed by QVulkanWindow; everything created from this
/// context must be released in QVulkanWindowRenderer::releaseResources().
struct VulkanContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    QVulkanFunctions* instanceFunctions = nullptr;
    QVulkanDeviceFunctions* deviceFunctions = nullptr;

    [[nodiscard]] bool valid() const noexcept { return device != VK_NULL_HANDLE && deviceFunctions != nullptr; }

    /// Index of a memory type allowed by `typeBits` that has all `properties`.
    [[nodiscard]] core::Result<u32> findMemoryType(u32 typeBits, VkMemoryPropertyFlags properties) const;
};

/// Human readable name of a VkResult for error messages.
[[nodiscard]] std::string_view vulkanResultName(VkResult result) noexcept;

} // namespace se::render
