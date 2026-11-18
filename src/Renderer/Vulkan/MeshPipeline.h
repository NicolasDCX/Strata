#pragma once

#include "Core/Error.h"
#include "Renderer/Vulkan/VulkanContext.h"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace se::render {

/// Per-frame constants, pushed to both shader stages. Must match the
/// `FrameConstants` block in the shaders (std430 layout).
struct MeshFrameConstants {
    glm::mat4 viewProjection{1.0f};
    glm::vec4 eyePosition{0.0f};
    glm::vec4 brushSphere{0.0f}; // xyz centre, w radius (<= 0: hidden)
    glm::vec4 brushColor{0.0f};
    glm::uvec4 params{0u};       // x: log2 of the GPU vertex page size
};
// 128 bytes is the minimum push constant size every Vulkan implementation supports.
static_assert(sizeof(MeshFrameConstants) == 128, "push constant block layout changed");

/// Graphics pipeline that draws the sculpt mesh with diffuse lighting.
/// Viewport and scissor are dynamic, so the pipeline survives window resizes.
/// There is no vertex input: the vertex shader (mesh_paged.vert) reads
/// positions and normals from the vertex page pool through a page table.
class MeshPipeline {
public:
    MeshPipeline() noexcept = default;
    ~MeshPipeline() { destroy(); }
    MeshPipeline(const MeshPipeline&) = delete;
    MeshPipeline& operator=(const MeshPipeline&) = delete;

    [[nodiscard]] core::Result<void> create(const VulkanContext& context, VkRenderPass renderPass,
                                            VkSampleCountFlagBits samples);
    void destroy() noexcept;

    void bind(VkCommandBuffer commandBuffer, const MeshFrameConstants& constants) const noexcept;
    /// PROTOTYPE: the same pipeline state with mesh_coarse.vert (coarse fallback).
    void bindCoarse(VkCommandBuffer commandBuffer, const MeshFrameConstants& constants) const noexcept;

    [[nodiscard]] bool valid() const noexcept { return m_pipeline != VK_NULL_HANDLE; }
    [[nodiscard]] VkPipelineLayout layout() const noexcept { return m_layout; }
    /// Set 0 ={ binding 0: page table, binding 1: vertex pool }, both storage buffers.
    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept { return m_setLayout; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_functions = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_coarsePipeline = VK_NULL_HANDLE;
};

} // namespace se::render
