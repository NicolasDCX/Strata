#pragma once

#include "Core/Error.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/Mesh.h"
#include "Renderer/ChunkPageMap.h"
#include "Renderer/PagePool.h"
#include "Renderer/Vulkan/GpuBuffer.h"

#include <span>
#include <vector>

namespace se::render {

/// GPU storage for a mesh: the renderer's only storage path.
///
/// There are two fixed-size pools:
///
/// - a vertex pool of slots, each holding one GPU vertex page (positions and
///   normals of 2^n consecutive vertex ids);
/// - an index pool of slots, each holding the index data of one chunk.
///
/// Which page is in which slot is decided by a PagePool per pool. Vertices are
/// found by the shader through a page table (see mesh_paged.vert), so a page
/// can sit in any slot and a vertex shared by several chunks exists once.
///
/// Per frame, prepare() walks the visible chunks in priority order and, for
/// each, requests its vertex pages and its index data, copying what is missing
/// or out of date from the CPU mesh (which stays complete and authoritative)
/// until the frame's upload limit is reached. A chunk is drawn only if
/// everything it needs is usable.
///
/// Frames in flight: the pools are shared by all frame slots, and PagePool
/// guarantees that a slot a queued frame may read is neither overwritten nor
/// reassigned. Each frame slot has its own page table buffer, written only when
/// that slot's previous frame has finished. So nothing here waits for the GPU.
///
/// Sizing: with budgetBytes 0 the pools hold the whole mesh (every vertex page
/// and every chunk's indices) plus spare vertex slots for copy on write, and
/// uploadAll() fills them once at creation. Every visible chunk is then drawn
/// in every frame; edits move pages into spare slots. With a budget, the pools
/// are that size and hold whatever the view needs most.
class ResidentMeshBuffers {
public:
    struct Config {
        u64 budgetBytes = 0; ///< 0: whole mesh plus copy-on-write spares
        u64 maxUploadBytesPerFrame = 0;
    };

    struct FrameResult {
        u64 uploadBytes = 0;
        u32 uploadVertexCount = 0;
        u32 evictions = 0;
        u32 stalePages = 0;
        /// Visible chunks not even tried because a pool was full.
        u32 skippedChunks = 0;
        /// Distinct vertex pages the visible chunks need (to tell whether the view fits the pool).
        u32 visibleVertexPages = 0;
        /// Uploads were held back by the per-frame limit: another frame will make progress.
        bool pendingWork = false;
    };

    /// Bytes one complete copy of the mesh needs in this representation.
    [[nodiscard]] static u64 fullResidencyBytes(const ChunkPageMap& map);

    /// How create() sizes the pools for a budget (0: whole mesh).
    struct SlotPlan {
        u32 vertexSlots = 0;
        u32 indexSlots = 0;
        u32 reserveSlots = 0;
        bool holdsWholeMesh = false; ///< see holdsWholeMesh()
    };
    [[nodiscard]] static SlotPlan planSlots(const ChunkPageMap& map, u64 budgetBytes);

    [[nodiscard]] core::Result<void> create(const VulkanContext& context, const ChunkPageMap& map, int frameSlots,
                                            const Config& config, VkDescriptorSetLayout setLayout);
    void destroy() noexcept;

    /// The CPU data of these vertices changed (any page size).
    void markDirty(const geo::DirtyVertexSet& vertices);

    /// True when one of the chunk's vertex pages is resident with content older
    /// than the CPU mesh (marked dirty, not re-uploaded yet). Drawing the chunk
    /// now could show that older content.
    [[nodiscard]] bool hasStaleCopy(const ChunkPageMap& map, u32 chunk) const noexcept
    {
        for (const u32 page : map.vertexPages(chunk)) {
            if (m_vertexPages.isDirty(page) && m_vertexPages.slotOf(page) != PagePool::kNoSlot) {
                return true;
            }
        }
        return false;
    }

    /// Makes as many of `visibleChunks` drawable as budget and upload limit
    /// allow, most important first, and writes the frame slot's page table.
    /// Appends the drawable ones to `readyChunks`. Call while the mesh may be read.
    /// With `unavailableChunks`, the visible chunks that cannot be drawn this
    /// frame are appended to it (for the coarse fallback).
    FrameResult prepare(int frameSlot, const geo::Mesh& mesh, const ChunkPageMap& map, std::span<const u32> visibleChunks,
                        std::vector<u32>& readyChunks, std::vector<u32>* unavailableChunks = nullptr);

    /// Uploads every vertex page and every chunk's indices, ignoring the
    /// per-frame limit. For pools that hold the whole mesh; call once after
    /// create(), before the first frame, while the mesh may be read.
    void uploadAll(const geo::Mesh& mesh, const ChunkPageMap& map);

    /// Index ranges in the index pool for the given ready chunks. With `merge`,
    /// chunks in consecutive slots become one range (drawn in slot order).
    void buildRuns(const ChunkPageMap& map, std::span<const u32> readyChunks, bool merge,
                   std::vector<IndexRun>& outRuns) const;

    /// Draws index ranges from buildRuns(). Returns the number of draw calls.
    u32 draw(VkCommandBuffer commandBuffer, int frameSlot, VkPipelineLayout pipelineLayout,
             std::span<const IndexRun> runs) const noexcept;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(m_vertexPool); }
    /// The pools can hold every page and every chunk at once, with the reserve
    /// left over (the whole-mesh default, or a budget at least that large).
    [[nodiscard]] bool holdsWholeMesh() const noexcept { return m_holdsWholeMesh; }
    [[nodiscard]] VkDeviceSize gpuMemoryBytes() const noexcept;
    [[nodiscard]] u32 vertexSlots() const noexcept { return m_vertexPages.slotCount(); }
    [[nodiscard]] u32 vertexPagesResident() const noexcept { return m_vertexPages.residentCount(); }
    [[nodiscard]] u32 vertexReserveSlots() const noexcept { return m_vertexPages.reserveSlots(); }
    /// True when every pool is in GPU memory the CPU can map (else some are in host memory).
    [[nodiscard]] bool deviceLocal() const noexcept { return m_deviceLocal; }
    [[nodiscard]] u32 indexSlots() const noexcept { return m_indexChunks.slotCount(); }
    [[nodiscard]] u32 indexChunksResident() const noexcept { return m_indexChunks.residentCount(); }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_functions = nullptr;

    GpuBuffer m_vertexPool;              // storage buffer: vertex page slots
    GpuBuffer m_indexPool;               // index buffer: chunk slots
    std::vector<GpuBuffer> m_pageTables; // storage buffers, one per frame slot
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets; // one per frame slot

    PagePool m_vertexPages; // logical: GPU vertex page
    PagePool m_indexChunks; // logical: chunk
    bool m_deviceLocal = false;
    bool m_holdsWholeMesh = false;
    std::vector<u64> m_pageSeen; // per vertex page: last frame counted in visibleVertexPages
    u32 m_pageShift = 10;
    u32 m_vertexCount = 0;
    u64 m_vertexSlotBytes = 0;
    u64 m_indexSlotBytes = 0;
    u32 m_indexSlotIndices = 0;
    u64 m_maxUploadBytes = 0;
};

} // namespace se::render
