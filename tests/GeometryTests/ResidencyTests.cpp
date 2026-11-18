// Chunk culling and GPU page residency bookkeeping, tested without a GPU:
// Frustum, ChunkPageMap and PagePool contain no Vulkan code.

#include "Geometry/ChunkedBvh.h"
#include "Geometry/Icosphere.h"
#include "Renderer/ChunkPageMap.h"
#include "Renderer/Frustum.h"
#include "Renderer/PagePool.h"

#include <gtest/gtest.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <deque>
#include <random>
#include <set>
#include <vector>

using namespace se;

namespace {

glm::mat4 viewProjection(const glm::vec3& eye, const glm::vec3& target, float fovDegrees = 45.0f)
{
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(fovDegrees), 1.6f, 0.05f, 50.0f);
    projection[1][1] *= -1.0f; // Vulkan Y flip, as OrbitCamera does
    return projection * glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
}

geo::Aabb box(const glm::vec3& center, float half)
{
    geo::Aabb result;
    result.expand(center - glm::vec3(half));
    result.expand(center + glm::vec3(half));
    return result;
}

/// True when some part of the triangle is inside the clip volume, found by
/// sampling points on it (exact enough for the "never miss" direction).
bool triangleSampledInside(const glm::mat4& vp, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    constexpr int kSteps = 4;
    for (int i = 0; i <= kSteps; ++i) {
        for (int j = 0; i + j <= kSteps; ++j) {
            const float u = static_cast<float>(i) / kSteps;
            const float v = static_cast<float>(j) / kSteps;
            const glm::vec4 clip = vp * glm::vec4(a + u * (b - a) + v * (c - a), 1.0f);
            if (clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w && clip.z >= 0.0f &&
                clip.z <= clip.w) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------- Frustum

TEST(Frustum, ClassifiesSimpleBoxes)
{
    const render::Frustum frustum = render::Frustum::fromViewProjection(viewProjection({0, 0, 5}, {0, 0, 0}));
    EXPECT_TRUE(frustum.intersects(box({0, 0, 0}, 0.5f)));     // straight ahead
    EXPECT_FALSE(frustum.intersects(box({0, 0, 8}, 0.5f)));    // behind the eye
    EXPECT_FALSE(frustum.intersects(box({30, 0, 0}, 0.5f)));   // far to the right
    EXPECT_FALSE(frustum.intersects(box({0, -30, 0}, 0.5f)));  // far below
    EXPECT_FALSE(frustum.intersects(box({0, 0, -60}, 0.5f)));  // beyond the far plane
    EXPECT_TRUE(frustum.intersects(box({0, 0, 0}, 100.0f)));   // contains the whole frustum
    EXPECT_TRUE(frustum.intersects(box({2.6f, 0, 0}, 0.5f)));  // straddles the right side
    EXPECT_FALSE(frustum.intersects(geo::Aabb{}));             // empty box
}

TEST(Frustum, NeverCullsAChunkWithAVisibleTriangle)
{
    const geo::Mesh mesh = geo::makeIcosphere(5); // 20,480 triangles
    geo::ChunkedBvh chunks;
    chunks.build(mesh, 8); // 256 triangles per chunk
    const auto positions = mesh.positions();

    std::mt19937 random(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    int partialViews = 0;
    for (int view = 0; view < 24; ++view) {
        glm::vec3 direction(unit(random), unit(random), unit(random));
        if (glm::length(direction) < 0.1f) {
            direction = {0, 0, 1};
        }
        direction = glm::normalize(direction);
        // From far (whole sphere) to close (a small patch).
        const float distance = 1.3f + 4.0f * static_cast<float>(view % 6) / 5.0f;
        const glm::vec3 target(unit(random) * 0.3f, unit(random) * 0.3f, unit(random) * 0.3f);
        const glm::mat4 vp = viewProjection(direction * distance, target, 30.0f);
        const render::Frustum frustum = render::Frustum::fromViewProjection(vp);

        u32 culled = 0;
        for (u32 chunk = 0; chunk < chunks.chunkCount(); ++chunk) {
            if (frustum.intersects(chunks.chunk(chunk).bounds())) {
                continue;
            }
            ++culled;
            const u32 first = chunk << chunks.chunkTriangleShift();
            const u32 last = std::min(mesh.triangleCount(), first + (1u << chunks.chunkTriangleShift()));
            for (u32 t = first; t < last; ++t) {
                const auto& tri = mesh.triangle(t);
                ASSERT_FALSE(triangleSampledInside(vp, positions[tri[0]], positions[tri[1]], positions[tri[2]]))
                    << "view " << view << " chunk " << chunk << " triangle " << t;
            }
        }
        partialViews += culled > 0 ? 1 : 0;
    }
    EXPECT_GT(partialViews, 6); // the test exercised culling, not only full views
}

// ---------------------------------------------------------- ChunkPageMap

TEST(ChunkPageMap, ListsEveryReferencedPageOnce)
{
    const geo::Mesh mesh = geo::makeIcosphere(5);
    render::ChunkPageMap map;
    map.build(mesh, 9, 8); // 512 triangles per chunk, 256 vertices per GPU page

    EXPECT_EQ(map.chunkCount(), (mesh.triangleCount() + 511) / 512);
    EXPECT_EQ(map.vertexPageCount(), (mesh.vertexCount() + 255) / 256);
    u32 triangles = 0;
    std::vector<u32> chunksPerPage(map.vertexPageCount(), 0);
    for (u32 chunk = 0; chunk < map.chunkCount(); ++chunk) {
        const auto pages = map.vertexPages(chunk);
        ASSERT_FALSE(pages.empty());
        EXPECT_TRUE(std::is_sorted(pages.begin(), pages.end()));
        EXPECT_EQ(std::adjacent_find(pages.begin(), pages.end()), pages.end());
        const std::set<u32> listed(pages.begin(), pages.end());
        std::set<u32> referenced;
        for (u32 t = map.firstTriangle(chunk); t < map.firstTriangle(chunk) + map.chunkTriangles(chunk); ++t) {
            for (const geo::VertexId v : mesh.triangle(t)) {
                referenced.insert(v >> 8);
            }
        }
        EXPECT_EQ(listed, referenced) << "chunk " << chunk;
        for (const u32 page : pages) {
            ++chunksPerPage[page];
        }
        triangles += map.chunkTriangles(chunk);
        EXPECT_EQ(map.indexRun(chunk).firstIndex, map.firstTriangle(chunk) * 3);
    }
    EXPECT_EQ(triangles, mesh.triangleCount());
    // Every page is used, and pages on chunk borders are shared, not copied.
    EXPECT_TRUE(std::all_of(chunksPerPage.begin(), chunksPerPage.end(), [](u32 n) { return n > 0; }));
    EXPECT_GT(map.pageReferenceCount(), map.vertexPageCount());
}

TEST(ChunkPageMap, PartialLastChunkAndWholeMeshChunk)
{
    const geo::Mesh mesh = geo::makeIcosphere(3); // 1,280 triangles
    render::ChunkPageMap map;
    map.build(mesh, 9, 10);
    ASSERT_EQ(map.chunkCount(), 3u);
    EXPECT_EQ(map.chunkTriangles(2), 1280u - 1024u);

    render::ChunkPageMap whole;
    whole.build(mesh, 40, 10); // shift clamped: one chunk holds everything
    ASSERT_EQ(whole.chunkCount(), 1u);
    EXPECT_EQ(whole.indexRun(0), (render::IndexRun{0, 1280u * 3u}));
}

TEST(IndexRuns, MergesAdjacentRangesInAnyOrder)
{
    // Slots of 768 indices (256 triangles), as the index pool lays them out.
    constexpr u32 kSlot = 256 * 3;
    const auto partialSlotRun = [](u32 slot, u32 indices) { return render::IndexRun{slot * kSlot, indices}; };
    const auto slotRun = [&](u32 slot) { return partialSlotRun(slot, kSlot); };

    std::vector<render::IndexRun> runs;
    for (u32 slot = 20; slot-- > 0;) { // every slot, given in reverse
        runs.push_back(slotRun(slot));
    }
    render::mergeIndexRuns(runs);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0], (render::IndexRun{0, 20 * kSlot}));

    // Gaps split runs; a partly filled slot (the last chunk) ends its run
    // even when the next slot is in use.
    runs = {slotRun(19), slotRun(7), slotRun(1), slotRun(5), slotRun(0), partialSlotRun(8, 100), slotRun(9), slotRun(2)};
    render::mergeIndexRuns(runs);
    ASSERT_EQ(runs.size(), 5u);
    EXPECT_EQ(runs[0], (render::IndexRun{0, 3 * kSlot}));
    EXPECT_EQ(runs[1], (render::IndexRun{5 * kSlot, kSlot}));
    EXPECT_EQ(runs[2], (render::IndexRun{7 * kSlot, kSlot + 100}));
    EXPECT_EQ(runs[3], (render::IndexRun{9 * kSlot, kSlot}));
    EXPECT_EQ(runs[4], (render::IndexRun{19 * kSlot, kSlot}));

    // Index count is preserved whatever the order.
    u32 total = 0;
    for (const auto& run : runs) {
        total += run.indexCount;
    }
    EXPECT_EQ(total, 7 * kSlot + 100);

    runs.clear();
    render::mergeIndexRuns(runs);
    EXPECT_TRUE(runs.empty());
}

// ------------------------------------------- Dirty vertices -> GPU pages

namespace {

/// A pool where every page is resident and current.
render::PagePool residentPool(u32 pages)
{
    render::PagePool pool;
    pool.reset(pages, pages + 4, 2);
    pool.beginFrame();
    for (u32 page = 0; page < pages; ++page) {
        pool.request(page, true);
    }
    return pool;
}

std::vector<u32> dirtyPages(const render::PagePool& pool)
{
    std::vector<u32> pages;
    for (u32 page = 0; page < pool.logicalCount(); ++page) {
        if (pool.isDirty(page)) {
            pages.push_back(page);
        }
    }
    return pages;
}

} // namespace

TEST(DirtyVertexPages, GeometryAndGpuPageSizesAreIndependent)
{
    // Geometry pages larger than GPU pages: one dirty geometry page -> several GPU pages.
    render::PagePool fine = residentPool(79); // 20,000 vertices in GPU pages of 256
    geo::DirtyVertexSet coarseGeometry(20000, 10); // 1,024 per geometry page
    coarseGeometry.mark(2048);
    render::markDirtyVertexPages(fine, coarseGeometry, 8);
    EXPECT_EQ(dirtyPages(fine), (std::vector<u32>{8, 9, 10, 11}));

    // Geometry pages smaller than GPU pages: the GPU page containing them is marked.
    render::PagePool coarse = residentPool(5); // GPU pages of 4,096
    geo::DirtyVertexSet fineGeometry(20000, 6); // 64 per geometry page
    fineGeometry.mark(4096 + 70);
    fineGeometry.mark(4096 + 3000);
    render::markDirtyVertexPages(coarse, fineGeometry, 12);
    EXPECT_EQ(dirtyPages(coarse), (std::vector<u32>{1}));

    // A geometry page straddling two GPU pages marks both; the last, partial page works.
    geo::DirtyVertexSet straddling(20000, 11); // 2,048 per page: [2048, 4096) and [4096, 6144)
    straddling.mark(4000);
    straddling.mark(4100);
    straddling.mark(19999);
    render::markDirtyVertexPages(coarse, straddling, 12);
    EXPECT_EQ(dirtyPages(coarse), (std::vector<u32>{0, 1, 4}));
}

TEST(DirtyVertexPages, IgnoresAbsentPagesAndLargerSets)
{
    render::PagePool pool;
    pool.reset(4, 6, 2); // nothing resident yet
    geo::DirtyVertexSet all(4096, 10);
    all.markAll();
    render::markDirtyVertexPages(pool, all, 10);
    EXPECT_TRUE(dirtyPages(pool).empty()) << "absent pages are uploaded current anyway";

    render::PagePool resident = residentPool(4);
    geo::DirtyVertexSet larger(9999, 10); // a set for a bigger mesh: pages past the end are ignored
    larger.markAll();
    render::markDirtyVertexPages(resident, larger, 10);
    EXPECT_EQ(dirtyPages(resident), (std::vector<u32>{0, 1, 2, 3}));
}

// --------------------------------------------------------------- PagePool

TEST(PagePool, UploadsOnceThenReuses)
{
    render::PagePool pool;
    pool.reset(10, 4, 2);
    pool.beginFrame();
    const auto first = pool.request(3, true);
    EXPECT_TRUE(first.usable);
    EXPECT_TRUE(first.upload);
    pool.beginFrame();
    const auto again = pool.request(3, true);
    EXPECT_TRUE(again.usable);
    EXPECT_FALSE(again.upload);
    EXPECT_EQ(again.slot, first.slot);
    EXPECT_EQ(pool.residentCount(), 1u);
}

TEST(PagePool, NeverReusesASlotAFrameInFlightMayRead)
{
    render::PagePool pool;
    pool.reset(10, 2, 3); // 2 slots, 3 frames in flight
    pool.beginFrame();
    EXPECT_TRUE(pool.request(0, true).upload);
    EXPECT_TRUE(pool.request(1, true).upload);
    // Both slots were used this frame and the next two: nothing can be evicted.
    for (int frame = 0; frame < 2; ++frame) {
        pool.beginFrame();
        const auto access = pool.request(2, true);
        EXPECT_FALSE(access.usable) << frame;
        EXPECT_EQ(pool.frameStats().unavailable, 1u);
        EXPECT_EQ(pool.slotOf(0) != render::PagePool::kNoSlot, true);
    }
    // Three frames after the last use the slots have retired.
    pool.beginFrame();
    const auto access = pool.request(2, true);
    EXPECT_TRUE(access.usable);
    EXPECT_TRUE(access.upload);
    EXPECT_EQ(pool.frameStats().evictions, 1u);
}

TEST(PagePool, EvictsTheLeastRecentlyUsedPage)
{
    render::PagePool pool;
    pool.reset(10, 3, 1);
    pool.beginFrame();
    pool.request(0, true);
    pool.beginFrame();
    pool.request(1, true);
    pool.beginFrame();
    pool.request(2, true);
    pool.beginFrame();
    pool.request(0, true); // 0 is now the most recently used, 1 the least
    pool.beginFrame();
    const auto access = pool.request(3, true);
    ASSERT_TRUE(access.upload);
    EXPECT_EQ(pool.slotOf(1), render::PagePool::kNoSlot);
    EXPECT_NE(pool.slotOf(0), render::PagePool::kNoSlot);
    EXPECT_NE(pool.slotOf(2), render::PagePool::kNoSlot);
    EXPECT_EQ(pool.residentCount(), 3u);
}

TEST(PagePool, DirtyPagesAreCopiedOnWrite)
{
    render::PagePool pool;
    pool.reset(4, 4, 2);
    pool.beginFrame();
    const u32 oldSlot = pool.request(0, true).slot;
    pool.beginFrame();
    pool.markDirty(0);
    EXPECT_TRUE(pool.isDirty(0));
    const auto fresh = pool.request(0, true);
    EXPECT_TRUE(fresh.upload);
    EXPECT_FALSE(fresh.stale);
    EXPECT_NE(fresh.slot, oldSlot); // the frame in flight still reads the old slot
    EXPECT_FALSE(pool.isDirty(0));
    EXPECT_EQ(pool.residentCount(), 1u);

    // The old slot is free again only once it has retired.
    pool.request(1, true);
    pool.request(2, true);
    EXPECT_FALSE(pool.request(3, true).usable); // old slot used last frame: not retired
    pool.beginFrame();
    const auto later = pool.request(3, true);
    EXPECT_TRUE(later.upload);
    EXPECT_EQ(later.slot, oldSlot);
}

TEST(PagePool, DirtyPageWhoseOwnSlotRetiredIsRewrittenInPlace)
{
    // Regression: the only retired slot is the dirty page's own; it is reused
    // and the page must stay counted as resident.
    render::PagePool pool;
    pool.reset(4, 1, 1);
    pool.beginFrame();
    const u32 slot = pool.request(0, true).slot;
    pool.beginFrame();
    pool.markDirty(0);
    const auto access = pool.request(0, true);
    EXPECT_TRUE(access.upload);
    EXPECT_EQ(access.slot, slot);
    EXPECT_EQ(pool.slotOf(0), slot);
    EXPECT_EQ(pool.residentCount(), 1u);
}
TEST(PagePool, ReserveKeepsEditsFlowingWhenTheViewFillsThePool)
{
    // 4 slots, 1 reserved for copy on write, 2 frames in flight.
    render::PagePool pool;
    pool.reset(16, 4, 2, 1);
    std::vector<u32> visible = {0, 1, 2, 3, 4, 5}; // more than fits
    const auto drawFrame = [&] {
        pool.beginFrame();
        u32 usable = 0;
        for (const u32 page : visible) {
            usable += pool.request(page, true).usable ? 1u : 0u;
        }
        return usable;
    };
    EXPECT_EQ(drawFrame(), 3u); // the reserve is not given to new pages
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(drawFrame(), 3u);
    // Edit a visible page in every frame: each frame either gets a fresh copy
    // or (while the reserve slot is still in flight) draws the old one.
    u32 fresh = 0;
    for (int frame = 0; frame < 12; ++frame) {
        pool.markDirty(1);
        pool.beginFrame();
        u32 usable = 0;
        for (const u32 page : visible) {
            const auto access = pool.request(page, true);
            usable += access.usable ? 1u : 0u;
            fresh += page == 1 && access.upload ? 1u : 0u;
        }
        EXPECT_EQ(usable, 3u) << frame;
    }
    EXPECT_GE(fresh, 5u); // about every other frame with 2 frames in flight
    // Without a reserve the same edits would never be uploaded.
    render::PagePool tight;
    tight.reset(16, 4, 2, 0);
    for (int frame = 0; frame < 3; ++frame) {
        tight.beginFrame();
        for (const u32 page : visible) {
            tight.request(page, true);
        }
    }
    u32 tightFresh = 0;
    for (int frame = 0; frame < 12; ++frame) {
        tight.markDirty(1);
        tight.beginFrame();
        for (const u32 page : visible) {
            const bool uploaded = tight.request(page, true).upload; // request every page
            tightFresh += page == 1 && uploaded ? 1u : 0u;
        }
    }
    EXPECT_EQ(tightFresh, 0u);
}
TEST(PagePool, FallsBackToStaleContentWhenNoSlotIsFree)
{
    render::PagePool pool;
    pool.reset(4, 1, 2);
    pool.beginFrame();
    const u32 slot = pool.request(0, true).slot;
    pool.beginFrame();
    pool.markDirty(0);
    const auto access = pool.request(0, true); // only slot is in flight
    EXPECT_TRUE(access.usable);
    EXPECT_TRUE(access.stale);
    EXPECT_FALSE(access.upload);
    EXPECT_EQ(access.slot, slot);
    EXPECT_TRUE(pool.isDirty(0)); // still owed an upload
    EXPECT_EQ(pool.frameStats().staleUses, 1u);
}

TEST(PagePool, CallerUploadLimitDefersWork)
{
    render::PagePool pool;
    pool.reset(4, 4, 1);
    pool.beginFrame();
    EXPECT_FALSE(pool.request(0, false).usable);
    EXPECT_EQ(pool.frameStats().deferredByCaller, 1u);
    EXPECT_EQ(pool.frameStats().unavailable, 1u);
    EXPECT_TRUE(pool.request(0, true).upload);
    pool.beginFrame();
    pool.markDirty(0);
    const auto access = pool.request(0, false);
    EXPECT_TRUE(access.stale);
}

TEST(PagePool, EditsToEvictedPagesNeedNoTracking)
{
    render::PagePool pool;
    pool.reset(4, 1, 1);
    pool.beginFrame();
    pool.request(0, true);
    pool.beginFrame();
    pool.request(1, true); // evicts 0
    EXPECT_EQ(pool.slotOf(0), render::PagePool::kNoSlot);
    pool.markDirty(0); // ignored: not resident
    EXPECT_FALSE(pool.isDirty(0));
    pool.beginFrame();
    const auto access = pool.request(0, true);
    EXPECT_TRUE(access.upload); // fetched again with current data
    EXPECT_FALSE(access.stale);
}

// Randomized frames with a model of the GPU: frames in flight read slot
// contents recorded when they were built. Checks that an upload never lands
// in a slot an unfinished frame reads, and that what a frame draws is the
// right page, either current or (when marked stale) an older version.
TEST(PagePool, RandomizedFramesInFlightStayConsistent)
{
    constexpr u32 kPages = 64;
    constexpr u32 kSlots = 20;
    constexpr u32 kFramesInFlight = 3;
    render::PagePool pool;
    pool.reset(kPages, kSlots, kFramesInFlight);

    struct Content {
        u32 page = render::PagePool::kNoSlot;
        u32 version = 0;
    };
    std::vector<Content> slots(kSlots);
    std::vector<u32> cpuVersion(kPages, 1);
    std::deque<std::set<u32>> inFlight; // slots read by each unfinished frame, newest last

    std::mt19937 random(11);
    u32 staleSeen = 0;
    u32 evictions = 0;
    for (int frame = 0; frame < 3000; ++frame) {
        pool.beginFrame();
        // The frame kFramesInFlight back has finished (its frame slot is reused now).
        while (inFlight.size() >= kFramesInFlight) {
            inFlight.pop_front();
        }
        // Edits.
        for (int e = 0; e < 3; ++e) {
            const u32 page = random() % kPages;
            ++cpuVersion[page];
            pool.markDirty(page);
        }
        // The visible set: a sliding window plus noise, sometimes bigger than the pool.
        const u32 base = static_cast<u32>(frame / 7) % kPages;
        const u32 count = 6 + random() % 18;
        std::set<u32> reads;
        u32 uploads = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 page = (base + i * (1 + random() % 2)) % kPages;
            const bool mayUpload = uploads < 8;
            const auto access = pool.request(page, mayUpload);
            if (access.upload) {
                for (const auto& old : inFlight) {
                    ASSERT_EQ(old.count(access.slot), 0u) << "frame " << frame << " overwrote a slot in flight";
                }
                ASSERT_EQ(reads.count(access.slot), 0u) << "frame " << frame << " overwrote a slot it already reads";
                slots[access.slot] = {page, cpuVersion[page]};
                ++uploads;
            }
            if (access.usable) {
                ASSERT_EQ(slots[access.slot].page, page);
                if (access.stale) {
                    ASSERT_LT(slots[access.slot].version, cpuVersion[page]);
                    ++staleSeen;
                } else {
                    ASSERT_EQ(slots[access.slot].version, cpuVersion[page]);
                }
                reads.insert(access.slot);
            }
        }
        evictions += pool.frameStats().evictions;
        ASSERT_LE(pool.residentCount(), kSlots);
        inFlight.push_back(std::move(reads));
    }
    EXPECT_GT(evictions, 100u);
    EXPECT_GT(staleSeen, 0u);
}
