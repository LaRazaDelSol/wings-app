// Wings TV app - A fake cover art switch for the PC preview and tests.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stands in for the console's ConsoleCoverArt (main.cpp), which sits on the real
// HTTPS art service. It says what the service says in each state and, when
// DNS-over-HTTPS is switched on, tells the FakeBackend to start answering store
// poster keys with generated pictures. Preview and tests only.

#pragma once

#include "fake_backend.hpp"

#include "orbit/cover_art.hpp"

namespace orbit::host
{

class FakeCoverArt final : public CoverArtControl
{
  public:
    explicit FakeCoverArt(FakeBackend &backend, bool doh = false) : backend_(backend)
    {
        backend_.set_store_art(doh);
    }

    CoverArtStatus status() override
    {
        CoverArtStatus out;
        out.available = true;
        out.doh = backend_.store_art();
        out.working = out.doh;
        out.detail = out.doh ? "Cover art is on."
                          : "Cover art is off. Turn on DNS-over-HTTPS in Settings to download "
                            "covers (the host names of the art servers are then sent to 1.1.1.1 "
                            "or 8.8.8.8). Covers that are already downloaded still show.";
        return out;
    }
    void set_doh(bool on) override
    {
        backend_.set_store_art(on);
        ++changes;
    }
    void clear_cache() override
    {
        ++cleared;
    }
    void retry_failed() override
    {
        ++retried;
    }

    bool doh() const
    {
        return backend_.store_art();
    }
    int changes = 0;
    int cleared = 0;
    int retried = 0;

  private:
    FakeBackend &backend_;
};

} // namespace orbit::host
