// Wings cover-art module: threads, locks and clocks over plain pthreads.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console build links only part of the C++ runtime (no std::mutex,
// std::thread; see wings/app/src/orbit/thread.hpp), so the module uses the
// same pthread calls. No exceptions and no RTTI are needed anywhere in the
// module (the app is built with -fno-exceptions -fno-rtti).
#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace wings::art
{

// Time source. Tests inject a fake one to step over the 60 s / 24 h windows.
class Clock
{
  public:
    virtual ~Clock() = default;
    virtual std::int64_t unix_seconds() const = 0; // wall clock, for persisted expiry
    virtual std::int64_t mono_ms() const = 0;      // monotonic, for backoff windows
};
const Clock &system_clock();

class Mutex
{
  public:
    Mutex() { pthread_mutex_init(&m_, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&m_); }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
    pthread_mutex_t *native() { return &m_; }

  private:
    pthread_mutex_t m_;
};

class Lock
{
  public:
    explicit Lock(Mutex &m) : m_(m) { m_.lock(); }
    ~Lock() { m_.unlock(); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

  private:
    Mutex &m_;
};

class Cond
{
  public:
    Cond() { pthread_cond_init(&c_, nullptr); }
    ~Cond() { pthread_cond_destroy(&c_); }
    Cond(const Cond &) = delete;
    Cond &operator=(const Cond &) = delete;
    // Mutex held. Returns after a notify or at most `ms` (spurious wakeups are
    // fine: every caller re-checks its predicate).
    void wait(Mutex &m, int ms);
    void notify_all() { pthread_cond_broadcast(&c_); }

  private:
    pthread_cond_t c_;
};

class Thread
{
  public:
    Thread() = default;
    ~Thread() { join(); }
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;
    // Threads are never named (pthread_setname_np hangs on the console).
    bool start(std::function<void()> body, std::size_t stack_bytes = 1u << 20);
    void join();

  private:
    static void *entry(void *self);
    pthread_t t_{};
    bool started_ = false;
    std::function<void()> body_;
};

void sleep_ms(int ms);

} // namespace wings::art
