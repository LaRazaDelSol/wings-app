// Wings TV app - Threads, locks and a main-thread inbox over pthreads.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/thread.hpp"

#include <ctime>

namespace orbit
{

double now_seconds()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

double unix_seconds()
{
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

void Signal::wait(Mutex &mutex, double seconds)
{
    if (seconds <= 0.0)
        return;
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    const long long whole = static_cast<long long>(seconds);
    long long nanos =
        deadline.tv_nsec + static_cast<long long>((seconds - static_cast<double>(whole)) * 1e9);
    deadline.tv_sec += static_cast<time_t>(whole + nanos / 1000000000LL);
    deadline.tv_nsec = static_cast<long>(nanos % 1000000000LL);
    pthread_cond_timedwait(&cond_, mutex.native(), &deadline);
}

bool Thread::start(std::function<void()> body, std::size_t stack_bytes)
{
    if (started_)
        return false;
    body_ = std::move(body);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, stack_bytes);
    started_ = pthread_create(&thread_, &attributes, &Thread::entry, this) == 0;
    pthread_attr_destroy(&attributes);
    return started_;
}

void Thread::join()
{
    if (!started_)
        return;
    pthread_join(thread_, nullptr);
    started_ = false;
}

void *Thread::entry(void *self)
{
    static_cast<Thread *>(self)->body_();
    return nullptr;
}

void Inbox::post(std::function<void()> item)
{
    Lock lock(mutex_);
    items_.push_back(std::move(item));
}

void Inbox::drain()
{
    std::vector<std::function<void()>> items;
    {
        Lock lock(mutex_);
        items.swap(items_);
    }
    for (std::function<void()> &item : items)
        item();
}

} // namespace orbit
