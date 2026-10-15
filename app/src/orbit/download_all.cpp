// Wings TV app - "Download all parts": one part at a time, a saved record of what was queued.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/download_all.hpp"

#include "core/save_file.hpp"
#include "orbit/i18n.hpp"

#include <cstdlib>
#include <utility>

namespace orbit
{

namespace
{

// "7", "7 and 12", "7, 12 and 33", "7, 12, 33 and 4 more".
std::string compact_list(const std::vector<std::string> &items)
{
    constexpr std::size_t kShown = 3;
    if (items.empty())
        return {};
    if (items.size() == 1)
        return items[0];
    const std::size_t head_count = items.size() <= kShown ? items.size() - 1 : kShown;
    std::string head = items[0];
    for (std::size_t i = 1; i < head_count; ++i)
        head += ", " + items[i];
    if (items.size() <= kShown)
        return tr("{first} and {last}", {{"first", head}, {"last", items.back()}});
    return trn(static_cast<long>(items.size() - kShown), "{list} and {count} more", "{list} and {count} more",
               {{"list", head}});
}

} // namespace

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
    if (outcome.kind == LinkResult::Kind::capture)
        return tr("still resolving"); // a capture that started is not a queued part
    return {};
}

void DownloadAll::begin(std::vector<PartPlan> parts, std::string host_name, Start start,
                        std::function<void(const LinkRef &)> queued, std::function<void()> finished)
{
    ++call_; // a Done still out from an earlier run is ignored
    parts_ = std::move(parts);
    host = std::move(host_name);
    total = static_cast<int>(parts_.size());
    done = 0;
    ok = 0;
    end = End::none;
    current = 0;
    attempt = 0;
    trying_host.clear();
    waiting = 0.0f;
    failed.clear();
    via_alternate = 0;
    alternates.clear();
    failed_number = 0;
    failed_why.clear();
    part_ = 0;
    ref_ = 0;
    tries_ = 0;
    in_a_row_ = 0;
    last_why_.clear();
    in_flight_ = false;
    waiting_ = false;
    cancel_ = false;
    start_ = std::move(start);
    queued_ = std::move(queued);
    finished_ = std::move(finished);
    active = total > 0;
    if (active)
        attempt_now();
}

void DownloadAll::begin(std::vector<LinkRef> refs, std::vector<int> numbers, std::string host_name, Start start,
                        std::function<void(const LinkRef &)> queued, std::function<void()> finished)
{
    std::vector<PartPlan> parts;
    parts.reserve(refs.size());
    for (std::size_t i = 0; i < refs.size(); ++i)
    {
        PartPlan part;
        part.number = i < numbers.size() ? numbers[i] : 0;
        part.refs.push_back(std::move(refs[i]));
        part.hosts.push_back(host_name);
        parts.push_back(std::move(part));
    }
    begin(std::move(parts), std::move(host_name), std::move(start), std::move(queued), std::move(finished));
}

void DownloadAll::tick(float dt)
{
    if (!active || !waiting_ || in_flight_)
        return;
    if (dt > 0.0f)
        waiting -= dt;
    if (waiting > 0.0f)
        return;
    waiting = 0.0f;
    waiting_ = false;
    attempt_now();
}

void DownloadAll::cancel()
{
    if (!active)
        return;
    cancel_ = true;
    // Ends at once (B-18): a part still out is let go, its late answer is ignored
    // (on_result sees the series ended) and the Store cancels what it queued.
    in_flight_ = false;
    finish(End::cancelled);
}

const std::string &DownloadAll::host_of(std::size_t ref) const
{
    const PartPlan &part = parts_[part_];
    if (ref < part.hosts.size() && !part.hosts[ref].empty())
        return part.hosts[ref];
    return host;
}

void DownloadAll::attempt_now()
{
    if (!active)
        return;
    if (cancel_)
    {
        finish(End::cancelled);
        return;
    }
    const PartPlan &part = parts_[part_];
    current = part.number;
    if (ref_ >= part.refs.size())
    {
        // Every host of this part was tried (or it has none).
        part_failed(last_why_.empty() ? std::string(tr("no link for this part")) : last_why_);
        return;
    }
    attempt = tries_ + 1;
    trying_host = host_of(ref_);
    waiting = 0.0f;
    waiting_ = false;
    in_flight_ = true;
    const int call = ++call_;
    const LinkRef ref = part.refs[ref_]; // a copy: Done may run inside start_ and move the series on
    start_(ref, [this, call](const LinkStart &outcome) { on_result(call, outcome); });
}

