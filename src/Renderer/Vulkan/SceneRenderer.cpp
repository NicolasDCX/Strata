#include "Renderer/Vulkan/SceneRenderer.h"

#include "Core/Assert.h"
#include "Core/JobSystem.h"
#include "Geometry/ChunkLod.h"
#include "Core/Log.h"
#include "Core/Timer.h"
#include "Renderer/Frustum.h"

#include <QVulkanFunctions>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <thread>

namespace se::render {

namespace {

constexpr VkClearColorValue kBackground = {{0.115f, 0.120f, 0.135f, 1.0f}};
constexpr double kMiB = 1024.0 * 1024.0;

/// GPU resource creation failures at startup leave nothing to render with.
[[noreturn]] void fatal(const core::Error& error)
{
    core::logError("Renderer initialisation failed: {}", error.message);
    qFatal("SculptEngine renderer initialisation failed: %s", error.message.c_str());
    std::abort(); // qFatal does not return; keeps [[noreturn]] honest for every Qt configuration
}

/// Locks the scene's mesh for reading, if it has a lock.
std::unique_lock<core::FairMutex> lockMesh(const RenderScene& scene)
{
    return scene.meshLock != nullptr ? std::unique_lock<core::FairMutex>(*scene.meshLock)
                                     : std::unique_lock<core::FairMutex>();
}

} // namespace

SceneRenderer::SceneRenderer(QVulkanWindow* window, RenderScene* scene, RenderStats* stats)
    : m_window(window), m_scene(scene), m_stats(stats)
{
    SE_ASSERT(window != nullptr && scene != nullptr);
}

void SceneRenderer::initResources()
{
    SE_ASSERT(m_scene->mesh != nullptr && m_scene->camera != nullptr);
    const RenderSettings& settings = m_scene->settings;

    QVulkanInstance* instance = m_window->vulkanInstance();
    m_context.physicalDevice = m_window->physicalDevice();
    m_context.device = m_window->device();
    m_context.instanceFunctions = instance->functions();
    m_context.deviceFunctions = instance->deviceFunctions(m_context.device);

    if (const VkPhysicalDeviceProperties* properties = m_window->physicalDeviceProperties()) {
        core::logInfo("Vulkan device: {} (API {}.{}.{}), {}x MSAA, {} frame slot(s)", properties->deviceName,
                      VK_VERSION_MAJOR(properties->apiVersion), VK_VERSION_MINOR(properties->apiVersion),
                      VK_VERSION_PATCH(properties->apiVersion), static_cast<int>(m_window->sampleCountFlagBits()),
                      m_window->concurrentFrameCount());
    }

    if (const auto created = m_pipeline.create(m_context, m_window->defaultRenderPass(), m_window->sampleCountFlagBits());
        !created) {
        fatal(created.error());
    }
    const bool timing = m_gpuTimer.create(m_context, m_window->graphicsQueueFamilyIndex(), m_window->concurrentFrameCount());

    u64 budgetBytes = 0;
    {
        const std::unique_lock<core::FairMutex> meshLock = lockMesh(*m_scene);
        const geo::Mesh& mesh = *m_scene->mesh;
        // Without chunk information the mesh is treated as one chunk.
        const u32 chunkShift = m_scene->chunks != nullptr ? m_scene->chunks->chunkTriangleShift() : 31u;
        m_chunkMap.build(mesh, chunkShift, settings.gpuPageShift);

        const u64 fullBytes = ResidentMeshBuffers::fullResidencyBytes(m_chunkMap);
        if (settings.limitedBudget()) {
            budgetBytes = settings.gpuBudgetBytes > 0
                              ? settings.gpuBudgetBytes
                              : static_cast<u64>(static_cast<double>(fullBytes) * settings.gpuBudgetFraction);
        }
        // Coarse fallback (--lod): built from the current mesh, resident in
        // full, and paid for out of the budget before the detailed pools. Only
        // a budget too small for the whole mesh can leave chunks without
        // detailed pages, so otherwise it is not allocated (unless forced for
        // seam tests).
        const bool budgetMissesChunks =
            budgetBytes > 0 && !ResidentMeshBuffers::planSlots(m_chunkMap, budgetBytes).holdsWholeMesh;
        const bool wantCoarse = settings.coarseFallback && (budgetMissesChunks || settings.coarseAlways);
        geo::CoarseChunks coarse;
        u64 coarseBytes = 0;
        double coarseBuildMs = 0.0;
        if (wantCoarse) {
            const core::Stopwatch buildTimer;
            core::JobSystem jobs(std::max(2u, std::thread::hardware_concurrency()) - 1u);
            coarse = geo::buildCoarseChunks(mesh, chunkShift, settings.coarseGroupShift, settings.coarseRatio, &jobs);
            coarseBuildMs = buildTimer.elapsedMs();
            coarseBytes = CoarseMeshBuffers::bytesFor(coarse, m_window->concurrentFrameCount());
        }
        u64 detailBudget = budgetBytes;
        if (budgetBytes > 0 && coarseBytes > 0) {
            detailBudget = budgetBytes > coarseBytes ? budgetBytes - coarseBytes : 1u;
        }
        const ResidentMeshBuffers::Config config{.budgetBytes = detailBudget,
                                                 .maxUploadBytesPerFrame = settings.maxUploadBytesPerFrame};
        if (const auto created = m_residentBuffers.create(m_context, m_chunkMap, m_window->concurrentFrameCount(), config,
                                                          m_pipeline.descriptorSetLayout());
            !created) {
            fatal(created.error());
        }
        if (budgetBytes == 0) {
            core::logInfo("GPU mesh storage: page pools sized for the whole mesh ({:.1f} MiB for one copy, {} spare vertex slots for edits)",
                          static_cast<double>(fullBytes) / kMiB, m_residentBuffers.vertexReserveSlots());
        } else {
            core::logInfo("GPU mesh storage: budget of {:.1f} MiB ({:.0f}% of the {:.1f} MiB a full copy needs)",
                          static_cast<double>(budgetBytes) / kMiB,
                          100.0 * static_cast<double>(budgetBytes) / static_cast<double>(fullBytes),
                          static_cast<double>(fullBytes) / kMiB);
        }
        if (settings.coarseFallback && !wantCoarse) {
            core::logInfo("  coarse fallback (--lod): not allocated, the pools hold the whole mesh");
        }
        if (wantCoarse) {
            const u32 coarseTriangles = static_cast<u32>(coarse.indices.size() / 3);
            const u32 coarseVertices = static_cast<u32>(coarse.vertices.size());
            const u32 groups = coarse.groupCount();
            // Qt enables every supported Vulkan 1.0 core feature (except
            // robustBufferAccess) when no features modifier is set, as here.
            VkPhysicalDeviceFeatures features{};
            instance->functions()->vkGetPhysicalDeviceFeatures(m_context.physicalDevice, &features);
            const VkPhysicalDeviceProperties* properties = m_window->physicalDeviceProperties();
            const bool indirect = settings.coarseIndirectDraws && features.multiDrawIndirect == VK_TRUE &&
                                  properties != nullptr && properties->limits.maxDrawIndirectCount >= groups;
            if (const auto created = m_coarseBuffers.create(m_context, std::move(coarse), settings.gpuPageShift,
                                                            mesh.vertexCount(), m_window->concurrentFrameCount(),
                                                            m_pipeline.descriptorSetLayout(), indirect);
                !created) {
                fatal(created.error());
            }
            m_coarseBuffers.uploadAll(mesh);
            core::logInfo("  coarse fallback: {} groups of {} chunks, {} triangles ({:.2f}% of the mesh), {} vertices + {} spare slots, "
                          "{:.2f} MiB on the GPU{}, {}, built in {:.0f} ms",
                          groups, 1u << settings.coarseGroupShift, coarseTriangles,
                          100.0 * coarseTriangles / std::max(1u, mesh.triangleCount()), coarseVertices,
                          m_coarseBuffers.spareSlots(), static_cast<double>(coarseBytes) / kMiB,
                          budgetBytes > 0 ? " (taken from the budget)" : "",
                          indirect ? "one indirect draw per pass" : "one draw per group", coarseBuildMs);
        }
        core::logInfo("  pools: {} of {} vertex pages, {} of {} chunks of indices; {:.1f} MiB allocated in {}",
                      m_residentBuffers.vertexSlots(), m_chunkMap.vertexPageCount(), m_residentBuffers.indexSlots(),
                      m_chunkMap.chunkCount(), static_cast<double>(m_residentBuffers.gpuMemoryBytes()) / kMiB,
                      m_residentBuffers.deviceLocal() ? "GPU memory mapped by the CPU" : "host memory (no mappable GPU memory)");
        // Pools that hold everything are filled now, so the first frame is
        // complete; a limited budget fills on demand from the first frame on.
        if (m_residentBuffers.holdsWholeMesh()) {
            const core::Stopwatch fillTimer;
            m_residentBuffers.uploadAll(mesh, m_chunkMap);
            core::logInfo("  whole mesh uploaded in {:.1f} ms", fillTimer.elapsedMs());
        }
        // Fresh pools hold current data or nothing, so earlier pending edits are covered.
        if (m_scene->pendingVertexUpdates != nullptr) {
            m_scene->pendingVertexUpdates->clear();
        }
    }
    core::logInfo("Chunks: {} of up to {} triangles, {} page references ({} GPU vertex pages)", m_chunkMap.chunkCount(),
                  m_chunkMap.chunkTriangleCapacity(), m_chunkMap.pageReferenceCount(), m_chunkMap.vertexPageCount());

    if (m_stats != nullptr) {
        m_stats->limitedBudget = budgetBytes > 0;
        m_stats->gpuTimingAvailable = timing;
        m_stats->gpuBudgetBytes = budgetBytes;
        m_stats->coarseBytes = m_coarseBuffers.valid() ? m_coarseBuffers.gpuMemoryBytes() : 0u;
        m_stats->coarseIndirectDraws = m_coarseBuffers.valid() && m_coarseBuffers.indirectDraws();
        m_stats->gpuBytesAllocated = m_residentBuffers.gpuMemoryBytes() + m_stats->coarseBytes;
        m_stats->gpuPageVertices = 1u << settings.gpuPageShift;
        m_stats->gpuPageCount = m_chunkMap.vertexPageCount();
        m_stats->chunkCount = m_chunkMap.chunkCount();
        m_stats->vertexSlots = m_residentBuffers.vertexSlots();
        m_stats->indexSlots = m_residentBuffers.indexSlots();
        m_stats->vertexReserveSlots = m_residentBuffers.vertexReserveSlots();
        m_stats->vertexPagesResident = m_residentBuffers.vertexPagesResident();
        m_stats->indexChunksResident = m_residentBuffers.indexChunksResident();
    }
    m_hasPreviousFrame = false;
}

void SceneRenderer::releaseResources()
{
    // Qt guarantees the device is idle here, so nothing is still in use.
    m_residentBuffers.destroy();
    m_coarseBuffers.destroy();
    m_gpuTimer.destroy();
    m_pipeline.destroy();
    m_context = VulkanContext{};
}

void SceneRenderer::selectChunks(const glm::mat4& viewProjection, const glm::vec3& eye)
{
    const u32 chunkTotal = m_chunkMap.chunkCount();
    m_visibleChunks.clear();

    // Current bounds, which follow every edit (they are the roots of the
    // chunks' local BVHs). Copied so nothing else needs the lock.
    const bool haveBounds = m_scene->chunks != nullptr && m_scene->chunks->chunkCount() == chunkTotal;
    const bool cull = haveBounds && m_scene->settings.cullMode != CullMode::Off;
    // Nearest first only matters when the pools cannot hold everything.
    const bool prioritize = haveBounds && !m_residentBuffers.holdsWholeMesh();
    if (cull || prioritize) {
        m_chunkBounds.resize(chunkTotal);
        for (u32 chunk = 0; chunk < chunkTotal; ++chunk) {
            m_chunkBounds[chunk] = m_scene->chunks->chunk(chunk).bounds();
        }
    }

    m_backFacing = 0;
    m_backFacingKeptStale = 0;
    if (cull) {
        // Back-facing chunks: the cones are computed on the sculpt thread in the
        // same refit as the bounds read above, so under this lock the two
        // describe the same mesh. A chunk whose GPU copy has not caught up with
        // that mesh yet is drawn whatever its cone says, because the older
        // content on the GPU may face the viewer.
        const bool backFacing = m_scene->chunks->hasNormalCones() &&
                                (m_scene->settings.cullMode == CullMode::On || m_scene->settings.cullMode == CullMode::OnUnmerged);
        const Frustum frustum = Frustum::fromViewProjection(viewProjection);
        for (u32 chunk = 0; chunk < chunkTotal; ++chunk) {
            if (!frustum.intersects(m_chunkBounds[chunk])) {
                continue;
            }
            if (backFacing && geo::coneFacesAway(m_scene->chunks->chunkCone(chunk), m_chunkBounds[chunk], eye)) {
                if (!m_residentBuffers.hasStaleCopy(m_chunkMap, chunk)) {
                    ++m_backFacing;
                    continue;
                }
                ++m_backFacingKeptStale;
            }
            m_visibleChunks.push_back(chunk);
        }
    } else {
        m_visibleChunks.resize(chunkTotal);
        std::iota(m_visibleChunks.begin(), m_visibleChunks.end(), 0u);
    }

    // Limited budget: nearest first, so that when not everything fits, what is
    // closest to the viewer gets memory and upload time first. With pools that
    // hold the whole mesh every visible chunk is drawn anyway, and ascending
    // order is kept. (Drawing order is decided later, by buildRuns.)
    // With the coarse fallback, a group is drawn coarse as a whole when any of
    // its visible chunks is missing, so detailed memory is spent group by
    // group: chunks are ordered by the distance of their group (nearest group
    // first, a group's chunks together), and the cut where the pools run out
    // falls between groups rather than inside many of them.
    if (prioritize) {
        m_chunkOrder.clear();
        if (m_coarseBuffers.valid()) {
            const geo::CoarseChunks& coarse = m_coarseBuffers.chunks();
            m_groupDistance.assign(coarse.groupCount(), -1.0f);
            for (const u32 chunk : m_visibleChunks) {
                float& distance = m_groupDistance[coarse.groupOf(chunk)];
                if (distance < 0.0f) {
                    const u32 first = coarse.firstChunk(coarse.groupOf(chunk));
                    const u32 last = std::min(chunkTotal, first + (1u << coarse.groupShift));
                    geo::Aabb bounds = m_chunkBounds[first];
                    for (u32 c = first + 1; c < last; ++c) {
                        bounds.expand(m_chunkBounds[c]);
                    }
                    const glm::vec3 toCenter = bounds.center() - eye;
                    distance = glm::dot(toCenter, toCenter);
                }
                // Ties (same group) keep ascending chunk order.
                m_chunkOrder.emplace_back(distance, chunk);
            }
        } else {
            for (const u32 chunk : m_visibleChunks) {
                const glm::vec3 toCenter = m_chunkBounds[chunk].center() - eye;
                m_chunkOrder.emplace_back(glm::dot(toCenter, toCenter), chunk);
            }
        }
        std::sort(m_chunkOrder.begin(), m_chunkOrder.end());
        for (std::size_t i = 0; i < m_chunkOrder.size(); ++i) {
            m_visibleChunks[i] = m_chunkOrder[i].second;
        }
    }
}

void SceneRenderer::startNextFrame()
{
    const double sincePreviousFrameMs = m_sinceFrameStart.elapsedMs();
    m_sinceFrameStart.reset();
    const core::Stopwatch frameTimer;
    QVulkanDeviceFunctions* df = m_context.deviceFunctions;
    const VkCommandBuffer commandBuffer = m_window->currentCommandBuffer();
    const int frameSlot = m_window->currentFrame();
    const QSize size = m_window->swapChainImageSize();

    const OrbitCamera& camera = *m_scene->camera;
    const float aspect = size.height() > 0 ? static_cast<float>(size.width()) / static_cast<float>(size.height()) : 1.0f;
    MeshFrameConstants constants;
    constants.viewProjection = camera.projection(aspect) * camera.view();
    constants.eyePosition = glm::vec4(camera.eye(), 1.0f);
    const BrushCursor& cursor = m_scene->cursor;
    constants.brushSphere = glm::vec4(cursor.center, cursor.visible ? cursor.radius : 0.0f);
    constants.brushColor = cursor.color;
    constants.params = glm::uvec4(m_chunkMap.gpuPageShift(), 0u, 0u, 0u);

    // 1. Everything that reads the mesh happens under one lock: taking the
    //    pending edits, reading chunk bounds and copying data. The buffers
    //    therefore always hold one consistent state of the mesh. The lock is
    //    fair: the wait is bounded by one dab of the sculpt thread.
    u32 uploadedVertices = 0;
    u64 uploadedBytes = 0;
    u32 evictions = 0;
    u32 stalePages = 0;
    u32 skippedChunks = 0;
    u32 visibleVertexPages = 0;
    bool pendingWork = false;
    double uploadMs = 0.0;
    double lockWaitMs = 0.0;
    double selectMs = 0.0;
    u64 coarseUploadBytes = 0;
    u32 coarseTriangles = 0;
    u32 coarseStale = 0;
    u32 coarseReplaced = 0;
    m_readyChunks.clear();
    {
        const core::Stopwatch lockTimer;
        const std::unique_lock<core::FairMutex> meshLock = lockMesh(*m_scene);
        lockWaitMs = lockTimer.elapsedMs();

        if (m_scene->pendingVertexUpdates != nullptr) {
            m_residentBuffers.markDirty(*m_scene->pendingVertexUpdates);
            if (m_coarseBuffers.valid()) {
                m_coarseBuffers.markDirty(*m_scene->pendingVertexUpdates);
            }
            m_scene->pendingVertexUpdates->clear();
        }
        const core::Stopwatch selectTimer;
        selectChunks(constants.viewProjection, camera.eye());
        selectMs = selectTimer.elapsedMs();

        const core::Stopwatch uploadTimer;
        // Coarse fallback: forced coarse groups (seam tests) skip the detailed
        // path; a group with a visible chunk that cannot be drawn detailed is
        // drawn coarse as a whole, and its other chunks are not drawn detailed.
        m_coarseChunks.clear();
        m_coarseGroups.clear();
        std::span<const u32> detailRequest = m_visibleChunks;
        const bool coarse = m_coarseBuffers.valid();
        const geo::CoarseChunks* coarseChunks = coarse ? &m_coarseBuffers.chunks() : nullptr;
        if (coarse && m_scene->settings.coarseDebug != 0) {
            m_detailChunks.clear();
            for (const u32 chunk : m_visibleChunks) {
                const bool forced = m_scene->settings.coarseDebug >= 2 || (coarseChunks->groupOf(chunk) % 2u) == 1u;
                (forced ? m_coarseChunks : m_detailChunks).push_back(chunk);
            }
            detailRequest = m_detailChunks;
        }
        const ResidentMeshBuffers::FrameResult result = m_residentBuffers.prepare(
            frameSlot, *m_scene->mesh, m_chunkMap, detailRequest, m_readyChunks, coarse ? &m_coarseChunks : nullptr);
        if (coarse) {
            m_groupCoarse.resize(coarseChunks->groupCount(), 0u);
            for (const u32 chunk : m_coarseChunks) {
                u8& flag = m_groupCoarse[coarseChunks->groupOf(chunk)];
                if (flag == 0u) {
                    flag = 1u;
                    m_coarseGroups.push_back(coarseChunks->groupOf(chunk));
                }
            }
            if (!m_coarseGroups.empty()) {
                // Ready chunks of a coarse group are covered by its proxy.
                const auto replaced = std::ranges::remove_if(m_readyChunks, [&](u32 chunk) {
                    return m_groupCoarse[coarseChunks->groupOf(chunk)] != 0u;
                });
                coarseReplaced = static_cast<u32>(replaced.size());
                m_coarseChunks.insert(m_coarseChunks.end(), replaced.begin(), replaced.end());
                m_readyChunks.erase(replaced.begin(), replaced.end());
            }
            for (const u32 group : m_coarseGroups) {
                m_groupCoarse[group] = 0u;
            }
            const CoarseMeshBuffers::FrameResult coarseResult = m_coarseBuffers.prepare(frameSlot, *m_scene->mesh, m_coarseGroups);
            coarseUploadBytes = coarseResult.uploadBytes;
            coarseTriangles = coarseResult.triangles;
            coarseStale = coarseResult.stale;
            if (m_scene->settings.coarseDebug >= 2 && m_stats != nullptr) {
                m_stats->coarseMismatchedVertices = m_coarseBuffers.countMismatches(*m_scene->mesh, m_coarseGroups);
            }
        }
        uploadedVertices = result.uploadVertexCount;
        uploadedBytes = result.uploadBytes + coarseUploadBytes;
        evictions = result.evictions;
        stalePages = result.stalePages;
        // Coarse groups drawn stale (no spare slot free) catch up next frames.
        pendingWork = result.pendingWork || coarseStale > 0;
        skippedChunks = result.skippedChunks;
        visibleVertexPages = result.visibleVertexPages;
        uploadMs = uploadTimer.elapsedMs();
    }

    // 2. Record the frame.
    const std::optional<double> gpuMs = m_gpuTimer.beginFrame(commandBuffer, frameSlot);

    std::array<VkClearValue, 3> clearValues{};
    clearValues[0].color = kBackground;
    clearValues[1].depthStencil = {1.0f, 0};
    clearValues[2].color = kBackground; // MSAA colour attachment, when enabled

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = m_window->defaultRenderPass();
    passInfo.framebuffer = m_window->currentFramebuffer();
    passInfo.renderArea.extent = {static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height())};
    passInfo.clearValueCount = m_window->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT ? 3u : 2u;
    passInfo.pClearValues = clearValues.data();
    df->vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(size.width()), static_cast<float>(size.height()),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, passInfo.renderArea.extent};
    df->vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    df->vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    m_pipeline.bind(commandBuffer, constants);

    u32 drawCalls = 0;
    u32 triangles = 0;
    for (const u32 chunk : m_readyChunks) {
        triangles += m_chunkMap.chunkTriangles(chunk);
    }
    // Chunks in consecutive index slots are drawn with one call; OnUnmerged
    // keeps one call per chunk to measure what merging saves.
    m_residentBuffers.buildRuns(m_chunkMap, m_readyChunks, m_scene->settings.cullMode != CullMode::OnUnmerged, m_runs);
    // drawRepeat > 1 only while measuring (see RenderSettings); stats count one pass.
    const u32 passes = std::max(1u, m_scene->settings.drawRepeat);
    const bool drawCoarse = m_coarseBuffers.valid() && !m_coarseChunks.empty();
    for (u32 pass = 0; pass < passes; ++pass) {
        if (pass > 0 && drawCoarse) {
            m_pipeline.bind(commandBuffer, constants);
        }
        drawCalls = m_residentBuffers.draw(commandBuffer, frameSlot, m_pipeline.layout(), m_runs);
        if (drawCoarse) {
            m_pipeline.bindCoarse(commandBuffer, constants);
            drawCalls += m_coarseBuffers.draw(commandBuffer, frameSlot, m_pipeline.layout());
        }
    }
    triangles += coarseTriangles;

    df->vkCmdEndRenderPass(commandBuffer);
    m_gpuTimer.endFrame(commandBuffer, frameSlot);

    if (m_stats != nullptr) {
        RenderStats& stats = *m_stats;
        stats.lastFrameCpuMs = frameTimer.elapsedMs();
        stats.lastMeshLockWaitMs = lockWaitMs;
        stats.worstMeshLockWaitMs = std::max(stats.worstMeshLockWaitMs, lockWaitMs);
        if (uploadedBytes > 0) {
            stats.lastUploadMs = uploadMs;
            stats.lastUploadVertexCount = uploadedVertices;
        }
        stats.lastUploadBytes = uploadedBytes;
        stats.totalUploadBytes += uploadedBytes;
        stats.lastEvictions = evictions;
        stats.totalEvictions += evictions;
        stats.lastStalePages = stalePages;
        stats.chunksSkipped = skippedChunks;
        stats.visibleVertexPages = visibleVertexPages;
        stats.pendingWork = pendingWork;
        stats.chunksVisible = static_cast<u32>(m_visibleChunks.size());
        stats.chunksDrawn = static_cast<u32>(m_readyChunks.size());
        stats.trianglesSubmitted = triangles;
        stats.drawCalls = drawCalls;
        stats.chunksCoarse = static_cast<u32>(m_coarseChunks.size());
        stats.chunksMissing = static_cast<u32>(m_visibleChunks.size()) - stats.chunksCoarse - static_cast<u32>(m_readyChunks.size());
        stats.coarseTriangles = coarseTriangles;
        stats.coarseGroups = static_cast<u32>(m_coarseGroups.size());
        stats.coarseReplacedDetailed = coarseReplaced;
        stats.lastCoarseUploadBytes = coarseUploadBytes;
        stats.backFacingChunks = m_backFacing;
        stats.backFacingKeptStale = m_backFacingKeptStale;
        stats.selectMs = selectMs;
        stats.vertexPagesResident = m_residentBuffers.vertexPagesResident();
        stats.indexChunksResident = m_residentBuffers.indexChunksResident();
        if (gpuMs) {
            stats.lastGpuMs = *gpuMs;
        }
        ++stats.frameCount;
        if (stats.recordSamples) {
            stats.frameCpuSamplesMs.push_back(static_cast<float>(stats.lastFrameCpuMs));
            stats.lockWaitSamplesMs.push_back(static_cast<float>(lockWaitMs));
            stats.uploadVertexSamples.push_back(uploadedVertices);
            stats.uploadKiBSamples.push_back(static_cast<u32>(uploadedBytes / 1024u));
            stats.chunksDrawnSamples.push_back(stats.chunksDrawn);
            stats.trianglesSubmittedSamples.push_back(stats.trianglesSubmitted);
            stats.backFacingSamples.push_back(stats.backFacingChunks);
            stats.chunksCoarseSamples.push_back(stats.chunksCoarse);
            stats.chunksMissingSamples.push_back(stats.chunksMissing);
            stats.evictionSamples.push_back(evictions);
            if (gpuMs) {
                stats.frameGpuSamplesMs.push_back(static_cast<float>(*gpuMs));
            }
            if (m_hasPreviousFrame) {
                stats.frameIntervalSamplesMs.push_back(static_cast<float>(sincePreviousFrameMs));
            }
        }
    }
    m_hasPreviousFrame = true;

    // 3. Submit and present. Rendering is on demand: the editor requests a new
    //    frame when the camera, the cursor or the mesh changed. The one case the
    //    renderer asks by itself is when uploads were held back by the per-frame
    //    limit, or pages were drawn stale, and more frames are needed to catch up.
    m_window->frameReady();
    if (pendingWork) {
        m_window->requestUpdate();
    }
}

} // namespace se::render
