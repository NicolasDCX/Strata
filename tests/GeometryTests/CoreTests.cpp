#include "Core/FairMutex.h"
#include "Core/JobSystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

using namespace se;

TEST(JobSystem, VisitsEveryElementExactlyOnce)
{
    core::JobSystem jobs(4);
    EXPECT_EQ(jobs.workerCount(), 4u);
    for (const std::size_t count : {std::size_t{1}, std::size_t{7}, std::size_t{1000}, std::size_t{100003}}) {
        std::vector<std::atomic<int>> visits(count);
        core::parallelFor(&jobs, count, 64, [&visits](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                visits[i].fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (std::size_t i = 0; i < count; ++i) {
            ASSERT_EQ(visits[i].load(), 1) << "element " << i << " of " << count;
        }
    }
}

TEST(JobSystem, ChunksRespectTheGrainAndAreContiguous)
{
    core::JobSystem jobs(3);
    std::mutex mutex;
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
    core::parallelFor(&jobs, 1000, 128, [&](std::size_t begin, std::size_t end) {
        const std::scoped_lock lock(mutex);
        chunks.emplace_back(begin, end);
    });
    std::sort(chunks.begin(), chunks.end());
    ASSERT_EQ(chunks.size(), 8u); // ceil(1000 / 128)
    std::size_t expectedBegin = 0;
    for (const auto& [begin, end] : chunks) {
        EXPECT_EQ(begin, expectedBegin);
        EXPECT_LE(end - begin, 128u);
        expectedBegin = end;
    }
    EXPECT_EQ(expectedBegin, 1000u);
}

TEST(JobSystem, RunsInlineWithoutPoolOrWorkersOrForSmallRanges)
{
    const std::thread::id caller = std::this_thread::get_id();
    const auto expectInline = [caller](core::JobSystem* jobs, std::size_t count, std::size_t grain) {
        int calls = 0;
        core::parallelFor(jobs, count, grain, [&](std::size_t begin, std::size_t end) {
            ++calls;
            EXPECT_EQ(std::this_thread::get_id(), caller);
            EXPECT_EQ(begin, 0u);
            EXPECT_EQ(end, count);
        });
        EXPECT_EQ(calls, 1);
    };
    expectInline(nullptr, 5000, 16);
    core::JobSystem none(0);
    expectInline(&none, 5000, 16);
    core::JobSystem some(2);
    expectInline(&some, 100, 100);

    int calls = 0;
    core::parallelFor(&some, 0, 16, [&calls](std::size_t, std::size_t) { ++calls; });
    EXPECT_EQ(calls, 0) << "an empty range must not call the body";
}

TEST(JobSystem, NestedLoopsDoNotDeadlock)
{
    core::JobSystem jobs(3);
    std::atomic<long long> sum{0};
    core::parallelFor(&jobs, 64, 1, [&](std::size_t outerBegin, std::size_t outerEnd) {
        for (std::size_t outer = outerBegin; outer < outerEnd; ++outer) {
            core::parallelFor(&jobs, 500, 16, [&sum](std::size_t begin, std::size_t end) {
                long long local = 0;
                for (std::size_t i = begin; i < end; ++i) {
                    local += static_cast<long long>(i);
                }
                sum.fetch_add(local, std::memory_order_relaxed);
            });
        }
    });
    EXPECT_EQ(sum.load(), 64LL * (499LL * 500LL / 2LL));
}

TEST(JobSystem, ManySmallRunsFromSeveralThreads)
{
    // Stress the claim/finish bookkeeping: jobs are created and destroyed quickly
    // by several callers sharing one pool.
    core::JobSystem jobs(4);
    std::vector<std::thread> callers;
    std::atomic<long long> total{0};
    for (int caller = 0; caller < 4; ++caller) {
        callers.emplace_back([&] {
            for (int run = 0; run < 300; ++run) {
                core::parallelFor(&jobs, 257, 8, [&total](std::size_t begin, std::size_t end) {
                    total.fetch_add(static_cast<long long>(end - begin), std::memory_order_relaxed);
                });
            }
        });
    }
    for (std::thread& thread : callers) {
        thread.join();
    }
    EXPECT_EQ(total.load(), 4LL * 300LL * 257LL);
}

TEST(FairMutex, ProvidesMutualExclusion)
{
    core::FairMutex mutex;
    long long counter = 0; // deliberately not atomic
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                const std::scoped_lock lock(mutex);
                ++counter;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(counter, 80000);
}

TEST(FairMutex, WaiterGetsTheLockDespiteABusyOwner)
{
    // One thread re-locks in a tight loop (like the sculpt thread between dabs).
    // A second thread must still get in, many times, within a short time.
    core::FairMutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<long long> busyIterations{0};
    std::thread busy([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const std::scoped_lock lock(mutex);
            busyIterations.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (busyIterations.load() == 0) {
        std::this_thread::yield();
    }

    int acquired = 0;
    for (int i = 0; i < 200; ++i) {
        const std::scoped_lock lock(mutex);
        ++acquired;
    }
    stop.store(true);
    busy.join();
    EXPECT_EQ(acquired, 200);
}
