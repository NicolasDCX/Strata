#include "Sculpt/StrokeProcessor.h"

#include "Core/Assert.h"
#include "Sculpt/Falloff.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace se::sculpt {

namespace {

/// Advances a per-vertex generation counter, resetting the stamps on wrap-around.
void nextGeneration(u32& generation, std::vector<u32>& stamps)
{
    if (++generation == 0) {
        std::fill(stamps.begin(), stamps.end(), 0u);
        generation = 1;
    }
}

constexpr u32 kNoCorner = std::numeric_limits<u32>::max();
/// Triangles per block of the parallel vertex selection. Blocks are fixed, so
/// the joined result does not depend on the number of threads.
constexpr std::size_t kGatherBlockTriangles = 2048;
/// Smaller dabs gather on the calling thread, where the tasks would cost more
/// than they save: the query below this many chunks hit, the vertex
/// selection below this many triangles.
constexpr std::size_t kParallelQueryMinChunks = 8;
constexpr std::size_t kParallelSelectMinTriangles = 32768;

} // namespace

StrokeProcessor::StrokeProcessor(geo::Mesh& mesh, geo::ChunkedBvh& bvh)
    : m_mesh(mesh),
      m_bvh(bvh),
      m_dirty(mesh.makeDirtySet()),
      m_gatherStamp(mesh.vertexCount(), 0),
      m_recordStamp(mesh.vertexCount(), 0),
      m_staging(mesh)
{
}

std::size_t StrokeProcessor::editScratchBytes() const noexcept
{
    return m_region.memoryBytes() + m_staging.memoryBytes() + m_newNormals.capacity() * sizeof(glm::vec3) +
           parallelGatherScratchBytes() +
           m_bvh.markScratchBytes();
}

std::size_t StrokeProcessor::parallelGatherScratchBytes() const noexcept
{
    std::size_t bytes = m_queryChunks.capacity() * sizeof(u32) + m_offsets.capacity() * sizeof(std::size_t) +
                        m_chunkTriangles.capacity() * sizeof(std::vector<u32>) +
                        m_blocks.capacity() * sizeof(GatherBlock);
    for (const std::vector<u32>& triangles : m_chunkTriangles) {
        bytes += triangles.capacity() * sizeof(u32);
    }
    for (const GatherBlock& block : m_blocks) {
        bytes += (block.inside.capacity() + block.outside.capacity()) * sizeof(u32) +
                 block.weights.capacity() * sizeof(float);
    }
    return bytes;
}

bool StrokeProcessor::begin(const Brush& brush, const BrushSettings& settings, const geo::Ray& ray)
{
    SE_ASSERT(!active());
    SE_ASSERT(settings.radius > 0.0f);

    const std::optional<geo::RayHit> hit = m_bvh.raycast(m_mesh, ray);
    if (!hit) {
        return false;
    }

    m_brush = &brush;
    m_settings = settings;
    m_delta = StrokeDelta{};
    m_stats = StrokeStats{};
    nextGeneration(m_strokeGeneration, m_recordStamp);

    if (brush.mode() == StrokeMode::Grab) {
        gather(hit->position, m_settings.radius);
        recordForUndo();
        const auto positions = m_mesh.positions();
        m_grabOriginals.resize(m_vertices.size());
        for (std::size_t i = 0; i < m_vertices.size(); ++i) {
            m_grabOriginals[i] = positions[m_vertices[i]];
        }
        m_grabOrigin = hit->position;
        // Drag in the plane facing the viewer, through the grabbed point.
        m_grabPlaneNormal = -ray.direction;
        m_grabSurfaceNormal = averageNormal(hit->normal);
        m_lastCenter = hit->position;
    } else {
        applyDab(*hit);
        m_lastRay = ray;
        m_lastHitPosition = hit->position;
        m_hasLastHit = true;
        m_distanceSinceDab = 0.0f;
    }
    return true;
}

