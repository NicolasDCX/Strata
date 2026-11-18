#pragma once

#include <mutex>

namespace se::core {

/// Mutex that hands over to a waiting thread instead of letting the thread that
/// just unlocked re-acquire it.
///
/// std::mutex gives no fairness guarantee: a thread that unlocks and locks
/// again in a tight loop (the sculpt worker, between two dabs) can starve a
/// waiter (the UI thread, which wants to copy vertex data for the next frame).
/// Here every locker first takes a "next in line" mutex, so the waiter is
/// already queued in front of the worker's next attempt. A waiter therefore
/// waits for at most one critical section of the other thread.
///
/// Satisfies BasicLockable: use it with std::scoped_lock / std::unique_lock.
class FairMutex {
public:
    void lock()
    {
        m_next.lock();
        m_data.lock();
        m_next.unlock();
    }

    void unlock() { m_data.unlock(); }

private:
    std::mutex m_next;
    std::mutex m_data;
};

} // namespace se::core
