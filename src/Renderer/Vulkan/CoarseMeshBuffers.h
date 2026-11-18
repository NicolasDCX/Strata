#pragma once

#include "Core/Error.h"
#include "Geometry/ChunkLod.h"
#include "Geometry/DirtyVertexSet.h"
#include "Geometry/Mesh.h"
#include "Renderer/Vulkan/GpuBuffer.h"

#include <span>
#include <vector>

namespace se::render {

/// Opt-in coarse render fallback (`--lod`): always-resident coarse stand-ins
/// for groups of chunks (geo::CoarseChunks), drawn for visible chunks whose
/// detailed pages are not usable this frame. A group is drawn coarse as a
/// whole.
///
/// - Vertex pool: every group's coarse vertices at a fixed "home" offset,
///   packed back to back (exact size), followed by a few spare slots of the
///   largest group's size. Each vertex is 16 bytes: position as float bits
///   (bit-identical to the detailed pool, for crack-free seams) + snorm8 normal.
/// - Copy on write without a full spare copy: an edited group is rewritten in
///   place when no frame in flight can still read its current copy (it was not
///   drawn in the last `frameSlots` frames), otherwise into a spare slot; the
///   group moves home again (copied from the CPU mesh) once home has retired,
///   which frees the spare. If no spare is free the group is drawn stale for a
///   frame or two and FrameResult::stale says so.
/// - Index pool: the coarse triangles of every group, 16-bit indices local to
///   the group's vertex list; written once (topology never changes). A draw's
///   vertexOffset selects where the group's vertices are.
/// - Draws: one vkCmdDrawIndexedIndirect per pass for all coarse groups, from
///   a small per-frame-slot command buffer (multiDrawIndirect), or one
///   vkCmdDrawIndexed per group without that feature.
///
/// Coarse vertices are original vertex ids, so after an edit the group is
/// refreshed by copying the current CPU positions and normals of those ids.
/// The CPU mesh, picking and sculpting stay at full resolution.
class CoarseMeshBuffers {
public:
    /// GPU bytes needed for `coarse` (all buffers), counted against the budget.
    [[nodiscard]] static u64 bytesFor(const geo::CoarseChunks& coarse, int frameSlots);

    /// `indirectDraws`: merge the coarse draws with multi-draw indirect (the
    /// device must have multiDrawIndirect enabled).
    [[nodiscard]] core::Result<void> create(const VulkanContext& context, geo::CoarseChunks coarse, u32 vertexPageShift,
                                            u32 vertexCount, int frameSlots, VkDescriptorSetLayout setLayout,
                                            bool indirectDraws);
    void destroy() noexcept;

    /// Fills every group's home from the CPU mesh (once, before the first frame).
    void uploadAll(const geo::Mesh& mesh);

    /// Groups with a coarse vertex among these vertices need a fresh copy.
    void markDirty(const geo::DirtyVertexSet& vertices);

    struct FrameResult {
        u64 uploadBytes = 0;
        u32 uploads = 0;
        u32 triangles = 0;
        u32 stale = 0; ///< groups drawn with older content (no spare free); later frames catch up
    };

    /// Makes the coarse versions of `groups` drawable (uploading those that
    /// changed) and records them for draw() in this frame slot. Call while the
    /// mesh may be read, once per frame.
    FrameResult prepare(int frameSlot, const geo::Mesh& mesh, std::span<const u32> groups);

    /// Test support: coarse vertices of `groups` whose GPU copy (read back from
    /// the mapped pool) differs from the current CPU position or normal.
    /// Call after prepare() for the same groups, while the mesh may be read.
    [[nodiscard]] u32 countMismatches(const geo::Mesh& mesh, std::span<const u32> groups) const;

    /// Draws what prepare() selected. The coarse pipeline must be bound.
    /// Returns the number of draw calls recorded.
    u32 draw(VkCommandBuffer commandBuffer, int frameSlot, VkPipelineLayout pipelineLayout) const noexcept;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(m_vertexPool); }
    [[nodiscard]] VkDeviceSize gpuMemoryBytes() const noexcept;
    [[nodiscard]] const geo::CoarseChunks& chunks() const noexcept { return m_coarse; }
    [[nodiscard]] u32 spareSlots() const noexcept { return static_cast<u32>(m_spareGroup.size()); }
    [[nodiscard]] bool indirectDraws() const noexcept { return m_indirect; }

private:
    static constexpr u32 kHome = 0xFFFFFFFFu;
    static constexpr u32 kNone = 0xFFFFFFFFu;

    void write(const geo::Mesh& mesh, u32 group, u32 firstVertex);
    [[nodiscard]] u32 baseOf(u32 group, u32 location) const noexcept
    {
        return location == kHome ? m_coarse.vertexOffsets[group] : m_spareBase + location * m_coarse.maxGroupVertices;
    }
    [[nodiscard]] bool retired(u64 lastUsed) const noexcept { return lastUsed + m_retireDelay <= m_frame; }
    [[nodiscard]] u64& lastUsedOf(u32 group, u32 location) noexcept
    {
        return location == kHome ? m_homeLastUsed[group] : m_spareLastUsed[location];
    }

    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_functions = nullptr;
    GpuBuffer m_vertexPool;
    GpuBuffer m_indexPool;
    std::vector<GpuBuffer> m_commandBuffers; // per frame slot: VkDrawIndexedIndirectCommand per drawn group
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    bool m_indirect = false;

    geo::CoarseChunks m_coarse;
    u32 m_spareBase = 0;             // first vertex of the spare slots
    std::vector<u32> m_location;     // per group: kHome or spare slot
    std::vector<u8> m_dirty;         // per group: GPU copy older than the CPU mesh
    std::vector<u64> m_homeLastUsed; // per group: frame its home was last drawn from
    std::vector<u32> m_spareGroup;   // per spare slot: group or kNone
    std::vector<u64> m_spareLastUsed;
    u64 m_frame = 0;
    u64 m_retireDelay = 1;
    u32 m_pageShift = 10;
    std::vector<u32> m_pageOffsets; // vertex page -> groups with a coarse vertex in it
    std::vector<u32> m_pageGroups;

    std::vector<VkDrawIndexedIndirectCommand> m_draws; // this frame
    u32 m_drawFrameSlot = 0;
};

} // namespace se::render