void StrokeProcessor::update(const geo::Ray& ray)
{
    if (!active()) {
        return;
    }
    if (m_brush->mode() == StrokeMode::Grab) {
        applyGrab(ray);
        return;
    }

    const std::optional<geo::RayHit> hit = m_bvh.raycast(m_mesh, ray);
    if (!hit) {
        // Pointer left the surface: pause, and restart spacing when it returns.
        m_hasLastHit = false;
        return;
    }
    if (!m_hasLastHit) {
        applyDab(*hit);
        m_lastRay = ray;
        m_lastHitPosition = hit->position;
        m_hasLastHit = true;
        m_distanceSinceDab = 0.0f;
        return;
    }

    // Consistent sampling: emit one dab every `spacing` world units travelled
    // over the surface, independent of how often pointer events arrive.
    const float spacing = std::max(m_settings.radius * kDabSpacing, 1e-6f);
    const float segmentLength = glm::distance(hit->position, m_lastHitPosition);
    if (segmentLength > 0.0f) {
        const float travelled = m_distanceSinceDab + segmentLength;
        const u32 dabs = std::min(static_cast<u32>(travelled / spacing), kMaxDabsPerUpdate);
        for (u32 i = 1; i <= dabs; ++i) {
            const float along = (static_cast<float>(i) * spacing - m_distanceSinceDab) / segmentLength;
            // Interpolate the pointer ray rather than the hit point, then re-cast,
            // so every dab lands on the (already deformed) surface.
            geo::Ray sample;
            sample.origin = glm::mix(m_lastRay.origin, ray.origin, along);
            sample.direction = glm::normalize(glm::mix(m_lastRay.direction, ray.direction, along));
            if (const std::optional<geo::RayHit> sampleHit = m_bvh.raycast(m_mesh, sample)) {
                applyDab(*sampleHit);
            }
        }
        m_distanceSinceDab = travelled - static_cast<float>(dabs) * spacing;
        if (dabs == kMaxDabsPerUpdate) {
            m_distanceSinceDab = 0.0f;
        }
    }
    m_lastRay = ray;
    m_lastHitPosition = hit->position;
}

std::optional<StrokeDelta> StrokeProcessor::end()
{
    if (!active()) {
        return std::nullopt;
    }
    m_brush = nullptr;
    m_hasLastHit = false;

    if (m_delta.empty()) {
        return std::nullopt;
    }
    const auto positions = m_mesh.positions();
    m_delta.after.resize(m_delta.vertices.size());
    for (std::size_t i = 0; i < m_delta.vertices.size(); ++i) {
        m_delta.after[i] = positions[m_delta.vertices[i]];
    }
    return std::exchange(m_delta, StrokeDelta{});
}

void StrokeProcessor::commit()
{
    core::Stopwatch timer;
    if (m_commitEnter) {
        m_commitEnter();
    }
    m_stats.commitWaitMs += timer.elapsedMs();
    timer.reset();

    // Everything below is what a reader must see together.
    const auto positions = m_mesh.mutablePositions();
    const geo::PagedArrayView<const glm::vec3> staged = stagedPositions();
    core::parallelFor(m_jobs, m_vertices.size(), kBrushParallelGrain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            positions[m_vertices[i]] = staged[m_vertices[i]];
        }
    });
    const std::span<const u32> affected = m_region.vertices();
    const auto normals = m_mesh.mutableNormals();
    core::parallelFor(m_jobs, affected.size(), kBrushParallelGrain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            normals[affected[i]] = m_newNormals[i];
        }
    });
    m_stats.publishMs += timer.elapsedMs();
    timer.reset();
    m_bvh.finishMarkedRefit(m_mesh, m_jobs);
    m_stats.bvhMs += timer.elapsedMs();
    m_stats.coneTriangles += m_bvh.lastConeTriangles();
    m_stats.refit.markMs += m_bvh.lastRefitTimings().markMs;
    m_stats.refit.localMs += m_bvh.lastRefitTimings().localMs;
    m_stats.refit.topMs += m_bvh.lastRefitTimings().topMs;
    m_stats.refit.boxCpuMs += m_bvh.lastRefitTimings().boxCpuMs;
    m_stats.refit.coneCpuMs += m_bvh.lastRefitTimings().coneCpuMs;
    m_stats.refit.chunks += m_bvh.lastRefitTimings().chunks;
    m_stats.refit.conesExact += m_bvh.lastRefitTimings().conesExact;
    m_stats.refit.conesWidened += m_bvh.lastRefitTimings().conesWidened;
    m_stats.refit.conesDisabled += m_bvh.lastRefitTimings().conesDisabled;
    m_stats.refit.widenTriangles += m_bvh.lastRefitTimings().widenTriangles;
    m_stats.refit.conePass1CpuMs += m_bvh.lastRefitTimings().conePass1CpuMs;
    m_stats.refit.conePass2CpuMs += m_bvh.lastRefitTimings().conePass2CpuMs;
    m_stats.refit.conePass2Triangles += m_bvh.lastRefitTimings().conePass2Triangles;

    if (m_commitLeave) {
        m_commitLeave();
    }
    releasePages();
}

