#pragma once

#include "Core/Assert.h"
#include "Core/Types.h"
#include "Geometry/DirtyVertexSet.h"

#include <vector>

namespace se::render {

/// Decides which logical pages are resident in a fixed number of GPU slots.
///
/// "Logical page" is whatever the caller pages: a GPU vertex page, or the index
/// data of one chunk. The pool holds `slotCount` slots of equal size; the
/// budget is slotCount x slot size. It does no I/O itself: request() says
/// whether the page can be used this frame and, if data must be copied, into
/// which slot.
///
/// Safety with frames in flight
/// ---------------------------
/// Several frames can be queued on the GPU at once, each reading the slots its
/// own page table pointed to. Two rules make that safe without waiting:
///
/// 1. A slot is never overwritten while a queued frame may still read it. Every
///    use stamps the slot with the frame number; a slot can be reused only when
///    its stamp is at least `retireDelayFrames` frames old. The caller passes
///    the number of frame slots, since the frame that last used a frame slot is
///    known to be finished when that slot comes round again.
/// 2. Changed content is never written in place. A dirty page is uploaded to a
///    different slot (copy on write) and the page's mapping switches to it; the
///    old slot keeps its content until it retires. If no slot can be had right
///    now, the page stays usable with its old content, one or more frames stale.
///
/// 3. Edits must reach the screen even when the visible set fills the pool. If
///    every slot were in use each frame, a dirty page could never move to a
///    fresh slot and would stay stale for as long as the view does not change.
///    So `reserveSlots` slots can be taken only for copy on write: a page that
///    is not resident yet gets a slot only while fewer than
///    slotCount - reserveSlots slots are in use this frame.
///
/// Eviction picks the least recently used resident page that is not needed by
/// the current frame. A page that was evicted is simply uploaded again when it
/// is next requested; edits made while it was absent need no special handling,
/// because uploads always copy the current CPU data.
///
/// No Vulkan code.
class PagePool {
public:
    static constexpr u32 kNoSlot = 0xFFFFFFFFu;

    struct Access {
        bool usable = false; ///< The page can be drawn from `slot` this frame.
        bool upload = false; ///< The caller must copy the page's current data into `slot` now.
        bool stale = false;  ///< Usable, but the content is older than the CPU data.
        u32 slot = kNoSlot;
    };

    struct FrameStats {
        u32 uploads = 0;
        u32 evictions = 0;
        u32 staleUses = 0;
        u32 unavailable = 0;      ///< requests that could not be satisfied at all
        u32 deferredByCaller = 0; ///< of the stale/unavailable ones: upload was not allowed
    };

    void reset(u32 logicalCount, u32 slotCount, u32 retireDelayFrames, u32 reserveSlots = 0)
    {
        SE_ASSERT(retireDelayFrames >= 1);
        SE_ASSERT(reserveSlots < slotCount);
        m_reserve = reserveSlots;
        m_usedThisFrame = 0;
        m_slotOf.assign(logicalCount, kNoSlot);
        m_dirty.assign(logicalCount, u8{0});
        m_pageOf.assign(slotCount, kNoSlot);
        m_lastUsed.assign(slotCount, 0);
        m_retireDelay = retireDelayFrames;
        m_frame = retireDelayFrames; // so that never-used slots (stamp 0) are retired from the start
        m_resident = 0;
        m_frameStats = {};
    }

    [[nodiscard]] u32 logicalCount() const noexcept { return static_cast<u32>(m_slotOf.size()); }
    [[nodiscard]] u32 slotCount() const noexcept { return static_cast<u32>(m_pageOf.size()); }
    [[nodiscard]] u32 residentCount() const noexcept { return m_resident; }
    [[nodiscard]] u32 reserveSlots() const noexcept { return m_reserve; }
    /// Slots read by the current frame so far.
    [[nodiscard]] u32 usedThisFrame() const noexcept { return m_usedThisFrame; }
    /// True when a page that is not resident can no longer get a slot this frame.
    [[nodiscard]] bool full() const noexcept { return m_usedThisFrame + m_reserve >= slotCount(); }
    [[nodiscard]] u64 frame() const noexcept { return m_frame; }
    [[nodiscard]] const FrameStats& frameStats() const noexcept { return m_frameStats; }

    /// Slot currently mapped to the page (possibly stale), or kNoSlot.
    [[nodiscard]] u32 slotOf(u32 page) const noexcept { return m_slotOf[page]; }
    [[nodiscard]] bool isDirty(u32 page) const noexcept { return m_dirty[page] != 0; }
    /// True when request(page) would use no slot beyond those already in use
    /// this frame: the page is resident, current, and already requested this frame.
    [[nodiscard]] bool freeToUse(u32 page) const noexcept
    {
        const u32 slot = m_slotOf[page];
        return slot != kNoSlot && m_dirty[page] == 0 && m_lastUsed[slot] == m_frame;
    }

