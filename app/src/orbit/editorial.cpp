// Wings TV app - Editorial data: see editorial.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/editorial.hpp"

#include "core/save_file.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"

#include <algorithm>
#include <cstdlib>

namespace orbit
{

namespace
{
// Version check shared by both files; same wording as the popularity data.
bool check_header(std::string_view text, std::size_t max_bytes, json::Value *root, std::string *error)
{
    const auto fail = [&](const std::string &why)
    {
        if (error != nullptr)
            *error = why;
        return false;
    };
    if (text.size() > max_bytes)
        return fail("the file is too large");
    std::string why;
    if (!json::parse(text, root, &why) || !root->is_object())
        return fail("the file is not valid JSON");
    if (!(*root)["schemaVersion"].is_number())
        return fail("unsupported data version");
    if ((*root)["schemaVersion"].as_int(0) > 1)
        return fail(tr("this data needs a newer Wings (Settings > Wings > Updates)"));
    if ((*root)["schemaVersion"].as_int(0) != 1)
        return fail("unsupported data version");
    return true;
}

// At least one character, no control characters (the length is not limited; the page scales the type).
bool valid_note(const std::string &text)
{
    std::size_t chars = 0;
    for (const char c : text)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f)
            return false;
        if ((u & 0xC0) != 0x80)
            ++chars;
    }
    return chars >= 1;
}

enum class Part
{
    ok,
    absent, // HTTP 404: the file is not published
    failed
};

Part fetch_part(DocFetcher &fetcher, const std::string &url, std::size_t max_bytes, bool is_notes, std::string *body,
                std::string *problem)
{
    if (!valid_popularity_url(url))
    {
        *problem = "the source URL must start with https://";
        return Part::failed;
    }
    const DocResult doc = fetcher.get(url, max_bytes);
    if (doc.status == 404 && doc.error.empty())
        return Part::absent;
    if (!doc.error.empty() || doc.status == 0)
    {
        *problem = doc.error.empty() ? std::string("no answer") : doc.error;
        return Part::failed;
    }
    if (doc.status != 200)
    {
        *problem = "HTTP " + std::to_string(doc.status);
        return Part::failed;
    }
    EditorialData probe;
    const bool good = is_notes ? EditorialData::parse_notes(doc.body, &probe, problem)
                               : EditorialData::parse_curated(doc.body, &probe, problem);
    if (!good)
        return Part::failed;
    *body = doc.body;
    return Part::ok;
}
} // namespace

// ---- the data ----

bool valid_editorial_id(std::string_view id)
{
    if (id.size() != 9 || (id.substr(0, 4) != "PPSA" && id.substr(0, 4) != "CUSA"))
        return false;
    for (std::size_t i = 4; i < 9; ++i)
        if (id[i] < '0' || id[i] > '9')
            return false;
    return true;
}

bool EditorialData::parse_notes(std::string_view text, EditorialData *into, std::string *error)
{
    json::Value root;
    if (!check_header(text, kEditorialNotesMaxBytes, &root, error))
        return false;
    const json::Value &notes = root["notes"];
    if (!notes.is_object())
    {
        if (error != nullptr)
            *error = "the file has no notes";
        return false;
    }
    std::map<std::string, std::string> kept;
    for (const std::string &id : notes.keys())
    {
        if (kept.size() >= kEditorialMaxNotes)
            break;
        const json::Value &v = notes[id];
        if (!valid_editorial_id(id) || !v.is_string() || !valid_note(v.as_string()))
            continue;
        kept[id] = v.as_string();
    }
    into->notes_ = std::move(kept);
    return true;
}