void StrokeProcessor::takeDabSamples(std::vector<float>& destination)
{
    destination.insert(destination.end(), m_dabSamples.begin(), m_dabSamples.end());
    m_dabSamples.clear();
}

void StrokeProcessor::drainDirty(geo::DirtyVertexSet& destination)
{
    m_mesh.invalidatePageBounds(m_dirty);
    destination.merge(m_dirty);
    m_dirty.clear();
}

void StrokeProcessor::setParallelGather(bool enabled)
{
    SE_ASSERT(!active());
    if (enabled == m_parallelGather) {
        return;
    }
    m_parallelGather = enabled;
    // The stamp array changes meaning (see m_gatherStamp).
    std::fill(m_gatherStamp.begin(), m_gatherStamp.end(), enabled ? kNoCorner : 0u);
    m_gatherGeneration = 0;
}

void StrokeProcessor::gatherParallel(const glm::vec3& center, float radius)
{
    m_triangles.clear();
    m_vertices.clear();
    m_weights.clear();

    // 1. Sphere query, one chunk per task, joined in the serial query's order.
    core::Stopwatch timer;
    m_queryChunks.clear();
    m_bvh.querySphereChunks(center, radius, m_queryChunks);
    const std::size_t chunkHits = m_queryChunks.size();
    const bool helpers = m_jobs != nullptr && m_jobs->workerCount() > 0;
    if (!helpers || chunkHits < kParallelQueryMinChunks) {
        for (const u32 chunk : m_queryChunks) {
            m_bvh.chunk(chunk).querySphere(center, radius, m_triangles);
        }
    } else {
        if (m_chunkTriangles.size() < chunkHits) {
            m_chunkTriangles.resize(chunkHits);
        }
        core::parallelFor(m_jobs, chunkHits, 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                m_chunkTriangles[i].clear();
                m_bvh.chunk(m_queryChunks[i]).querySphere(center, radius, m_chunkTriangles[i]);
            }
        });
        m_offsets.resize(chunkHits + 1);
        m_offsets[0] = 0;
        for (std::size_t i = 0; i < chunkHits; ++i) {
            m_offsets[i + 1] = m_offsets[i] + m_chunkTriangles[i].size();
        }
        m_triangles.resize(m_offsets[chunkHits]);
        core::parallelFor(m_jobs, chunkHits, 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                std::copy(m_chunkTriangles[i].begin(), m_chunkTriangles[i].end(),
                          m_triangles.begin() + static_cast<std::ptrdiff_t>(m_offsets[i]));
            }
        });
    }
    m_stats.gather.queryMs += timer.elapsedMs();
    timer.reset();
    if (!helpers || m_triangles.size() < kParallelSelectMinTriangles) {
        // Too small to share out (or nobody to share with): same result, one thread.
        selectFirstCornersSerial(center, radius);
        m_stats.gather.selectMs += timer.elapsedMs();
        return;
    }

    core::Stopwatch step;
    // 2. Every vertex's first corner in the triangle list (corner k of
    //    triangle t is 3t + k). The serial gather tests a vertex there and
    //    nowhere else, so selecting first corners gives its list and order.
    const std::size_t triangleCount = m_triangles.size();
    core::parallelFor(m_jobs, triangleCount, 4096, [&](std::size_t begin, std::size_t end) {
        for (std::size_t t = begin; t < end; ++t) {
            const auto& corners = m_mesh.triangle(m_triangles[t]);
            for (u32 c = 0; c < 3; ++c) {
                const u32 corner = static_cast<u32>(3 * t + c);
                std::atomic_ref<u32> first(m_gatherStamp[corners[c]]);
                u32 current = first.load(std::memory_order_relaxed);
                while (corner < current && !first.compare_exchange_weak(current, corner, std::memory_order_relaxed)) {
                }
            }
        }
    });

    m_stats.gather.selectFirstMs += step.elapsedMs();
    step.reset();
    // 3. Test the first occurrences, block by block, then join the blocks in order.
    const auto positions = m_mesh.positions();
    const float radiusSquared = radius * radius;
    const float invRadiusSquared = 1.0f / radiusSquared;
    const std::size_t blockCount = (triangleCount + kGatherBlockTriangles - 1) / kGatherBlockTriangles;
    if (m_blocks.size() < blockCount) {
        m_blocks.resize(blockCount);
    }
    core::parallelFor(m_jobs, blockCount, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t b = begin; b < end; ++b) {
            GatherBlock& block = m_blocks[b];
            block.inside.clear();
            block.weights.clear();
            block.outside.clear();
            const std::size_t last = std::min(triangleCount, (b + 1) * kGatherBlockTriangles);
            for (std::size_t t = b * kGatherBlockTriangles; t < last; ++t) {
                const auto& corners = m_mesh.triangle(m_triangles[t]);
                for (u32 c = 0; c < 3; ++c) {
                    const u32 vertex = corners[c];
                    if (m_gatherStamp[vertex] != static_cast<u32>(3 * t + c)) {
                        continue; // not this vertex's first corner
                    }
                    const glm::vec3 offset = positions[vertex] - center;
                    const float distanceSquared = glm::dot(offset, offset);
                    if (distanceSquared < radiusSquared) {
                        block.inside.push_back(vertex);
                        block.weights.push_back(smoothFalloff(distanceSquared * invRadiusSquared));
                    } else {
                        block.outside.push_back(vertex);
                    }
                }
            }
        }
    });
    m_stats.gather.selectTestMs += step.elapsedMs();
    step.reset();
    m_offsets.resize(blockCount + 1);
    m_offsets[0] = 0;
    for (std::size_t b = 0; b < blockCount; ++b) {
        m_offsets[b + 1] = m_offsets[b] + m_blocks[b].inside.size();
    }
    m_vertices.resize(m_offsets[blockCount]);
    m_weights.resize(m_offsets[blockCount]);
    // Each vertex is in exactly one block's lists, so resetting is race free.
    core::parallelFor(m_jobs, blockCount, 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t b = begin; b < end; ++b) {
            const GatherBlock& block = m_blocks[b];
            const auto at = static_cast<std::ptrdiff_t>(m_offsets[b]);
            std::copy(block.inside.begin(), block.inside.end(), m_vertices.begin() + at);
            std::copy(block.weights.begin(), block.weights.end(), m_weights.begin() + at);
            for (const u32 vertex : block.inside) {
                m_gatherStamp[vertex] = kNoCorner;
            }
            for (const u32 vertex : block.outside) {
                m_gatherStamp[vertex] = kNoCorner;
            }
        }
    });
    m_stats.gather.selectJoinMs += step.elapsedMs();
    m_stats.gather.selectMs += timer.elapsedMs();
}

