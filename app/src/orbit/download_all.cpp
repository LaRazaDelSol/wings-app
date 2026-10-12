// Wings TV app - "Download all parts": one part at a time, a saved record of what was queued.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/download_all.hpp"

#include "core/save_file.hpp"
#include "orbit/i18n.hpp"

#include <cstdlib>
#include <utility>

namespace orbit
{

std::string part_failure_reason(const LinkStart &outcome)
{
    if (!outcome.error.empty())
        return outcome.error;
    if (outcome.kind == LinkResult::Kind::blocked)
        return outcome.message.empty() ? std::string(tr("not supported")) : outcome.message;
    if (outcome.kind == LinkResult::Kind::select_files)
        return tr("the debrid service asks which files to take: open this part on its own");
    if (outcome.kind == LinkResult::Kind::browser_opened)
        return tr("it opened in the PS5 browser instead of the queue");
    return {};
}

void DownloadAll::begin(std::vector<LinkRef> refs, std::vector<int> numbers, std::string host_name, Start start,
                        std::function<void(const LinkRef &)> queued, std::function<void()> finished)
{
    ++generation_;
    refs_ = std::move(refs);
    numbers_ = std::move(numbers);
    numbers_.resize(refs_.size(), 0);
    host = std::move(host_name);
    total = static_cast<int>(refs_.size());
    done = 0;
    ok = 0;
    next_ = 0;
    end = End::none;
    failed_number = 0;
    failed_why.clear();
    cancel_ = false;
    start_ = std::move(start);
    queued_ = std::move(queued);
    finished_ = std::move(finished);
    active = total > 0;
    step();
}

void DownloadAll::cancel()
{
    if (active)
        cancel_ = true;
}

void DownloadAll::step()
{
    if (!active)
        return;
    if (cancel_)
    {
        finish(End::cancelled);
        return;
    }
    if (next_ >= total)
    {
        finish(End::finished);
        return;
    }
    const int index = next_;
    const int generation = generation_;
    start_(refs_[static_cast<std::size_t>(index)],
           [this, index, generation](const LinkStart &outcome)
           {
               if (generation == generation_ && active)
                   on_result(index, outcome);
           });
}

void DownloadAll::on_result(int index, const LinkStart &outcome)
{
    ++done;
    const std::string why = part_failure_reason(outcome);
    if (!why.empty())
    {
        failed_number = numbers_[static_cast<std::size_t>(index)];
        failed_why = why;
        finish(End::failed);
        return;
    }
    ++ok;
    if (queued_)
        queued_(refs_[static_cast<std::size_t>(index)]);
    next_ = index + 1;
    step();
}

void DownloadAll::finish(End how)
{
    active = false;
    end = how;
    std::function<void()> callback = finished_;
    if (callback)
        callback();
}

std::string DownloadAll::toast() const
{
    return trn(ok, "{count} part queued from {host}", "{count} parts queued from {host}", {{"host", host}});
}

std::string DownloadAll::report_title() const
{
    if (end == End::cancelled)
        return tr("Cancelled: {ok} of {total} parts queued",
                  {{"ok", std::to_string(ok)}, {"total", std::to_string(total)}});
    return tr("{ok} of {total} parts queued", {{"ok", std::to_string(ok)}, {"total", std::to_string(total)}});
}

std::string DownloadAll::report_message() const
{
    const std::string retry = tr("Choose \"Download all\" again to continue: parts already queued are left out.");
    if (end == End::cancelled)
        return tr("Stopped on your request after part {n} of {total}.",
                  {{"n", std::to_string(done)}, {"total", std::to_string(total)}}) +
               " " + retry;
    return tr("Stopped at part {n}: {why}.", {{"n", std::to_string(failed_number)}, {"why", failed_why}}) + " " +
           trn(ok, "{count} part was queued before it.", "{count} parts were queued before it.") + " " + retry;
}

// ---- the record of queued parts ----

std::string encode_queued_record(const StartedLinks &started)
{
    std::string out = "v=1\n";
    for (const std::string &key : started.keys)
    {
        std::string line = key;
        for (char &c : line)
            if (c == '\x1f')
                c = '\t';
        out += "q=" + line + "\n";
    }
    return out;
}

bool decode_queued_record(std::string_view payload, StartedLinks *out)
{
    if (payload.empty())
        return false;
    StartedLinks found;
    bool versioned = false;
    std::size_t at = 0;
    while (at < payload.size())
    {
        std::size_t end = payload.find('\n', at);
        if (end == std::string_view::npos)
            end = payload.size();
        const std::string_view line = payload.substr(at, end - at);
        at = end + 1;
        if (line.substr(0, 2) == "v=")
        {
            if (line.substr(2) != "1")
                return false; // a newer layout: leave the record alone
            versioned = true;
            continue;
        }
        if (line.substr(0, 2) != "q=")
            continue;
        const std::string_view pair = line.substr(2);
        const std::size_t tab = pair.find('\t');
        if (tab == std::string_view::npos || tab == 0 || tab > 128)
            continue;
        const std::string_view id = pair.substr(0, tab);
        const std::string_view index = pair.substr(tab + 1);
        bool plain = true;
        for (const char c : id)
            plain = plain && static_cast<unsigned char>(c) >= 0x20;
        if (!plain || index.empty() || index.size() > 6)
            continue;
        bool digits = true;
        for (const char c : index)
            digits = digits && c >= '0' && c <= '9';
        if (!digits)
            continue;
        found.add(LinkRef{std::string(id), std::atoi(std::string(index).c_str())});
    }
    if (!versioned)
        return false;
    *out = std::move(found);
    return true;
}

bool load_queued_record(const std::vector<std::string> &paths, StartedLinks *out)
{
    for (const std::string &path : paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, 1 << 20))
            continue;
        const hui::save::Decoded decoded = hui::save::decode(hui::save::Kind::prefs, data);
        if (decoded.ok && decoded.version == 1 && decode_queued_record(decoded.payload, out))
            return true;
    }
    return false;
}

int save_queued_record(const std::vector<std::string> &paths, const StartedLinks &started)
{
    const std::string data = hui::save::encode(hui::save::Kind::prefs, 1, encode_queued_record(started));
    int written = 0;
    for (const std::string &path : paths)
    {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            hui::save::ensure_directory(path.substr(0, slash));
        if (hui::save::write_atomic(path, data).empty())
            ++written;
    }
    return written;
}

std::vector<std::string> queued_record_paths(const std::vector<std::string> &prefs_paths)
{
    std::vector<std::string> out;
    for (const std::string &path : prefs_paths)
    {
        const std::size_t slash = path.rfind('/');
        out.push_back((slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "queued_parts.sav");
    }
    return out;
}

} // namespace orbit
