// Wings TV app - Known archive passwords: the list and the attempt state machine.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/archive_passwords.hpp"

#include "orbit/i18n.hpp"

namespace orbit
{

const std::array<const char *, kKnownPasswordCount> &known_archive_passwords()
{
    static const std::array<const char *, kKnownPasswordCount> list = {
        "DLPSGAME.COM", "[DLPSGAME.COM]", "SuperPSX", "www.DLPSGAME.COM", "downloadgameps3.com", "hako",
    };
    return list;
}

std::string tried_note(int tried)
{
    return tr("Tried {count} known passwords", {{"count", std::to_string(tried)}});
}

PasswordTrials::Step PasswordTrials::send_step(Trial &trial, std::string_view detail)
{
    Step step;
    step.kind = Step::Kind::send;
    step.password = known_archive_passwords()[static_cast<std::size_t>(trial.tried)];
    ++trial.tried;
    step.number = trial.tried;
    step.total = static_cast<int>(kKnownPasswordCount);
    trial.detail_at_send = std::string(detail);
    trial.left = false;
    trial.phase = Phase::awaiting; // the clock starts at sent(); until then the settle test is off
    trial.sent_at = -1;
    return step;
}

PasswordTrials::Step PasswordTrials::prompt_step(Trial &trial, std::string_view error)
{
    trial.phase = Phase::prompt;
    trial.prompt_left = false;
    Step step;
    step.kind = Step::Kind::prompt;
    step.number = trial.tried;
    step.total = static_cast<int>(kKnownPasswordCount);
    step.note = tried_note(trial.tried);
    if (!error.empty())
        step.note += " - " + std::string(error);
    return step;
}

void PasswordTrials::sent(const std::string &key, std::int64_t now_ms)
{
    const auto it = trials_.find(key);
    if (it != trials_.end() && it->second.phase == Phase::awaiting)
        it->second.sent_at = now_ms;
}

PasswordTrials::Step PasswordTrials::send_failed(const std::string &key, std::string_view error)
{
    return prompt_step(trials_[key], error);
}

PasswordTrials::Step PasswordTrials::observe(const std::string &key, Seen seen, std::string_view detail,
                                             std::int64_t now_ms)
{
    if (seen == Seen::finished || seen == Seen::ended)
    {
        if (trials_.erase(key) == 0)
            return {};
        Step done;
        done.kind = Step::Kind::done;
        return done;
    }
    const auto found = trials_.find(key);
    if (found == trials_.end() && seen != Seen::waiting)
        return {};
    Trial &trial = trials_[key];

    switch (trial.phase)
    {
    case Phase::next:
        if (seen != Seen::waiting)
            return {};
        if (trial.tried >= static_cast<int>(kKnownPasswordCount))
            return prompt_step(trial, {});
        return send_step(trial, detail);

    case Phase::awaiting:
        if (seen == Seen::active)
        {
            trial.left = true;
            return {};
        }
        // Waiting again. The password did not work when the job went away and came
        // back, when its text changed, or when nothing moved for the settle time.
        if (trial.left || detail != trial.detail_at_send ||
            (trial.sent_at >= 0 && now_ms - trial.sent_at >= kSettleMs))
        {
            if (trial.tried >= static_cast<int>(kKnownPasswordCount))
                return prompt_step(trial, {});
            return send_step(trial, detail);
        }
        return {};

    case Phase::prompt:
        // The player's own try left the job waiting: ask again, once per try.
        if (seen == Seen::active)
        {
            trial.prompt_left = true;
            return {};
        }
        if (trial.user_sent && (trial.prompt_left || detail != trial.detail_at_send ||
                                (trial.sent_at >= 0 && now_ms - trial.sent_at >= kSettleMs)))
        {
            trial.user_sent = false;
            return prompt_step(trial, tr("That password did not work"));
        }
        return {};
    }
    return {};
}

void PasswordTrials::set_user_password(const std::string &key, std::string password,
                                       std::string_view detail, std::int64_t now_ms)
{
    Trial &trial = trials_[key];
    trial.user_password = std::move(password);
    trial.has_user_password = true;
    trial.phase = Phase::prompt;
    trial.prompt_left = false;
    trial.user_sent = true;
    trial.detail_at_send = std::string(detail);
    trial.sent_at = now_ms;
}

const std::string *PasswordTrials::user_password(const std::string &key) const
{
    const auto it = trials_.find(key);
    if (it == trials_.end() || !it->second.has_user_password)
        return nullptr;
    return &it->second.user_password;
}

int PasswordTrials::trying_number(const std::string &key) const
{
    const auto it = trials_.find(key);
    if (it == trials_.end() || it->second.phase == Phase::prompt)
        return 0;
    return it->second.tried;
}

std::vector<std::string> PasswordTrials::keys() const
{
    std::vector<std::string> out;
    for (const auto &entry : trials_)
        out.push_back(entry.first);
    return out;
}

int PasswordTrials::tried_count(const std::string &key) const
{
    const auto it = trials_.find(key);
    return it == trials_.end() ? 0 : it->second.tried;
}

} // namespace orbit