void StrokeProcessor::selectFirstCornersSerial(const glm::vec3& center, float radius)
{
    // The serial gather's loop, with m_gatherStamp in the parallel gather's
    // meaning: anything but kNoCorner marks a vertex seen. Seen vertices go back
    // to kNoCorner at the end.
    const auto positions = m_mesh.positions();
    const float radiusSquared = radius * radius;
    const float invRadiusSquared = 1.0f / radiusSquared;
    if (m_blocks.empty()) {
        m_blocks.resize(1);
    }
    std::vector<u32>& outside = m_blocks[0].outside;
    outside.clear();
    for (const u32 triangle : m_triangles) {
        for (const u32 vertex : m_mesh.triangle(triangle)) {
            if (m_gatherStamp[vertex] != kNoCorner) {
                continue;
            }
            m_gatherStamp[vertex] = 0;
            const glm::vec3 offset = positions[vertex] - center;
            const float distanceSquared = glm::dot(offset, offset);
            if (distanceSquared < radiusSquared) {
                m_vertices.push_back(vertex);
                m_weights.push_back(smoothFalloff(distanceSquared * invRadiusSquared));
            } else {
                outside.push_back(vertex);
            }
        }
    }
    for (const u32 vertex : m_vertices) {
        m_gatherStamp[vertex] = kNoCorner;
    }
    for (const u32 vertex : outside) {
        m_gatherStamp[vertex] = kNoCorner;
    }
}

void StrokeProcessor::gather(const glm::vec3& center, float radius)
{
    if (m_parallelGather) {
        gatherParallel(center, radius);
        return;
    }
    m_triangles.clear();
    m_vertices.clear();
    m_weights.clear();
    core::Stopwatch timer;
    m_bvh.querySphere(center, radius, m_triangles);
    m_stats.gather.queryMs += timer.elapsedMs();
    timer.reset();
    nextGeneration(m_gatherGeneration, m_gatherStamp);

    const auto positions = m_mesh.positions();
    const float radiusSquared = radius * radius;
    const float invRadiusSquared = 1.0f / radiusSquared;

    for (const u32 triangle : m_triangles) {
        for (const u32 vertex : m_mesh.triangle(triangle)) {
            if (m_gatherStamp[vertex] == m_gatherGeneration) {
                continue; // already tested through another triangle
            }
            m_gatherStamp[vertex] = m_gatherGeneration;
            const glm::vec3 offset = positions[vertex] - center;
            const float distanceSquared = glm::dot(offset, offset);
            if (distanceSquared < radiusSquared) {
                m_vertices.push_back(vertex);
                m_weights.push_back(smoothFalloff(distanceSquared * invRadiusSquared));
            }
        }
    }
    m_stats.gather.selectMs += timer.elapsedMs();
}

