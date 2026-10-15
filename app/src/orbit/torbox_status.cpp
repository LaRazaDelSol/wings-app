// Wings TV app - TorBox "still preparing" status and the wait that forces it through (B-19).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/torbox_status.hpp"

#include "orbit/i18n.hpp"

#include <cctype>
#include <cmath>

namespace orbit
{
namespace
{

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

bool is_torbox_preparing(std::string_view error)
{
    // Pegasus DL 1.11.0: "TorBox is still preparing this download; try again shortly".
    const std::string text = lower(error);
    if (!has(text, "torbox"))
        return false;
    return has(text, "still prepar") || has(text, "preparing this") || has(text, "being prepared") ||
           has(text, "still being cached") || has(text, "still caching");
}

void TorboxPrepareWait::start(double now)
{
    if (state_ != State::waiting && state_ != State::in_flight)
    {
        started_ = now;
        attempts_ = 0;
    }
    if (now - started_ >= kGiveUpAfter)
    {
        state_ = State::gave_up;
        return;
    }
    state_ = State::waiting;
    next_ = now + kRetryEvery;
}

TorboxPrepareWait::Tick TorboxPrepareWait::tick(double now)
{
    Tick t;
    switch (state_)
    {
    case State::idle:
        break;
    case State::in_flight:
        t.kind = Tick::Kind::in_flight;
        break;
    case State::gave_up:
        state_ = State::idle;
        t.kind = Tick::Kind::gave_up;
        break;
    case State::waiting:
        if (now >= next_)
        {
            state_ = State::in_flight;
            ++attempts_;
            t.kind = Tick::Kind::retry_now;
        }
        else
        {
            t.kind = Tick::Kind::waiting;
            t.seconds_left = static_cast<int>(std::ceil(next_ - now));
        }
        break;
    }
    return t;
}

void TorboxPrepareWait::cancel()
{
    state_ = State::idle;
    attempts_ = 0;
}

std::string torbox_waiting_text(int seconds)
{
    if (seconds <= 0)
        return tr("TorBox is getting this file ready on its server\xE2\x80\xA6 trying again now");
    return tr("TorBox is getting this file ready on its server\xE2\x80\xA6 trying again in {s} s",
              {{"s", std::to_string(seconds)}});
}

std::string torbox_asking_text()
{
    return tr("TorBox is getting this file ready on its server\xE2\x80\xA6");
}

std::string torbox_gave_up_text()
{
    return tr("TorBox is still preparing this file. Try again later, or open TorBox to check it.");
}

std::string torbox_preparing_title()
{
    return tr("TorBox is still preparing this file");
}

std::string torbox_card_label()
{
    return tr("Preparing on TorBox");
}

} // namespace orbit
