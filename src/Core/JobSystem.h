#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace se::core {

/// Small fork-join thread pool for data-parallel loops.
///
/// run() splits [0, count) into chunks of `grain` elements, executes them on the
/// worker threads and on the calling thread, and returns when all are done. The
/// caller always takes part, so run() is safe to call from inside a body
/// (nested loops cannot deadlock) and works with zero worker threads.
///
/// Scope: this is intentionally minimal (one shared queue, chunked ranges). It
/// covers the per-dab loops of the sculpt engine without adding a dependency.
/// oneTBB can replace it behind the same parallelFor() helper if work stealing
/// or task graphs become necessary.
///
/// Bodies must not throw.
class JobSystem {
public:
    /// Non-owning reference to a callable `void(std::size_t begin, std::size_t end)`.
    struct RangeBody {
        void* context = nullptr;
        void (*invoke)(void* context, std::size_t begin, std::size_t end) = nullptr;
    };

    /// Starts `workerThreads` threads (0 is valid: everything runs on the caller).
    explicit JobSystem(unsigned workerThreads);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    [[nodiscard]] unsigned workerCount() const noexcept { return static_cast<unsigned>(m_threads.size()); }

    /// Worker count that leaves room for the UI thread and the calling thread:
    /// about one participant per physical core.
    [[nodiscard]] static unsigned defaultWorkerCount() noexcept;

    /// Runs `body` over [0, count) in chunks of at most `grain`; blocks until done.
    void run(std::size_t count, std::size_t grain, RangeBody body);

private:
    struct Job {
        RangeBody body;
        std::size_t count = 0;
        std::size_t grain = 1;
        std::size_t next = 0;          // first unclaimed element (guarded by m_mutex)
        std::size_t pendingChunks = 0; // claimed or unclaimed chunks not yet finished (guarded by m_mutex)
    };

    void workerMain();
    /// Claims the next chunk of `job`. Requires m_mutex. Returns false if none is left.
    bool claimChunk(Job& job, std::size_t& begin, std::size_t& end);

    std::mutex m_mutex;
    std::condition_variable m_workAvailable;
    std::condition_variable m_chunkFinished;
    std::deque<Job*> m_jobs; // jobs that still have unclaimed chunks
    bool m_stopping = false;
    std::vector<std::thread> m_threads;
};

/// Runs `body(begin, end)` over [0, count). Executes inline when there is no
/// job system, no worker thread, or the range fits in one chunk; otherwise in
/// parallel. Chunks never overlap, so a body that only writes elements of its
/// own range needs no synchronisation, and the result is identical to the
/// serial loop.
template <typename Body>
void parallelFor(JobSystem* jobs, std::size_t count, std::size_t grain, Body&& body)
{
    if (count == 0) {
        return;
    }
    if (jobs == nullptr || jobs->workerCount() == 0 || count <= grain) {
        body(std::size_t{0}, count);
        return;
    }
    using BodyType = std::remove_reference_t<Body>;
    const JobSystem::RangeBody reference{
        const_cast<void*>(static_cast<const void*>(std::addressof(body))),
        [](void* context, std::size_t begin, std::size_t end) { (*static_cast<BodyType*>(context))(begin, end); }};
    jobs->run(count, grain, reference);
}

} // namespace se::core
