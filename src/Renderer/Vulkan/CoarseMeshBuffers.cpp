#include "Renderer/Vulkan/CoarseMeshBuffers.h"

#include "Core/Assert.h"

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace se::render {

namespace {

constexpr VkMemoryPropertyFlags kHostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags kDeviceHostMemory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | kHostMemory;
constexpr u64 kVertexBytes = 4 * sizeof(u32);

core::Result<GpuBuffer> createPoolBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage)
{
    core::Result<GpuBuffer> buffer = GpuBuffer::create(context, size, usage, kDeviceHostMemory, true);
    if (buffer) {
        return buffer;
    }
    return GpuBuffer::create(context, size, usage, kHostMemory, true);
}

/// Spare slots for copy on write: a group drawn in the last frames and edited
/// again needs one while its home is still read by a frame in flight. A brush
/// touches a few groups per frame; larger edits (undo of a long stroke) catch
/// up over a few frames, drawn stale meanwhile.
u32 sparesFor(u32 groupCount) { return std::clamp(groupCount / 16u, 8u, 64u); }

u32 packSnorm8(const glm::vec3& n)
{
    const auto pack = [](float v) {
        const float c = std::clamp(v, -1.0f, 1.0f);
        return static_cast<u32>(static_cast<u8>(static_cast<std::int8_t>(std::lround(c * 127.0f))));
    };
    return pack(n.x) | (pack(n.y) << 8u) | (pack(n.z) << 16u);
}

} // namespace

u64 CoarseMeshBuffers::bytesFor(const geo::CoarseChunks& coarse, int frameSlots)
{
    const u64 vertices = coarse.vertices.size() + static_cast<u64>(sparesFor(coarse.groupCount())) * coarse.maxGroupVertices;
    const u64 commands = static_cast<u64>(frameSlots) * coarse.groupCount() * sizeof(VkDrawIndexedIndirectCommand);
    return vertices * kVertexBytes + coarse.indices.size() * sizeof(u16) + commands;
}

core::Result<void> CoarseMeshBuffers::create(const VulkanContext& context, geo::CoarseChunks coarse, u32 vertexPageShift,
                                             u32 vertexCount, int frameSlots, VkDescriptorSetLayout setLayout,
                                             bool indirectDraws)
{
    destroy();
    SE_ASSERT(frameSlots > 0 && coarse.groupCount() > 0);
    m_device = context.device;
    m_functions = context.deviceFunctions;
    m_coarse = std::move(coarse);
    m_indirect = indirectDraws;
    m_pageShift = vertexPageShift;
    const u32 groupTotal = m_coarse.groupCount();
    const u32 spares = sparesFor(groupTotal);
    m_spareBase = static_cast<u32>(m_coarse.vertices.size());
    m_location.assign(groupTotal, kHome);
    m_dirty.assign(groupTotal, u8{0});
    m_homeLastUsed.assign(groupTotal, 0);
    m_spareGroup.assign(spares, kNone);
    m_spareLastUsed.assign(spares, 0);
    m_retireDelay = static_cast<u64>(frameSlots);
    m_frame = m_retireDelay; // never-used copies (stamp 0) count as retired
    m_draws.clear();
    m_draws.reserve(groupTotal);

    // Vertex page -> groups whose coarse vertices lie in it.
    const u32 pageCount = (vertexCount + (1u << vertexPageShift) - 1u) >> vertexPageShift;
    std::vector<std::vector<u32>> lists(pageCount);
    for (u32 group = 0; group < groupTotal; ++group) {
        for (const geo::VertexId v : m_coarse.groupVertices(group)) {
            auto& list = lists[v >> vertexPageShift];
            if (list.empty() || list.back() != group) {
                list.push_back(group);
            }
        }
    }
    m_pageOffsets.assign(static_cast<std::size_t>(pageCount) + 1, 0u);
    m_pageGroups.clear();
    for (u32 page = 0; page < pageCount; ++page) {
        m_pageGroups.insert(m_pageGroups.end(), lists[page].begin(), lists[page].end());
        m_pageOffsets[page + 1] = static_cast<u32>(m_pageGroups.size());
    }

    const u64 poolVertices = m_spareBase + static_cast<u64>(spares) * m_coarse.maxGroupVertices;
    auto vertexPool = createPoolBuffer(context, static_cast<VkDeviceSize>(poolVertices * kVertexBytes),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!vertexPool) {
        return std::unexpected(vertexPool.error());
    }
    m_vertexPool = std::move(*vertexPool);
    auto indexPool = createPoolBuffer(context, std::max<VkDeviceSize>(4, m_coarse.indices.size() * sizeof(u16)),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!indexPool) {
        destroy();
        return std::unexpected(indexPool.error());
    }
    m_indexPool = std::move(*indexPool);
    std::memcpy(m_indexPool.mappedData(), m_coarse.indices.data(), m_coarse.indices.size() * sizeof(u16));

    // Per frame slot: the indirect commands of the groups drawn in that slot's
    // frame. Written only when that slot's previous frame has finished.
    m_commandBuffers.clear();
    if (m_indirect) {
        m_commandBuffers.resize(static_cast<std::size_t>(frameSlots));
        for (GpuBuffer& buffer : m_commandBuffers) {
            auto created = createPoolBuffer(context, static_cast<VkDeviceSize>(groupTotal) * sizeof(VkDrawIndexedIndirectCommand),
                                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            if (!created) {
                destroy();
                return std::unexpected(created.error());
            }
            buffer = std::move(*created);
        }
    }
    // One descriptor set: binding 1 is the coarse pool; binding 0 (the page
    // table, unused by mesh_coarse.vert) points at the same buffer.
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkResult result = m_functions->vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkCreateDescriptorPool failed: {}", vulkanResultName(result)));
    }
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    result = m_functions->vkAllocateDescriptorSets(m_device, &allocateInfo, &m_descriptorSet);
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkAllocateDescriptorSets failed: {}", vulkanResultName(result)));
    }
    const VkDescriptorBufferInfo bufferInfo{m_vertexPool.handle(), 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo;
    }
    m_functions->vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return {};
}


