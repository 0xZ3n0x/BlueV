// SPDX-License-Identifier: GPL-3.0-or-later
// Synchronisation primitives annotated for clang's -Wthread-safety, so the
// compiler checks that every guarded member is only touched under its lock.
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined(__clang__)
#define BLUEV_THREAD_ANNOTATION(x) __attribute__((x))
#else
#define BLUEV_THREAD_ANNOTATION(x)
#endif

#define BLUEV_CAPABILITY(x) BLUEV_THREAD_ANNOTATION(capability(x))
#define BLUEV_SCOPED_LOCKABLE BLUEV_THREAD_ANNOTATION(scoped_lockable)
#define BLUEV_GUARDED_BY(x) BLUEV_THREAD_ANNOTATION(guarded_by(x))
#define BLUEV_REQUIRES(...) BLUEV_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
#define BLUEV_EXCLUDES(...) BLUEV_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))
#define BLUEV_ACQUIRE(...) BLUEV_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define BLUEV_RELEASE(...) BLUEV_THREAD_ANNOTATION(release_capability(__VA_ARGS__))

namespace bluev
{

class BLUEV_CAPABILITY("mutex") Mutex
{
  public:
    void lock() BLUEV_ACQUIRE()
    {
        m_mutex.lock();
    }
    void unlock() BLUEV_RELEASE()
    {
        m_mutex.unlock();
    }

    std::mutex& native()
    {
        return m_mutex;
    }

  private:
    std::mutex m_mutex;
};

class BLUEV_SCOPED_LOCKABLE ScopedLock
{
  public:
    explicit ScopedLock(Mutex& mutex) BLUEV_ACQUIRE(mutex) : m_mutex(mutex)
    {
        m_mutex.lock();
    }
    ~ScopedLock() BLUEV_RELEASE()
    {
        m_mutex.unlock();
    }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

  private:
    Mutex& m_mutex;
};

// Waits on a condition variable while holding `mutex`. The predicate runs
// with the lock held.
template <typename Predicate>
void waitFor(std::condition_variable& cv, Mutex& mutex, Predicate pred) BLUEV_REQUIRES(mutex)
{
    std::unique_lock<std::mutex> lock(mutex.native(), std::adopt_lock);
    cv.wait(lock, pred);
    lock.release();
}

// The same with a time limit; false if the limit passed first.
template <typename Rep, typename Period, typename Predicate>
bool waitFor(std::condition_variable& cv, Mutex& mutex, std::chrono::duration<Rep, Period> limit, Predicate pred)
    BLUEV_REQUIRES(mutex)
{
    std::unique_lock<std::mutex> lock(mutex.native(), std::adopt_lock);
    const bool satisfied = cv.wait_for(lock, limit, pred);
    lock.release();
    return satisfied;
}

} // namespace bluev
