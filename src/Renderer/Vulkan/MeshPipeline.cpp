#include "Renderer/Vulkan/MeshPipeline.h"

#include "Renderer/Shaders/mesh.frag.h"
#include "Renderer/Shaders/mesh_coarse.vert.h"
#include "Renderer/Shaders/mesh_paged.vert.h"

#include <array>
#include <format>

namespace se::render {

namespace {

constexpr VkShaderStageFlags kPushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

/// Destroys a shader module when it goes out of scope (modules are only needed
/// while the pipeline is being created).
class ScopedShaderModule {
public:
    ScopedShaderModule(const VulkanContext& context, const unsigned char* code, std::size_t size)
        : m_device(context.device), m_functions(context.deviceFunctions)
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = reinterpret_cast<const uint32_t*>(code); // arrays are alignas(4)
        m_result = m_functions->vkCreateShaderModule(m_device, &info, nullptr, &m_module);
    }
    ~ScopedShaderModule()
    {
        if (m_module != VK_NULL_HANDLE) {
            m_functions->vkDestroyShaderModule(m_device, m_module, nullptr);
        }
    }
    ScopedShaderModule(const ScopedShaderModule&) = delete;
    ScopedShaderModule& operator=(const ScopedShaderModule&) = delete;

    [[nodiscard]] VkResult result() const noexcept { return m_result; }
    [[nodiscard]] VkShaderModule handle() const noexcept { return m_module; }

private:
    VkDevice m_device;
    QVulkanDeviceFunctions* m_functions;
    VkShaderModule m_module = VK_NULL_HANDLE;
    VkResult m_result = VK_SUCCESS;
};

} // namespace

core::Result<void> MeshPipeline::create(const VulkanContext& context, VkRenderPass renderPass,
                                        VkSampleCountFlagBits samples)
{
    destroy();
    m_device = context.device;
    m_functions = context.deviceFunctions;

    const ScopedShaderModule vertexShader(context, shaders::kMeshPagedVertexSpirv, shaders::kMeshPagedVertexSpirvSize);
    const ScopedShaderModule fragmentShader(context, shaders::kMeshFragmentSpirv, shaders::kMeshFragmentSpirvSize);
    const ScopedShaderModule coarseShader(context, shaders::kMeshCoarseVertexSpirv, shaders::kMeshCoarseVertexSpirvSize);
    if (vertexShader.result() != VK_SUCCESS || fragmentShader.result() != VK_SUCCESS || coarseShader.result() != VK_SUCCESS) {
        return core::makeError("vkCreateShaderModule failed for the mesh shaders");
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader.handle();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader.handle();
    stages[1].pName = "main";

    // No vertex input: the shader fetches vertices from the pool.
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    // World triangles are counter-clockwise; the projection's Y flip keeps that
    // winding in framebuffer space (see OrbitCamera::projection).
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = samples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    const std::array<VkDescriptorSetLayoutBinding, 2> setBindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = static_cast<uint32_t>(setBindings.size());
    setInfo.pBindings = setBindings.data();
    VkResult result = m_functions->vkCreateDescriptorSetLayout(m_device, &setInfo, nullptr, &m_setLayout);
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkCreateDescriptorSetLayout failed: {}", vulkanResultName(result)));
    }

    const VkPushConstantRange pushConstants{kPushConstantStages, 0, sizeof(MeshFrameConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstants;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_setLayout;
    result = m_functions->vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_layout);
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkCreatePipelineLayout failed: {}", vulkanResultName(result)));
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    result = m_functions->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkCreateGraphicsPipelines failed: {}", vulkanResultName(result)));
    }
    // Coarse fallback (--lod): the same state with its vertex shader, drawn
    // two-sided. Coarse topology is fixed when the mesh is loaded, so after
    // strong deformation some coarse triangles can face the other way than
    // the surface they stand in for; culled, they would open see-through
    // holes. For a closed surface, back faces lie behind front faces and
    // the depth test hides them.
    stages[0].module = coarseShader.handle();
    rasterization.cullMode = VK_CULL_MODE_NONE;
    result = m_functions->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_coarsePipeline);
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkCreateGraphicsPipelines (coarse) failed: {}", vulkanResultName(result)));
    }
    return {};
}

void MeshPipeline::destroy() noexcept
{
    if (m_functions != nullptr && m_device != VK_NULL_HANDLE) {
        if (m_pipeline != VK_NULL_HANDLE) {
            m_functions->vkDestroyPipeline(m_device, m_pipeline, nullptr);
        }
        if (m_coarsePipeline != VK_NULL_HANDLE) {
            m_functions->vkDestroyPipeline(m_device, m_coarsePipeline, nullptr);
        }
        if (m_layout != VK_NULL_HANDLE) {
            m_functions->vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        }
        if (m_setLayout != VK_NULL_HANDLE) {
            m_functions->vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        }
    }
    m_pipeline = VK_NULL_HANDLE;
    m_coarsePipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_functions = nullptr;
}

void MeshPipeline::bind(VkCommandBuffer commandBuffer, const MeshFrameConstants& constants) const noexcept
{
    m_functions->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    m_functions->vkCmdPushConstants(commandBuffer, m_layout, kPushConstantStages, 0, sizeof(MeshFrameConstants),
                                    &constants);
}

void MeshPipeline::bindCoarse(VkCommandBuffer commandBuffer, const MeshFrameConstants& constants) const noexcept
{
    m_functions->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_coarsePipeline);
    m_functions->vkCmdPushConstants(commandBuffer, m_layout, kPushConstantStages, 0, sizeof(MeshFrameConstants),
                                    &constants);
}

} // namespace se::render