void CoarseMeshBuffers::destroy() noexcept
{
    if (m_functions != nullptr && m_device != VK_NULL_HANDLE && m_descriptorPool != VK_NULL_HANDLE) {
        m_functions->vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorSet = VK_NULL_HANDLE;
    m_commandBuffers.clear();
    m_indexPool.reset();
    m_vertexPool.reset();
    m_device = VK_NULL_HANDLE;
    m_functions = nullptr;
}

VkDeviceSize CoarseMeshBuffers::gpuMemoryBytes() const noexcept
{
    VkDeviceSize bytes = m_vertexPool.size() + m_indexPool.size();
    for (const GpuBuffer& buffer : m_commandBuffers) {
        bytes += buffer.size();
    }
    return bytes;
}

void CoarseMeshBuffers::write(const geo::Mesh& mesh, u32 group, u32 firstVertex)
{
    auto* const out = static_cast<u32*>(m_vertexPool.mappedData()) + static_cast<std::size_t>(firstVertex) * 4u;
    const auto positions = mesh.positions();
    const auto normals = mesh.normals();
    const auto vertices = m_coarse.groupVertices(group);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const glm::vec3 p = positions[vertices[i]];
        out[i * 4 + 0] = std::bit_cast<u32>(p.x);
        out[i * 4 + 1] = std::bit_cast<u32>(p.y);
        out[i * 4 + 2] = std::bit_cast<u32>(p.z);
        out[i * 4 + 3] = packSnorm8(normals[vertices[i]]);
    }
}

void CoarseMeshBuffers::uploadAll(const geo::Mesh& mesh)
{
    for (u32 group = 0; group < m_coarse.groupCount(); ++group) {
        write(mesh, group, m_coarse.vertexOffsets[group]);
        m_location[group] = kHome;
        m_dirty[group] = 0;
    }
}

void CoarseMeshBuffers::markDirty(const geo::DirtyVertexSet& vertices)
{
    const u32 pageCount = static_cast<u32>(m_pageOffsets.size()) - 1u;
    vertices.forEachRun([&](u32 first, u32 count) {
        const u32 lastPage = (first + count - 1u) >> m_pageShift;
        for (u32 page = first >> m_pageShift; page <= lastPage && page < pageCount; ++page) {
            for (u32 i = m_pageOffsets[page]; i < m_pageOffsets[page + 1]; ++i) {
                m_dirty[m_pageGroups[i]] = 1u;
            }
        }
    });
}