bool EditorialData::parse_curated(std::string_view text, EditorialData *into, std::string *error)
{
    json::Value root;
    if (!check_header(text, kEditorialCuratedMaxBytes, &root, error))
        return false;
    const json::Value &items = root["items"];
    if (!items.is_array())
    {
        if (error != nullptr)
            *error = "the file has no items";
        return false;
    }
    std::vector<std::string> kept;
    for (const json::Value &item : items.items())
    {
        if (kept.size() >= kEditorialMaxCurated)
            break;
        if (!item.is_string() || !valid_editorial_id(item.as_string()))
            continue;
        bool seen = false;
        for (const std::string &id : kept)
            seen = seen || id == item.as_string();
        if (!seen)
            kept.push_back(item.as_string());
    }
    into->curated_ = std::move(kept);
    return true;
}

std::string EditorialData::note(const std::vector<std::string> &title_ids) const
{
    for (const std::string &id : title_ids)
    {
        const auto it = notes_.find(id);
        if (it != notes_.end())
            return it->second;
    }
    return {};
}

// ---- the hub ----

void EditorialHub::set_enabled(bool notes, bool carousel)
{
    Lock lock(mutex_);
    if (notes_ != notes || carousel_ != carousel)
    {
        notes_ = notes;
        carousel_ = carousel;
        ++serial_;
    }
}

bool EditorialHub::notes_enabled() const
{
    Lock lock(mutex_);
    return notes_;
}

bool EditorialHub::carousel_enabled() const
{
    Lock lock(mutex_);
    return carousel_;
}

void EditorialHub::set_data(std::shared_ptr<const EditorialData> data)
{
    Lock lock(mutex_);
    data_ = std::move(data);
    ++serial_;
}

std::shared_ptr<const EditorialData> EditorialHub::data() const
{
    Lock lock(mutex_);
    return data_;
}

std::string EditorialHub::note(const std::vector<std::string> &title_ids) const
{
    std::shared_ptr<const EditorialData> data;
    {
        Lock lock(mutex_);
        if (notes_)
            data = data_;
    }
    return data ? data->note(title_ids) : std::string();
}

std::vector<std::string> EditorialHub::curated_ids() const
{
    std::shared_ptr<const EditorialData> data;
    {
        Lock lock(mutex_);
        if (carousel_)
            data = data_;
    }
    return data ? data->curated_ids() : std::vector<std::string>();
}

void EditorialHub::set_status(const EditorialStatus &status)
{
    Lock lock(mutex_);
    status_ = status;
    ++serial_;
}

EditorialStatus EditorialHub::status() const
{
    Lock lock(mutex_);
    return status_;
}

std::uint64_t EditorialHub::serial() const
{
    Lock lock(mutex_);
    return serial_;
}

EditorialHub &editorial_hub()
{
    static EditorialHub hub;
    return hub;
}

// ---- the service ----

EditorialService::EditorialService(EditorialHub &hub, DocFetcher &fetcher, EditorialServiceConfig config)
    : hub_(hub), fetcher_(fetcher), config_(std::move(config))
{
    if (!config_.now)
        config_.now = [] { return static_cast<std::int64_t>(unix_seconds()); };
}

EditorialService::~EditorialService()
{
    thread_.join();
}

void EditorialService::apply(bool notes, bool carousel)
{
    hub_.set_enabled(notes, carousel);
    if ((!notes && !carousel) || started_)
        return;
    started_ = true;
    EditorialStatus status = hub_.status();
    status.busy = true;
    hub_.set_status(status);
    if (!thread_.start([this] { run(); }, 256u << 10))
    {
        status.busy = false;
        status.message = "Could not start the update";
        hub_.set_status(status);
    }
}

void EditorialService::wait_idle()
{
    thread_.join();
}

