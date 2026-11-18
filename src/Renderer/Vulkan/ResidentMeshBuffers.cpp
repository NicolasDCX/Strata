#include "Renderer/Vulkan/ResidentMeshBuffers.h"

#include "Core/Assert.h"

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace se::render {

namespace {

constexpr VkMemoryPropertyFlags kHostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
/// GPU memory the CPU can write directly (BAR / resizable BAR).
constexpr VkMemoryPropertyFlags kDeviceHostMemory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | kHostMemory;

/// The pools are written by the CPU and read by every frame. In plain host
/// memory the GPU reads them across PCIe each frame (measured at 5.2M
/// triangles: 10.6 ms GPU per frame instead of about 0.6), so GPU memory the
/// CPU can map is preferred. Without such memory, or once its heap is full, host
/// memory still works.
core::Result<GpuBuffer> createPoolBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                                         bool& deviceLocal)
{
    core::Result<GpuBuffer> buffer = GpuBuffer::create(context, size, usage, kDeviceHostMemory, true);
    if (buffer) {
        return buffer;
    }
    deviceLocal = false;
    return GpuBuffer::create(context, size, usage, kHostMemory, true);
}

/// Fewest slots a pool is given, however small the budget.
constexpr u32 kMinimumSlots = 4;

u64 vertexSlotBytesFor(u32 pageShift)
{
    return (u64{1} << pageShift) * 2ull * sizeof(glm::vec3); // positions + normals
}

u64 indexSlotBytesFor(const ChunkPageMap& map)
{
    return static_cast<u64>(map.chunkTriangleCapacity()) * 3ull * sizeof(u32);
}

} // namespace

u64 ResidentMeshBuffers::fullResidencyBytes(const ChunkPageMap& map)
{
    return static_cast<u64>(map.vertexPageCount()) * vertexSlotBytesFor(map.gpuPageShift()) +
           static_cast<u64>(map.chunkCount()) * indexSlotBytesFor(map);
}

ResidentMeshBuffers::SlotPlan ResidentMeshBuffers::planSlots(const ChunkPageMap& map, u64 budgetBytes)
{
    const u32 pageCount = map.vertexPageCount();
    const u64 vertexSlotBytes = vertexSlotBytesFor(map.gpuPageShift());
    const u64 indexSlotBytes = indexSlotBytesFor(map);
    u32 vertexSlots = 0;
    u32 indexSlots = 0;
    u32 reserveSlots = 0;
    if (budgetBytes == 0) {
        // Whole mesh: one slot per page and per chunk, plus spare vertex slots.
        // An edited page is uploaded to a spare slot and its old slot becomes
        // free once no frame in flight reads it, so the spares bound how many
        // pages can change within that many frames before edits wait a frame.
        // An eighth (at least 64) covers the largest strokes measured, about
        // 45 changed pages per frame at 5.2M triangles.
        reserveSlots = std::max(64u, pageCount / 8u);
        vertexSlots = pageCount + reserveSlots;
        indexSlots = map.chunkCount();
    } else {
        // Split the budget between the two pools in the proportion a complete
        // copy of the mesh would use. Slots beyond what the mesh can ever fill
        // would be wasted, so both pools are capped; the vertex pool keeps some
        // extra slots because an edited page briefly exists twice.
        const u64 fullVertexBytes = static_cast<u64>(pageCount) * vertexSlotBytes;
        const u64 fullIndexBytes = static_cast<u64>(map.chunkCount()) * indexSlotBytes;
        const double vertexShare = static_cast<double>(fullVertexBytes) / static_cast<double>(fullVertexBytes + fullIndexBytes);
        const u64 vertexBudget = static_cast<u64>(static_cast<double>(budgetBytes) * vertexShare);
        const u64 indexBudget = budgetBytes - std::min(budgetBytes, vertexBudget);
        const u32 vertexSlotCap = pageCount + pageCount / 8u + 64u;
        vertexSlots = static_cast<u32>(std::clamp<u64>(vertexBudget / vertexSlotBytes, kMinimumSlots, vertexSlotCap));
        indexSlots = static_cast<u32>(std::clamp<u64>(indexBudget / indexSlotBytes, kMinimumSlots, map.chunkCount()));
        // An eighth of the vertex slots is kept for copy on write, so edits
        // reach the screen even when the visible set fills the pool.
        reserveSlots = std::max(1u, vertexSlots / 8u);
    }
    return {vertexSlots, indexSlots, reserveSlots, vertexSlots >= pageCount + reserveSlots && indexSlots >= map.chunkCount()};

}

