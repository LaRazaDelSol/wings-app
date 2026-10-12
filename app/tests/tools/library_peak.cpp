// Wings TV app - Measures the peak C++ heap of reading the real 7 MB catalogue.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: library_peak <library.json>
//
// Serves the recorded GET /api/library reply through PegasusBackend (the real
// adapter, array-wise parse, trimmed packages, link cache) and reports how much
// heap the call needed at its peak, with every operator new counted. The app
// heap is a fixed 128 MiB, shared with textures and decode buffers; this tool
// fails (exit 1) when the peak is over 64 MiB. Built without sanitizers.

#include "orbit/pegasus_backend.hpp"

#include <malloc.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>

namespace
{
std::atomic<std::size_t> g_live{0};
std::atomic<std::size_t> g_peak{0};

void note_alloc(void *p)
{
    const std::size_t now = g_live.fetch_add(malloc_usable_size(p)) + malloc_usable_size(p);
    std::size_t peak = g_peak.load();
    while (now > peak && !g_peak.compare_exchange_weak(peak, now))
    {
    }
}
} // namespace

void *operator new(std::size_t n)
{
    void *p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr)
        std::abort();
    note_alloc(p);
    return p;
}
void operator delete(void *p) noexcept
{
    if (p == nullptr)
        return;
    g_live.fetch_sub(malloc_usable_size(p));
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept
{
    operator delete(p);
}
void *operator new[](std::size_t n)
{
    return operator new(n);
}
void operator delete[](void *p) noexcept
{
    operator delete(p);
}
void operator delete[](void *p, std::size_t) noexcept
{
    operator delete(p);
}

namespace
{

class FileTransport final : public wings::http::Transport
{
  public:
    explicit FileTransport(std::string library) : library_(std::move(library))
    {
    }
    wings::http::Response send(const wings::http::Request &request) override
    {
        wings::http::Response r;
        r.status = 200;
        r.content_type = "application/json";
        if (request.path == "/api/health")
            r.body = R"({"status":"running","port":6970,"downloadOnly":false,"nativePkgHandoff":true,"version":"1.11.0"})";
        else if (request.path == "/api/library")
            r.body = library_; // a socket would hand over its receive buffer: one copy
        else
        {
            r.status = 404;
            r.body = R"({"error":"Route not found"})";
        }
        return r;
    }

  private:
    std::string library_;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s library.json\n", argv[0]);
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    std::ostringstream text;
    text << file.rdbuf();
    const std::size_t reply_bytes = text.str().size();
    auto transport = std::make_shared<FileTransport>(text.str());
    text.str(std::string());

    orbit::PegasusBackend backend(transport, "127.0.0.1:6970");
    if (!backend.health().ok())
    {
        std::fprintf(stderr, "health failed\n");
        return 2;
    }
    const std::size_t before = g_live.load();
    g_peak.store(before);
    std::size_t peak = 0;
    std::size_t after = 0;
    std::size_t kept = 0;
    {
        orbit::Result<std::vector<orbit::Package>> result = backend.library();
        if (!result.ok())
        {
            std::fprintf(stderr, "library failed: %s\n", result.error.c_str());
            return 2;
        }
        peak = g_peak.load() - before;
        after = g_live.load() - before;
        const auto stats = backend.cache_stats();
        std::size_t grid = result.value.capacity() * sizeof(orbit::Package);
        kept = stats.bytes;
        std::printf("reply:            %zu bytes\n", reply_bytes);
        std::printf("packages:         %zu (%zu links cached)\n", result.value.size(), stats.links);
        std::printf("grid list:        ~%.1f MiB (what the Store snapshot keeps besides strings: %.1f MiB)\n",
                    static_cast<double>(g_live.load() - before - kept) / 1048576.0,
                    static_cast<double>(grid) / 1048576.0);
        std::printf("link cache:       ~%.1f MiB\n", static_cast<double>(kept) / 1048576.0);
        std::printf("live after call:  %.1f MiB (grid list + cache, reply freed)\n",
                    static_cast<double>(after) / 1048576.0);
        std::printf("PEAK during call: %.1f MiB\n", static_cast<double>(peak) / 1048576.0);
    }
    const bool ok = peak < (64u << 20);
    std::printf("%s: peak is %s 64 MiB (the app heap is 128 MiB in total)\n", ok ? "PASS" : "FAIL",
                ok ? "under" : "OVER");
    return ok ? 0 : 1;
}
