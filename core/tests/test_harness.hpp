// Wings core tests - a tiny assert harness (no external framework).
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing
{

struct Case
{
    const char *name;
    std::function<void()> fn;
};

inline std::vector<Case> &registry()
{
    static std::vector<Case> r;
    return r;
}

struct Registrar
{
    Registrar(const char *name, std::function<void()> fn)
    {
        registry().push_back({name, std::move(fn)});
    }
};

inline int &failures()
{
    static int n = 0;
    return n;
}
inline int &checks()
{
    static int n = 0;
    return n;
}

inline void report(bool ok, const char *expr, const char *file, int line, const std::string &detail = std::string())
{
    ++checks();
    if (ok)
        return;
    ++failures();
    std::printf("    FAIL %s:%d  %s%s%s\n", file, line, expr, detail.empty() ? "" : "  -- ", detail.c_str());
}

inline std::string show(const std::string &s)
{
    return "\"" + s + "\"";
}

inline double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace testing

#define TEST(name)                                                            \
    static void test_##name();                                                \
    static testing::Registrar registrar_##name(#name, test_##name);           \
    static void test_##name()

#define CHECK(expr) testing::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_MSG(expr, msg) testing::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, (msg))
#define CHECK_EQ_STR(actual, expected)                                                                      \
    do                                                                                                      \
    {                                                                                                       \
        const std::string a_ = (actual);                                                                    \
        const std::string e_ = (expected);                                                                  \
        testing::report(a_ == e_, #actual " == " #expected, __FILE__, __LINE__,                             \
                        "got " + testing::show(a_) + " expected " + testing::show(e_));                     \
    } while (0)
#define CHECK_EQ_INT(actual, expected)                                                                      \
    do                                                                                                      \
    {                                                                                                       \
        const long long a_ = static_cast<long long>(actual);                                                \
        const long long e_ = static_cast<long long>(expected);                                              \
        testing::report(a_ == e_, #actual " == " #expected, __FILE__, __LINE__,                             \
                        "got " + std::to_string(a_) + " expected " + std::to_string(e_));                   \
    } while (0)
