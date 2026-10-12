// Wings cover-art module: threads, locks and clocks over plain pthreads.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/art/platform.hpp"

#include <time.h>

namespace wings::art
{

namespace
{
class SystemClock final : public Clock
{
  public:
    std::int64_t unix_seconds() const override
    {
        timespec t{};
        clock_gettime(CLOCK_REALTIME, &t);
        return static_cast<std::int64_t>(t.tv_sec);
    }
    std::int64_t mono_ms() const override
    {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return static_cast<std::int64_t>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
    }
};
} // namespace

const Clock &system_clock()
{
    static const SystemClock clock;
    return clock;
}

void Cond::wait(Mutex &m, int ms)
{
    if (ms <= 0)
        return;
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    long long nanos = deadline.tv_nsec + static_cast<long long>(ms % 1000) * 1000000LL;
    deadline.tv_sec += static_cast<time_t>(ms / 1000 + nanos / 1000000000LL);
    deadline.tv_nsec = static_cast<long>(nanos % 1000000000LL);
    pthread_cond_timedwait(&c_, m.native(), &deadline);
}

bool Thread::start(std::function<void()> body, std::size_t stack_bytes)
{
    if (started_)
        return false;
    body_ = std::move(body);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    started_ = pthread_create(&t_, &attr, &Thread::entry, this) == 0;
    pthread_attr_destroy(&attr);
    return started_;
}

void Thread::join()
{
    if (!started_)
        return;
    pthread_join(t_, nullptr);
    started_ = false;
}

void *Thread::entry(void *self)
{
    static_cast<Thread *>(self)->body_();
    return nullptr;
}

void sleep_ms(int ms)
{
    timespec t{ms / 1000, static_cast<long>(ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

} // namespace wings::art