void StrokeProcessor::recordForUndo()
{
    // Serial in both gathers: it is a small share of the gather (0.07 ms at
    // 5.2M, 0.35 ms at 21M) and a blocked version was slower.
    const core::Stopwatch timer;
    const auto positions = m_mesh.positions();
    for (const u32 vertex : m_vertices) {
        if (m_recordStamp[vertex] != m_strokeGeneration) {
            m_recordStamp[vertex] = m_strokeGeneration;
            m_delta.vertices.push_back(vertex);
            m_delta.before.push_back(positions[vertex]);
        }
    }
    m_stats.gather.recordMs += timer.elapsedMs();
}

glm::vec3 StrokeProcessor::averageNormal(const glm::vec3& fallback) const noexcept
{
    const auto normals = m_mesh.normals();
    glm::vec3 sum(0.0f);
    for (std::size_t i = 0; i < m_vertices.size(); ++i) {
        sum += normals[m_vertices[i]] * m_weights[i];
    }
    const float lengthSquared = glm::dot(sum, sum);
    return lengthSquared > 1e-12f ? sum / std::sqrt(lengthSquared) : fallback;
}

void StrokeProcessor::applyDab(const geo::RayHit& hit)
{
    m_lastCenter = hit.position;
    m_dabTimer.reset();
    core::Stopwatch timer;
    gather(hit.position, m_settings.radius);
    if (m_vertices.empty()) {
        return;
    }
    // Undo record, average normal and page staging read the gathered lists
    // and the mesh, which nothing changes before commit, and each writes its
    // own state, so they can run at the same time.
    glm::vec3 normal;
    const auto normalStep = [&] {
        const core::Stopwatch normalTimer;
        normal = averageNormal(hit.normal);
        m_stats.gather.normalMs += normalTimer.elapsedMs();
    };
    const double stagedBefore = m_stats.stageMs;
    const core::Stopwatch prepare;
    const bool overlap = m_overlapPrepare && m_jobs != nullptr && m_jobs->workerCount() > 0 &&
                         m_vertices.size() >= kOverlapPrepareMinVertices;
    if (overlap) {
        core::parallelFor(m_jobs, 3, 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t step = begin; step < end; ++step) {
                switch (step) {
                case 0: normalStep(); break;
                case 1: recordForUndo(); break;
                default: stagePages(); break;
                }
            }
        });
        ++m_stats.gather.overlappedDabs;
    } else {
        recordForUndo();
        stagePages();
        normalStep();
    }
    const double prepareMs = prepare.elapsedMs();
    m_stats.gather.prepareWallMs += prepareMs;
    // gatherMs leaves out the staging; overlapped, staging costs no extra time.
    const double stagingInGather = overlap ? 0.0 : m_stats.stageMs - stagedBefore;
    const DabContext ctx{
        .mesh = m_mesh,
        .positions = stagedPositions(),
        .vertices = m_vertices,
        .weights = m_weights,
        .center = hit.position,
        .normal = normal,
        .radius = m_settings.radius,
        .strength = m_settings.strength,
        .direction = m_settings.invert ? -1.0f : 1.0f,
        .dragDelta = glm::vec3(0.0f),
        .originalPositions = {},
        .scratch = m_scratch,
        .jobs = m_jobs,
    };
    m_stats.gatherMs += timer.elapsedMs() - stagingInGather;
    timer.reset();
    m_brush->apply(ctx);
    m_stats.brushMs += timer.elapsedMs();
    finishEdit();
}

bool StrokeProcessor::grabActive() const noexcept
{
    return m_brush != nullptr && m_brush->mode() == StrokeMode::Grab;
}

bool StrokeProcessor::grabUpdateApplies(const geo::Ray& ray) const noexcept
{
    return grabActive() && grabPlaneDistance(ray).has_value();
}