void EditorialService::run()
{
    EditorialStatus status;
    status.busy = true;
    std::string notes_body; // the raw files in use; "" = none
    std::string curated_body;
    bool cache_ok = false;
    bool cache_current = false;
    std::int64_t fetched = 0;
    const std::size_t cap = kEditorialNotesMaxBytes + kEditorialCuratedMaxBytes + 4096;
    // 1. The saved copy: "fetched=<unix>\napp=<build tag>\nnotes=<bytes>\ncurated=<bytes>\n<notes><curated>".
    for (const std::string &path : config_.cache_paths)
    {
        std::string data;
        if (!hui::save::read_file(path, &data, cap + 4096))
            continue;
        const hui::save::Decoded d = hui::save::decode(hui::save::Kind::stats, data);
        if (!d.ok || d.version != 1)
            continue;
        const std::string &p = d.payload;
        std::size_t at = 0;
        std::string tag;
        long long t = 0, nlen = -1, clen = -1;
        bool header_ok = true;
        for (int line = 0; line < 4 && header_ok; ++line)
        {
            const std::size_t nl = p.find('\n', at);
            if (nl == std::string::npos)
            {
                header_ok = false;
                break;
            }
            const std::string row = p.substr(at, nl - at);
            at = nl + 1;
            if (line == 0 && row.compare(0, 8, "fetched=") == 0)
                t = std::atoll(row.c_str() + 8);
            else if (line == 1 && row.compare(0, 4, "app=") == 0)
                tag = row.substr(4);
            else if (line == 2 && row.compare(0, 6, "notes=") == 0)
                nlen = std::atoll(row.c_str() + 6);
            else if (line == 3 && row.compare(0, 8, "curated=") == 0)
                clen = std::atoll(row.c_str() + 8);
            else
                header_ok = false;
        }
        if (!header_ok || nlen < 0 || clen < 0 || static_cast<std::size_t>(nlen + clen) != p.size() - at)
            continue;
        notes_body = p.substr(at, static_cast<std::size_t>(nlen));
        curated_body = p.substr(at + static_cast<std::size_t>(nlen));
        fetched = t;
        cache_ok = true;
        cache_current = tag == config_.app_tag;
        break;
    }
    // Parses what is held and hands it to the hub; a half that no longer parses is dropped.
    const auto publish = [&]()
    {
        auto data = std::make_shared<EditorialData>();
        std::string why;
        if (!notes_body.empty() && !EditorialData::parse_notes(notes_body, data.get(), &why))
            notes_body.clear();
        if (!curated_body.empty() && !EditorialData::parse_curated(curated_body, data.get(), &why))
            curated_body.clear();
        hub_.set_data(data);
    };
    publish();
    const std::int64_t now = config_.now();
    if (cache_ok)
    {
        status.updated = fetched;
        status.message = "Using the saved copy";
    }
    // A file that was not there is looked for again soon: it may simply not have been published yet.
    const int ttl = (notes_body.empty() || curated_body.empty()) ? std::min(config_.ttl_s, 600) : config_.ttl_s;
    if (cache_ok && cache_current && fetched <= now && now - fetched < ttl)
    {
        status.busy = false;
        hub_.set_status(status);
        return;
    }
    hub_.set_status(status);
    // 2. The fetch of both files. A failed file keeps its saved copy; a 404 means that file is not published.
    std::string problem;
    const Part notes = fetch_part(fetcher_, config_.notes_url, kEditorialNotesMaxBytes, true, &notes_body, &problem);
    if (notes == Part::absent)
        notes_body.clear();
    std::string curated_problem;
    const Part curated =
        fetch_part(fetcher_, config_.curated_url, kEditorialCuratedMaxBytes, false, &curated_body, &curated_problem);
    if (curated == Part::absent)
        curated_body.clear();
    if (problem.empty())
        problem = curated_problem;
    if (notes != Part::failed || curated != Part::failed)
    {
        publish();
        write_everywhere(config_.cache_paths,
                         hui::save::encode(hui::save::Kind::stats, 1,
                                           "fetched=" + std::to_string(now) + "\napp=" + config_.app_tag +
                                               "\nnotes=" + std::to_string(notes_body.size()) +
                                               "\ncurated=" + std::to_string(curated_body.size()) + "\n" + notes_body +
                                               curated_body),
                         nullptr);
        status.updated = now;
        status.message = problem.empty() ? "Updated" : "Updated in part: " + problem;
    }
    else
        status.message = "Could not refresh: " + problem + (cache_ok ? ". Using the saved copy" : "");
    status.busy = false;
    hub_.set_status(status);
}

} // namespace orbit
