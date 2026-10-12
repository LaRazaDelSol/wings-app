// Wings TV app - Threads, locks and a main-thread inbox over pthreads.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console build links only part of the C++ runtime, so std::thread and
// std::mutex are avoided in favour of the pthread calls the kit already uses.
// Console threads are first-in first-out and never time-sliced: workers block
// on the network or on a condition, and keep CPU-heavy work short.

#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace orbit
{

// Monotonic seconds.
double now_seconds();
// Wall-clock Unix seconds (retry countdowns from the backend use these).
double unix_seconds();

class Mutex
{
  public:
    Mutex()
    {
        pthread_mutex_init(&mutex_, nullptr);
    }
    ~Mutex()
    {
        pthread_mutex_destroy(&mutex_);
    }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock()
    {
        pthread_mutex_lock(&mutex_);
    }
    void unlock()
    {
        pthread_mutex_unlock(&mutex_);
    }
    pthread_mutex_t *native()
    {
        return &mutex_;
    }

  private:
    pthread_mutex_t mutex_;
};

class Lock
{
  public:
    explicit Lock(Mutex &mutex) : mutex_(mutex)
    {
        mutex_.lock();
    }
    ~Lock()
    {
        mutex_.unlock();
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

  private:
    Mutex &mutex_;
};

class Signal
{
  public:
    Signal()
    {
        pthread_cond_init(&cond_, nullptr);
    }
    ~Signal()
    {
        pthread_cond_destroy(&cond_);
    }
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;
    // Waits (the mutex held) until notified or `seconds` pass.
    void wait(Mutex &mutex, double seconds);
    void notify()
    {
        pthread_cond_broadcast(&cond_);
    }

  private:
    pthread_cond_t cond_;
};

class Thread
{
  public:
    Thread() = default;
    ~Thread()
    {
        join();
    }
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;
    // Network workers get a stack of their own size: the default is small.
    // Threads are never named (pthread_setname_np hangs on the console).
    bool start(std::function<void()> body, std::size_t stack_bytes = 1u << 20);
    void join();
    bool running() const
    {
        return started_;
    }

  private:
    static void *entry(void *self);
    pthread_t thread_{};
    bool started_ = false;
    std::function<void()> body_;
};

// Work handed from a worker to the frame loop. post() from any thread; the
// frame loop calls drain() once per frame and runs what arrived, in order.
class Inbox
{
  public:
    void post(std::function<void()> item);
    void drain();

  private:
    Mutex mutex_;
    std::vector<std::function<void()>> items_;
};

} // namespace orbit