    /// Starts a new frame. Stamps of this frame protect slots from eviction.
    void beginFrame() noexcept
    {
        ++m_frame;
        m_frameStats = {};
        m_usedThisFrame = 0;
    }

    /// The CPU data of the page changed. Only matters while the page is resident.
    void markDirty(u32 page) noexcept
    {
        if (m_slotOf[page] != kNoSlot) {
            m_dirty[page] = 1;
        }
    }

    /// Asks to use `page` in the current frame. `mayUpload` lets the caller
    /// enforce a per-frame upload limit.
    Access request(u32 page, bool mayUpload)
    {
        const u32 current = m_slotOf[page];
        if (current != kNoSlot && m_dirty[page] == 0) {
            stamp(current);
            return {.usable = true, .upload = false, .stale = false, .slot = current};
        }

        // The page needs its current data in a slot it does not have yet.
        // A page that is not resident may not take the copy-on-write reserve.
        const bool allowed = current != kNoSlot || !full();
        if (mayUpload && allowed) {
            const u32 fresh = acquireSlot();
            if (fresh != kNoSlot) {
                // acquireSlot() may have evicted this very page when its own slot
                // had retired and was the least recently used; then the page is
                // simply rewritten in place (no frame in flight reads that slot).
                const u32 mapped = m_slotOf[page];
                if (mapped != kNoSlot) {
                    // Copy on write: the old slot keeps its content for frames in
                    // flight and becomes reusable once its stamp has aged.
                    m_pageOf[mapped] = kNoSlot;
                } else {
                    ++m_resident;
                }
                m_slotOf[page] = fresh;
                m_pageOf[fresh] = page;
                stamp(fresh);
                m_dirty[page] = 0;
                ++m_frameStats.uploads;
                return {.usable = true, .upload = true, .stale = false, .slot = fresh};
            }
        } else if (!mayUpload) {
            ++m_frameStats.deferredByCaller;
        }

        if (current != kNoSlot) {
            stamp(current);
            ++m_frameStats.staleUses;
            return {.usable = true, .upload = false, .stale = true, .slot = current};
        }
        ++m_frameStats.unavailable;
        return {};
    }

private:
    void stamp(u32 slot) noexcept
    {
        if (m_lastUsed[slot] != m_frame) {
            m_lastUsed[slot] = m_frame;
            ++m_usedThisFrame;
        }
    }

    [[nodiscard]] bool retired(u32 slot) const noexcept { return m_lastUsed[slot] + m_retireDelay <= m_frame; }

    /// A slot that no queued frame can still read: an unmapped one if there is
    /// any, otherwise the least recently used mapped one (which is evicted).
    u32 acquireSlot()
    {
        u32 best = kNoSlot;
        u64 bestStamp = 0;
        const u32 slotTotal = slotCount();
        for (u32 i = 0; i < slotTotal; ++i) {
            // Rotate the starting point so repeated calls do not rescan the same prefix.
            const u32 slot = m_cursor + i < slotTotal ? m_cursor + i : m_cursor + i - slotTotal;
            if (!retired(slot)) {
                continue;
            }
            if (m_pageOf[slot] == kNoSlot) {
                m_cursor = slot + 1 < slotTotal ? slot + 1 : 0;
                return slot; // free: take it at once
            }
            if (best == kNoSlot || m_lastUsed[slot] < bestStamp) {
                best = slot;
                bestStamp = m_lastUsed[slot];
            }
        }
        if (best != kNoSlot) {
            const u32 evicted = m_pageOf[best];
            m_slotOf[evicted] = kNoSlot;
            m_dirty[evicted] = 0;
            m_pageOf[best] = kNoSlot;
            --m_resident;
            ++m_frameStats.evictions;
        }
        return best;
    }

    std::vector<u32> m_slotOf;   // per logical page
    std::vector<u8> m_dirty;     // per logical page: resident copy is out of date
    std::vector<u32> m_pageOf;   // per slot: mapped page, or kNoSlot
    std::vector<u64> m_lastUsed; // per slot: frame of last use
    u64 m_frame = 0;
    u32 m_retireDelay = 1;
    u32 m_resident = 0;
    u32 m_cursor = 0;
    u32 m_reserve = 0;
    u32 m_usedThisFrame = 0;
    FrameStats m_frameStats;
};

/// Marks dirty every GPU vertex page (2^gpuPageShift vertices) that overlaps a
/// changed vertex run of `vertices`, whatever that set's own page size. Pages
/// that are not resident are left alone (see PagePool::markDirty).
inline void markDirtyVertexPages(PagePool& pool, const geo::DirtyVertexSet& vertices, u32 gpuPageShift)
{
    vertices.forEachRun([&pool, gpuPageShift](u32 first, u32 count) {
        const u32 lastPage = (first + count - 1u) >> gpuPageShift;
        for (u32 page = first >> gpuPageShift; page <= lastPage && page < pool.logicalCount(); ++page) {
            pool.markDirty(page);
        }
    });
}

} // namespace se::render