core::Result<void> ResidentMeshBuffers::create(const VulkanContext& context, const ChunkPageMap& map, int frameSlots,
                                               const Config& config, VkDescriptorSetLayout setLayout)
{
    destroy();
    SE_ASSERT(frameSlots > 0);
    if (map.chunkCount() == 0 || map.vertexPageCount() == 0) {
        return core::makeError("cannot create GPU pools for an empty mesh");
    }
    m_device = context.device;
    m_functions = context.deviceFunctions;
    m_pageShift = map.gpuPageShift();
    m_vertexSlotBytes = vertexSlotBytesFor(m_pageShift);
    m_indexSlotBytes = indexSlotBytesFor(map);
    m_indexSlotIndices = map.chunkTriangleCapacity() * 3u;
    m_maxUploadBytes = std::max<u64>(config.maxUploadBytesPerFrame, m_vertexSlotBytes + m_indexSlotBytes);

    const u32 pageCount = map.vertexPageCount();
    const SlotPlan plan = planSlots(map, config.budgetBytes);
    const u32 vertexSlots = plan.vertexSlots;
    const u32 indexSlots = plan.indexSlots;
    const u32 reserveSlots = plan.reserveSlots;
    m_holdsWholeMesh = plan.holdsWholeMesh;

    const u32 retireDelay = static_cast<u32>(frameSlots);
    // Index data never changes (fixed topology) and needs no reserve.
    m_vertexPages.reset(pageCount, vertexSlots, retireDelay, reserveSlots);
    m_indexChunks.reset(map.chunkCount(), indexSlots, retireDelay);
    m_pageSeen.assign(map.vertexPageCount(), 0);

    m_deviceLocal = true;
    core::Result<GpuBuffer> vertexPool = createPoolBuffer(context, static_cast<VkDeviceSize>(vertexSlots) * m_vertexSlotBytes,
                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_deviceLocal);
    if (!vertexPool) {
        return std::unexpected(vertexPool.error());
    }
    m_vertexPool = std::move(*vertexPool);

    core::Result<GpuBuffer> indexPool = createPoolBuffer(context, static_cast<VkDeviceSize>(indexSlots) * m_indexSlotBytes,
                                                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_deviceLocal);
    if (!indexPool) {
        destroy();
        return std::unexpected(indexPool.error());
    }
    m_indexPool = std::move(*indexPool);

    const VkDeviceSize tableBytes = static_cast<VkDeviceSize>(map.vertexPageCount()) * sizeof(u32);
    m_pageTables.resize(static_cast<std::size_t>(frameSlots));
    for (GpuBuffer& table : m_pageTables) {
        core::Result<GpuBuffer> buffer =
            createPoolBuffer(context, tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_deviceLocal);
        if (!buffer) {
            destroy();
            return std::unexpected(buffer.error());
        }
        table = std::move(*buffer);
    }

    // One descriptor set per frame slot: its own page table, the shared vertex pool.
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<uint32_t>(frameSlots) * 2u};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<uint32_t>(frameSlots);
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkResult result = m_functions->vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkCreateDescriptorPool failed: {}", vulkanResultName(result)));
    }
    const std::vector<VkDescriptorSetLayout> layouts(static_cast<std::size_t>(frameSlots), setLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(frameSlots);
    allocateInfo.pSetLayouts = layouts.data();
    m_descriptorSets.resize(static_cast<std::size_t>(frameSlots));
    result = m_functions->vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data());
    if (result != VK_SUCCESS) {
        destroy();
        return core::makeError(std::format("vkAllocateDescriptorSets failed: {}", vulkanResultName(result)));
    }
    for (std::size_t slot = 0; slot < m_descriptorSets.size(); ++slot) {
        const VkDescriptorBufferInfo tableInfo{m_pageTables[slot].handle(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo poolBufferInfo{m_vertexPool.handle(), 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptorSets[slot];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &tableInfo;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].pBufferInfo = &poolBufferInfo;
        m_functions->vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return {};
}

void ResidentMeshBuffers::destroy() noexcept
{
    if (m_functions != nullptr && m_device != VK_NULL_HANDLE && m_descriptorPool != VK_NULL_HANDLE) {
        m_functions->vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); // frees the sets
    }
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorSets.clear();
    m_pageTables.clear();
    m_indexPool.reset();
    m_vertexPool.reset();
    m_device = VK_NULL_HANDLE;
    m_functions = nullptr;
}

void ResidentMeshBuffers::markDirty(const geo::DirtyVertexSet& vertices)
{
    // A changed page that is resident gets a fresh copy the next time it is
    // requested. A page that is not resident needs nothing: whenever it is
    // uploaded, the upload reads the current data.
    markDirtyVertexPages(m_vertexPages, vertices, m_pageShift);
}

ResidentMeshBuffers::FrameResult ResidentMeshBuffers::prepare(int frameSlot, const geo::Mesh& mesh, const ChunkPageMap& map,
                                                              std::span<const u32> visibleChunks,
                                                              std::vector<u32>& readyChunks,
                                                              std::vector<u32>* unavailableChunks)
{
    FrameResult result;
    m_vertexPages.beginFrame();
    m_indexChunks.beginFrame();
    m_vertexCount = mesh.vertexCount();

    auto* const vertexPool = static_cast<glm::vec3*>(m_vertexPool.mappedData());
    auto* const indexPool = static_cast<u32*>(m_indexPool.mappedData());
    const u32 pageVertices = 1u << m_pageShift;
    u64 uploadLeft = m_maxUploadBytes;

    const u64 frame = m_vertexPages.frame();
    for (const u32 chunk : visibleChunks) {
        for (const u32 page : map.vertexPages(chunk)) {
            if (m_pageSeen[page] != frame) {
                m_pageSeen[page] = frame;
                ++result.visibleVertexPages;
            }
        }
        // Chunks arrive nearest first. Once a pool has no more slots for this
        // frame, a farther chunk is drawn only if it needs no slot beyond those
        // already in use: requesting more would claim slots for a chunk that
        // cannot be completed, or eat into the copy-on-write reserve (and then
        // edits could never be uploaded).
        const bool vertexFull = m_vertexPages.full();
        if (vertexFull || m_indexChunks.full()) {
            bool free = m_indexChunks.full() ? m_indexChunks.freeToUse(chunk) : true;
            if (vertexFull) {
                for (const u32 page : map.vertexPages(chunk)) {
                    free = free && m_vertexPages.freeToUse(page);
                }
            }
            if (!free) {
                ++result.skippedChunks;
                if (unavailableChunks != nullptr) {
                    unavailableChunks->push_back(chunk);
                }
                continue;
            }
        }
        bool ready = true;
        for (const u32 page : map.vertexPages(chunk)) {
            const PagePool::Access access = m_vertexPages.request(page, uploadLeft >= m_vertexSlotBytes);
            if (access.upload) {
                // Slot layout: [pageVertices positions | pageVertices normals].
                const u32 first = page << m_pageShift;
                const u32 count = std::min(pageVertices, m_vertexCount - first);
                glm::vec3* const slot = vertexPool + static_cast<std::size_t>(access.slot) * pageVertices * 2u;
                mesh.copyPositions(first, count, slot);
                mesh.copyNormals(first, count, slot + pageVertices);
                uploadLeft -= m_vertexSlotBytes;
                result.uploadBytes += static_cast<u64>(count) * 2ull * sizeof(glm::vec3);
                result.uploadVertexCount += count;
            }
            ready = ready && access.usable;
        }
        const PagePool::Access indexAccess = m_indexChunks.request(chunk, uploadLeft >= m_indexSlotBytes);
        if (indexAccess.upload) {
            const u32 triangles = map.chunkTriangles(chunk);
            mesh.copyIndices(map.firstTriangle(chunk), triangles,
                             indexPool + static_cast<std::size_t>(indexAccess.slot) * m_indexSlotIndices);
            uploadLeft -= m_indexSlotBytes;
            result.uploadBytes += static_cast<u64>(triangles) * 3ull * sizeof(u32);
        }
        ready = ready && indexAccess.usable;
        if (ready) {
            readyChunks.push_back(chunk);
        } else if (unavailableChunks != nullptr) {
            unavailableChunks->push_back(chunk);
        }
    }

    // The frame slot's page table: where every resident page is right now.
    // Entries of absent pages are never read, because chunks that need them
    // are not drawn.
    auto* const table = static_cast<u32*>(m_pageTables[static_cast<std::size_t>(frameSlot)].mappedData());
    const u32 pageCount = m_vertexPages.logicalCount();
    for (u32 page = 0; page < pageCount; ++page) {
        const u32 slot = m_vertexPages.slotOf(page);
        table[page] = slot == PagePool::kNoSlot ? 0u : slot;
    }

    const PagePool::FrameStats& vertexStats = m_vertexPages.frameStats();
    const PagePool::FrameStats& indexStats = m_indexChunks.frameStats();
    result.evictions = vertexStats.evictions + indexStats.evictions;
    result.stalePages = vertexStats.staleUses;
    // Deferred uploads and stale pages are caught up over the next frames.
    result.pendingWork = vertexStats.deferredByCaller + indexStats.deferredByCaller + vertexStats.staleUses > 0;
    return result;
}

void ResidentMeshBuffers::uploadAll(const geo::Mesh& mesh, const ChunkPageMap& map)
{
    // One unlimited prepare() over every chunk in ascending order. Free slots
    // are handed out in order, so chunk c gets index slot c and consecutive
    // chunks can be drawn as one range. Frame slot 0's page table is written
    // too; no frame is in flight yet, and every frame rewrites its own table.
    std::vector<u32> all(map.chunkCount());
    for (u32 chunk = 0; chunk < map.chunkCount(); ++chunk) {
        all[chunk] = chunk;
    }
    std::vector<u32> ready;
    ready.reserve(all.size());
    const u64 limit = m_maxUploadBytes;
    m_maxUploadBytes = ~u64{0};
    prepare(0, mesh, map, all, ready);
    m_maxUploadBytes = limit;
}

void ResidentMeshBuffers::buildRuns(const ChunkPageMap& map, std::span<const u32> readyChunks, bool merge,
                                    std::vector<IndexRun>& outRuns) const
{
    outRuns.clear();
    for (const u32 chunk : readyChunks) {
        outRuns.push_back({m_indexChunks.slotOf(chunk) * m_indexSlotIndices, map.chunkTriangles(chunk) * 3u});
    }
    // A slot holds a full chunk's capacity, so a chunk's range ends where the
    // next slot begins exactly when the chunk is full (all but the last chunk).
    if (merge) {
        mergeIndexRuns(outRuns);
    }
}

u32 ResidentMeshBuffers::draw(VkCommandBuffer commandBuffer, int frameSlot, VkPipelineLayout pipelineLayout,
                              std::span<const IndexRun> runs) const noexcept
{
    if (runs.empty()) {
        return 0;
    }
    m_functions->vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                                         &m_descriptorSets[static_cast<std::size_t>(frameSlot)], 0, nullptr);
    m_functions->vkCmdBindIndexBuffer(commandBuffer, m_indexPool.handle(), 0, VK_INDEX_TYPE_UINT32);
    for (const IndexRun& run : runs) {
        // Indices are global vertex ids; the vertex shader resolves them through the page table.
        m_functions->vkCmdDrawIndexed(commandBuffer, run.indexCount, 1, run.firstIndex, 0, 0);
    }
    return static_cast<u32>(runs.size());
}

VkDeviceSize ResidentMeshBuffers::gpuMemoryBytes() const noexcept
{
    VkDeviceSize bytes = m_vertexPool.size() + m_indexPool.size();
    for (const GpuBuffer& table : m_pageTables) {
        bytes += table.size();
    }
    return bytes;
}

} // namespace se::render