void DownloadAll::on_result(int call, const LinkStart &outcome)
{
    if (call != call_ || !in_flight_ || !active)
        return; // an older start, a second Done, or a series that ended
    in_flight_ = false;
    const std::string why = part_failure_reason(outcome);
    if (why.empty())
    {
        part_queued();
        return;
    }
    // A capture without an error has not decided anything yet: worth asking again.
    const bool resolving = outcome.error.empty() && outcome.kind == LinkResult::Kind::capture;
    const bool transient = outcome.transient || resolving;
    last_why_ = parts_[part_].refs.size() > 1 ? host_of(ref_) + ": " + why : why;
    ++tries_;
    if (cancel_)
    {
        finish(End::cancelled);
        return;
    }
    if (transient && tries_ < kPartAttempts)
    {
        waiting = tries_ == 1 ? kFirstBackoff : kSecondBackoff;
        waiting_ = true;
        return; // tick() starts the next attempt
    }
    ++ref_; // the next host of the same part, or the part failed
    tries_ = 0;
    attempt_now();
}

void DownloadAll::part_queued()
{
    const PartPlan &part = parts_[part_];
    ++done;
    ++ok;
    in_a_row_ = 0;
    if (ref_ > 0)
    {
        ++via_alternate;
        alternates.push_back(AlternatePart{part.number, host_of(ref_)});
    }
    const LinkRef ref = part.refs[ref_];
    if (queued_)
        queued_(ref);
    next_part();
}

void DownloadAll::part_failed(const std::string &why)
{
    ++done;
    failed.push_back(FailedPart{current, why});
    failed_number = current;
    failed_why = why;
    ++in_a_row_;
    if (in_a_row_ >= kStopAfterFailedParts && part_ + 1 < parts_.size())
    {
        finish(End::stopped);
        return;
    }
    next_part();
}

void DownloadAll::next_part()
{
    ++part_;
    ref_ = 0;
    tries_ = 0;
    attempt = 0;
    last_why_.clear();
    if (part_ >= parts_.size())
    {
        finish(failed.empty() ? End::finished : End::finished_with_failures);
        return;
    }
    if (cancel_)
    {
        finish(End::cancelled);
        return;
    }
    attempt_now();
}

void DownloadAll::finish(End how)
{
    active = false;
    in_flight_ = false;
    waiting_ = false;
    waiting = 0.0f;
    end = how;
    std::function<void()> callback = finished_;
    if (callback)
        callback();
}

std::string DownloadAll::toast() const
{
    std::string out =
        trn(ok, "{count} part queued from {host}", "{count} parts queued from {host}", {{"host", host}});
    if (via_alternate > 0)
        out += " " + trn(via_alternate, "({count} from another host)", "({count} from other hosts)");
    return out;
}

std::string DownloadAll::report_title() const
{
    const std::string ok_text = std::to_string(ok);
    const std::string total_text = std::to_string(total);
    if (end == End::cancelled)
        return tr("Cancelled: {ok} of {total} parts queued", {{"ok", ok_text}, {"total", total_text}});
    if (end == End::stopped)
        return tr("Stopped: {ok} of {total} parts queued", {{"ok", ok_text}, {"total", total_text}});
    return tr("{ok} of {total} parts queued", {{"ok", ok_text}, {"total", total_text}});
}

std::string DownloadAll::report_message() const
{
    std::string out;
    const auto add = [&out](const std::string &sentence)
    {
        if (sentence.empty())
            return;
        if (!out.empty())
            out += ' ';
        out += sentence;
    };
    if (end == End::cancelled)
        add(tr("Stopped on your request after part {n} of {total}.",
               {{"n", std::to_string(done)}, {"total", std::to_string(total)}}));
    else if (end == End::stopped)
        add(trn(kStopAfterFailedParts,
                "Pegasus seems stuck: {count} part in a row did not queue, so the rest was not tried.",
                "Pegasus seems stuck: {count} parts in a row did not queue, so the rest was not tried."));
    if (failed.size() == 1)
    {
        add(tr("Part {n} did not queue: {why}.", {{"n", std::to_string(failed[0].number)}, {"why", failed[0].why}}));
    }
    else if (failed.size() > 1)
    {
        std::vector<std::string> numbers;
        for (const FailedPart &part : failed)
            numbers.push_back(std::to_string(part.number));
        add(tr("Parts {list} did not queue. Last reason: {why}.",
               {{"list", compact_list(numbers)}, {"why", failed.back().why}}));
    }
    add(trn(ok, "{count} part was queued.", "{count} parts were queued."));
    if (!alternates.empty())
    {
        std::vector<std::string> items;
        for (const AlternatePart &part : alternates)
            items.push_back(tr("part {n} from {host}", {{"n", std::to_string(part.number)}, {"host", part.host}}));
        add(trn(via_alternate, "{count} part came from another host: {list}.",
                "{count} parts came from other hosts: {list}.", {{"list", compact_list(items)}}));
    }
    add(tr("Choose \"Download all\" again to continue: parts already queued are left out."));
    return out;
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
