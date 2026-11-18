#pragma once

#include "Core/Timer.h"
#include "Geometry/Aabb.h"
#include "Renderer/ChunkPageMap.h"
#include "Renderer/RenderScene.h"
#include "Renderer/Vulkan/CoarseMeshBuffers.h"
#include "Renderer/Vulkan/GpuTimer.h"
#include "Renderer/Vulkan/MeshPipeline.h"
#include "Renderer/Vulkan/ResidentMeshBuffers.h"
#include "Renderer/Vulkan/VulkanContext.h"

#include <QVulkanWindow>

#include <vector>

namespace se::render {

/// Draws a RenderScene into a QVulkanWindow.
///
/// QVulkanWindow owns the instance-level plumbing this prototype does not need
/// to customise: device selection, the swapchain, the depth/MSAA attachments,
/// the default render pass, per-frame command buffers and frame pacing. This
/// class owns everything specific to the engine.
///
/// Per frame:
///   1. under the mesh lock: take pending edits, snapshot the chunk bounds,
///      cull chunks against the view frustum, bring GPU data up to date;
///   2. record one render pass that draws the chunks that are visible and whose
///      data is on the GPU.
///
/// Storage: vertex pages and chunk index data live in page pools
/// (ResidentMeshBuffers). By default the pools hold the whole mesh plus spare
/// slots for edits; with a budget from RenderSettings they hold what the view
/// needs most, uploaded on demand and evicted when full.
///
/// Lifetime: resources are created in initResources() and destroyed in
/// releaseResources(), which Qt calls before the device goes away.
class SceneRenderer final : public QVulkanWindowRenderer {
public:
    /// `scene` (and the mesh/camera it points to) must outlive the renderer.
    /// `stats` is optional and receives per-frame measurements.
    SceneRenderer(QVulkanWindow* window, RenderScene* scene, RenderStats* stats = nullptr);

    void initResources() override;
    void releaseResources() override;
    void startNextFrame() override;

private:
    /// Fills m_visibleChunks with the chunks to consider this frame, in the
    /// order the active storage mode wants them. Needs the mesh lock.
    void selectChunks(const glm::mat4& viewProjection, const glm::vec3& eye);

    QVulkanWindow* m_window;
    RenderScene* m_scene;
    RenderStats* m_stats;

    VulkanContext m_context;
    MeshPipeline m_pipeline;
    GpuTimer m_gpuTimer;
    ChunkPageMap m_chunkMap;
    ResidentMeshBuffers m_residentBuffers;
    CoarseMeshBuffers m_coarseBuffers; // opt-in (RenderSettings::coarseFallback)
    std::vector<u32> m_coarseChunks;   // visible chunks drawn coarse this frame
    std::vector<u32> m_coarseGroups;   // groups drawn coarse this frame
    std::vector<u8> m_groupCoarse;     // per group, scratch (all 0 between frames)
    std::vector<float> m_groupDistance;
    std::vector<u32> m_detailChunks;
    u32 m_backFacing = 0;
    u32 m_backFacingKeptStale = 0;

    // Per-frame scratch, kept to avoid allocations.
    std::vector<geo::Aabb> m_chunkBounds;
    std::vector<u32> m_visibleChunks;
    std::vector<u32> m_readyChunks;
    std::vector<IndexRun> m_runs;
    std::vector<std::pair<float, u32>> m_chunkOrder;

    core::Stopwatch m_sinceFrameStart;
    bool m_hasPreviousFrame = false;
};

} // namespace se::render