std::optional<float> StrokeProcessor::grabPlaneDistance(const geo::Ray& ray) const noexcept
{
    if (m_vertices.empty()) {
        return std::nullopt;
    }
    // Intersect the pointer ray with the drag plane fixed at begin().
    const float denominator = glm::dot(ray.direction, m_grabPlaneNormal);
    if (std::abs(denominator) < 1e-6f) {
        return std::nullopt;
    }
    const float t = glm::dot(m_grabOrigin - ray.origin, m_grabPlaneNormal) / denominator;
    if (t <= 0.0f) {
        return std::nullopt;
    }
    return t;
}

void StrokeProcessor::applyGrab(const geo::Ray& ray)
{
    const std::optional<float> t = grabPlaneDistance(ray);
    if (!t) {
        return;
    }
    m_dabTimer.reset();
    const glm::vec3 delta = ray.at(*t) - m_grabOrigin;
    m_lastCenter = m_grabOrigin + delta * m_settings.strength;
    stagePages();

    const DabContext ctx{
        .mesh = m_mesh,
        .positions = stagedPositions(),
        .vertices = m_vertices,
        .weights = m_weights,
        .center = m_grabOrigin,
        .normal = m_grabSurfaceNormal,
        .radius = m_settings.radius,
        .strength = m_settings.strength,
        .direction = m_settings.invert ? -1.0f : 1.0f,
        .dragDelta = delta,
        .originalPositions = m_grabOriginals,
        .scratch = m_scratch,
        .jobs = m_jobs,
    };
    const core::Stopwatch timer;
    m_brush->apply(ctx);
    m_stats.brushMs += timer.elapsedMs();
    finishEdit();
}

void StrokeProcessor::stagePages()
{
    const core::Stopwatch timer;
    m_staging.stage(m_vertices);
    m_stats.stageMs += timer.elapsedMs();
}

void StrokeProcessor::releasePages()
{
    m_staging.release();
}

geo::PagedArrayView<glm::vec3> StrokeProcessor::stagedPositions() noexcept
{
    return m_staging.view();
}

void StrokeProcessor::finishEdit()
{
    // Prepare, last step: the normals the proposed positions give. One walk
    // over the adjacency finds what both the normals and the BVH need.
    core::Stopwatch timer;
    core::Stopwatch step;
    m_region.collect(m_mesh, m_vertices, m_parallelRegion ? m_jobs : nullptr);
    const std::span<const u32> affected = m_region.vertices();
    m_stats.normals.regionMs += step.elapsedMs();
    m_stats.normals.parallelRegions += m_region.lastWasParallel() ? 1u : 0u;
    step.reset();
    m_newNormals.resize(affected.size());
    const geo::PagedArrayView<const glm::vec3> staged = stagedPositions();
    const std::span<glm::vec3> normals(m_newNormals);
    std::atomic<double> computeCpuMs{0.0};
    core::parallelFor(m_jobs, affected.size(), 4096, [&](std::size_t begin, std::size_t end) {
        const core::Stopwatch task;
        m_mesh.computeNormals(affected.subspan(begin, end - begin), staged, normals.subspan(begin, end - begin));
        computeCpuMs.fetch_add(task.elapsedMs(), std::memory_order_relaxed);
    });
    m_stats.normals.computeMs += step.elapsedMs();
    m_stats.normals.computeCpuMs += computeCpuMs.load(std::memory_order_relaxed);
    step.reset();
    for (const u32 vertex : affected) {
        m_dirty.mark(vertex);
    }
    m_stats.normals.dirtyMs += step.elapsedMs();
    m_stats.normals.affected += affected.size();
    m_stats.normals.moved += m_vertices.size();
    m_stats.normals.triangles += m_region.triangles().size();
    m_stats.normalsMs += timer.elapsedMs();
    // Queue the BVH leaves to refit: invisible to readers, so no lock yet.
    timer.reset();
    m_bvh.markTriangles(m_region.triangles(), m_parallelMarking ? m_jobs : nullptr);
    m_stats.bvhMs += timer.elapsedMs();

    commit();

    ++m_stats.dabCount;
    m_stats.lastDabVertexCount = static_cast<u32>(m_vertices.size());
    m_stats.maxDabVertexCount = std::max(m_stats.maxDabVertexCount, m_stats.lastDabVertexCount);
    if (m_dabSamples.size() < 100000) {
        m_dabSamples.push_back(static_cast<float>(m_dabTimer.elapsedMs()));
    }
}

} // namespace se::sculpt