CoarseMeshBuffers::FrameResult CoarseMeshBuffers::prepare(int frameSlot, const geo::Mesh& mesh, std::span<const u32> groups)
{
    FrameResult result;
    ++m_frame;
    m_draws.clear();
    m_drawFrameSlot = static_cast<u32>(frameSlot);
    // Groups living in a spare go back home as soon as no frame in flight
    // reads home, so spares stay free for the next edits. (Otherwise a few
    // groups would hold every spare and other edited groups could never leave
    // a copy that is drawn every frame: stale for good.) A dirty group moves
    // in the loop below instead, with fresh data.
    for (u32 s = 0; s < m_spareGroup.size(); ++s) {
        const u32 group = m_spareGroup[s];
        if (group != kNone && m_dirty[group] == 0u && retired(m_homeLastUsed[group])) {
            write(mesh, group, m_coarse.vertexOffsets[group]); // same data: the group is not dirty
            m_location[group] = kHome;
            m_spareGroup[s] = kNone; // reusable once its stamp has aged
            result.uploadBytes += m_coarse.groupVertices(group).size() * kVertexBytes;
            ++result.uploads;
        }
    }
    for (const u32 group : groups) {
        u32 location = m_location[group];
        if (m_dirty[group] != 0u) {
            // Where the fresh copy can go: the current copy itself if no frame
            // in flight reads it, else home (when the current copy is a spare
            // and home has retired), else a free retired spare.
            u32 target = kNone;
            if (retired(lastUsedOf(group, location))) {
                target = location;
            } else if (location != kHome && retired(m_homeLastUsed[group])) {
                target = kHome;
            } else {
                for (u32 s = 0; s < m_spareGroup.size(); ++s) {
                    if (m_spareGroup[s] == kNone && retired(m_spareLastUsed[s])) {
                        target = s;
                        break;
                    }
                }
            }
            if (target != kNone) {
                write(mesh, group, baseOf(group, target));
                if (target != location && location != kHome) {
                    m_spareGroup[location] = kNone; // reusable once its stamp has aged
                }
                if (target != kHome) {
                    m_spareGroup[target] = group;
                }
                location = target;
                m_location[group] = location;
                m_dirty[group] = 0u;
                result.uploadBytes += m_coarse.groupVertices(group).size() * kVertexBytes;
                ++result.uploads;
            } else {
                ++result.stale;
            }
        }
        lastUsedOf(group, location) = m_frame;
        const auto indices = m_coarse.groupIndices(group);
        m_draws.push_back({static_cast<u32>(indices.size()), 1u, static_cast<u32>(indices.data() - m_coarse.indices.data()),
                           static_cast<i32>(baseOf(group, location)), 0u});
        result.triangles += static_cast<u32>(indices.size() / 3);
    }
    if (m_indirect && !m_draws.empty()) {
        std::memcpy(m_commandBuffers[m_drawFrameSlot].mappedData(), m_draws.data(),
                    m_draws.size() * sizeof(VkDrawIndexedIndirectCommand));
    }
    return result;
}

u32 CoarseMeshBuffers::countMismatches(const geo::Mesh& mesh, std::span<const u32> groups) const
{
    const auto positions = mesh.positions();
    const auto normals = mesh.normals();
    const auto* const pool = static_cast<const u32*>(m_vertexPool.mappedData());
    u32 mismatches = 0;
    for (const u32 group : groups) {
        const u32* const data = pool + static_cast<std::size_t>(baseOf(group, m_location[group])) * 4u;
        const auto vertices = m_coarse.groupVertices(group);
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const glm::vec3 p = positions[vertices[i]];
            const bool same = data[i * 4 + 0] == std::bit_cast<u32>(p.x) && data[i * 4 + 1] == std::bit_cast<u32>(p.y) &&
                              data[i * 4 + 2] == std::bit_cast<u32>(p.z) && data[i * 4 + 3] == packSnorm8(normals[vertices[i]]);
            mismatches += same ? 0u : 1u;
        }
    }
    return mismatches;
}

u32 CoarseMeshBuffers::draw(VkCommandBuffer commandBuffer, int frameSlot, VkPipelineLayout pipelineLayout) const noexcept
{
    if (m_draws.empty()) {
        return 0;
    }
    SE_ASSERT(static_cast<u32>(frameSlot) == m_drawFrameSlot);
    m_functions->vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                                         &m_descriptorSet, 0, nullptr);
    m_functions->vkCmdBindIndexBuffer(commandBuffer, m_indexPool.handle(), 0, VK_INDEX_TYPE_UINT16);
    if (m_indirect) {
        m_functions->vkCmdDrawIndexedIndirect(commandBuffer, m_commandBuffers[m_drawFrameSlot].handle(), 0,
                                              static_cast<uint32_t>(m_draws.size()), sizeof(VkDrawIndexedIndirectCommand));
        return 1;
    }
    for (const VkDrawIndexedIndirectCommand& d : m_draws) {
        m_functions->vkCmdDrawIndexed(commandBuffer, d.indexCount, 1, d.firstIndex, d.vertexOffset, 0);
    }
    return static_cast<u32>(m_draws.size());
}

} // namespace se::render
