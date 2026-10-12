// Wings core tests - shared helpers.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <memory>
#include <string>

#include "wings/pegasus_v1.hpp"
#include "wings/replay_transport.hpp"

#ifndef WINGS_SAMPLES_DIR
#define WINGS_SAMPLES_DIR "../../recon/samples"
#endif
#ifndef WINGS_FIXTURES_DIR
#define WINGS_FIXTURES_DIR "tests/fixtures"
#endif

inline std::string samples_dir()
{
    const char *env = std::getenv("WINGS_SAMPLES");
    return env && *env ? env : WINGS_SAMPLES_DIR;
}

inline std::string sample(const std::string &name)
{
    return samples_dir() + "/" + name;
}

inline std::string fixture(const std::string &name)
{
    return std::string(WINGS_FIXTURES_DIR) + "/" + name;
}

struct Rig
{
    std::shared_ptr<wings::http::ReplayTransport> t = std::make_shared<wings::http::ReplayTransport>();
    wings::PegasusV1 api{t};
};

// Reads a whole file or aborts the test run with a clear message.
inline std::string must_read(const std::string &path)
{
    bool ok = false;
    std::string text = wings::http::read_text_file(path, &ok);
    if (!ok)
    {
        std::fprintf(stderr, "cannot read %s (cwd must be wings/core, or set WINGS_SAMPLES)\n", path.c_str());
        std::exit(2);
    }
    return text;
}
