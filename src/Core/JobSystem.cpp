#include "Core/JobSystem.h"

#include <algorithm>

namespace se::core {

JobSystem::JobSystem(unsigned workerThreads)
{
    m_threads.reserve(workerThreads);
    for (unsigned i = 0; i < workerThreads; ++i) {
        m_threads.emplace_back([this] { workerMain(); });
    }
}

JobSystem::~JobSystem()
{
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }
    m_workAvailable.notify_all();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

unsigned JobSystem::defaultWorkerCount() noexcept
{
    // hardware_concurrency() counts logical cores. The loops here are memory
    // bound and gain little from SMT siblings, so aim for the physical cores,
    // minus one because the calling thread also works.
    const unsigned logical = std::max(1u, std::thread::hardware_concurrency());
    const unsigned participants = std::clamp(logical / 2u, 1u, 16u);
    return participants - 1u;
}

bool JobSystem::claimChunk(Job& job, std::size_t& begin, std::size_t& end)
{
    if (job.next >= job.count) {
        return false;
    }
    begin = job.next;
    end = std::min(job.count, begin + job.grain);
    job.next = end;
    if (job.next >= job.count) {
        // Fully claimed: hide the job from the workers. It stays alive on its
        // owner's stack until pendingChunks reaches zero.
        m_jobs.erase(std::find(m_jobs.begin(), m_jobs.end(), &job));
    }
    return true;
}

void JobSystem::run(std::size_t count, std::size_t grain, RangeBody body)
{
    if (count == 0) {
        return;
    }
    Job job;
    job.body = body;
    job.count = count;
    job.grain = std::max<std::size_t>(grain, 1);
    job.pendingChunks = (count + job.grain - 1) / job.grain;

    // Wake only as many workers as there are chunks beyond the caller's own:
    // waking a thread costs far more than a small chunk is worth.
    const std::size_t helpers = std::min<std::size_t>(job.pendingChunks - 1, m_threads.size());

    std::unique_lock lock(m_mutex);
    m_jobs.push_back(&job);
    lock.unlock();
    for (std::size_t i = 0; i < helpers; ++i) {
        m_workAvailable.notify_one();
    }
    lock.lock();

    // Work on our own job alongside the workers.
    std::size_t begin = 0;
    std::size_t end = 0;
    while (claimChunk(job, begin, end)) {
        lock.unlock();
        job.body.invoke(job.body.context, begin, end);
        lock.lock();
        --job.pendingChunks;
    }
    // Chunks claimed by workers may still be running. They decrement under the
    // mutex and do not touch the job afterwards, so it is safe to return (and
    // destroy the job) as soon as the count is zero.
    m_chunkFinished.wait(lock, [&job] { return job.pendingChunks == 0; });
}

void JobSystem::workerMain()
{
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_workAvailable.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
        if (m_stopping) {
            return;
        }
        Job& job = *m_jobs.front();
        std::size_t begin = 0;
        std::size_t end = 0;
        if (!claimChunk(job, begin, end)) {
            continue; // cannot happen (exhausted jobs are removed), but stay safe
        }
        lock.unlock();
        job.body.invoke(job.body.context, begin, end);
        lock.lock();
        if (--job.pendingChunks == 0) {
            m_chunkFinished.notify_all();
        }
    }
}

} // namespace se::core
