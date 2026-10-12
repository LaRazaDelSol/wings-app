// Wings core tests - runner.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstring>

#include "test_harness.hpp"

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const testing::Case &c : testing::registry())
    {
        if (filter && !std::strstr(c.name, filter))
            continue;
        const int before = testing::failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("[ %s ] %s\n", testing::failures() == before ? " OK " : "FAIL", c.name);
        ++ran;
    }
    std::printf("\n%d tests, %d checks, %d failures\n", ran, testing::checks(), testing::failures());
    return testing::failures() == 0 ? 0 : 1;
}
