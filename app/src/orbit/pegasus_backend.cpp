// Wings TV app - The real backend: Pegasus DL through the wings/core adapter.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_backend.hpp"

#include "wings/logic.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <utility>

namespace orbit
{

namespace
{

constexpr std::size_t kDescriptionBytes = 420;

std::int64_t to_id(std::string_view text)
{
    const std::string copy(text);
    char *end = nullptr;
    const long long value = std::strtoll(copy.c_str(), &end, 10);
    return end != nullptr && *end == '\0' && !copy.empty() ? value : 0;
}

const char *status_name(wings::JobStatus status)
{
    using wings::JobStatus;
    switch (status)
    {
    case JobStatus::queued:
        return "queued";
    case JobStatus::starting:
        return "starting";
    case JobStatus::downloading:
        return "downloading";
    case JobStatus::merging:
        return "merging";
    case JobStatus::extracting:
        return "extracting";
    case JobStatus::finalizing:
        return "finalizing";
    case JobStatus::waitingPassword:
        return "waitingPassword";
    case JobStatus::waitingChoice:
        return "waitingChoice";
    case JobStatus::pausing:
        return "pausing";
    case JobStatus::cancelling:
        return "cancelling";
    case JobStatus::paused:
        return "paused";
    case JobStatus::completed:
        return "completed";
    case JobStatus::failed:
        return "failed";
    case JobStatus::cancelled:
        return "cancelled";
    case JobStatus::unknown:
        break;
    }
    return "unknown";
}

const char *phase_name(wings::JobPhase phase)
{
    using wings::JobPhase;
    switch (phase)
    {
    case JobPhase::download:
        return "download";
    case JobPhase::merge:
        return "merge";
    case JobPhase::extract:
        return "extract";
    case JobPhase::finish:
        return "finish";
    default:
        return "";
    }
}

const char *capture_name(wings::CaptureState state)
{
    using wings::CaptureState;
    switch (state)
    {
    case CaptureState::idle:
        return "priming";
    case CaptureState::priming:
        return "priming";
    case CaptureState::watching:
        return "watching";
    case CaptureState::queueing:
        return "queueing";
    case CaptureState::completed:
        return "completed";
    case CaptureState::failed:
        return "failed";
    case CaptureState::cancelled:
        return "cancelled";
    case CaptureState::unknown:
        break;
    }
    return "priming";
}

Job to_job(const wings::QueueJob &j)
{
    Job job;
    job.id = std::to_string(j.id);
    job.name = !j.title.empty() ? j.title
               : !j.fileName.empty() ? j.fileName
                                     : "Download " + std::to_string(j.id);
    job.title_id = j.titleId;
    job.source_name = j.sourceName;
    job.status = status_name(j.status);
    job.phase = phase_name(j.phase);
    job.detail = j.detail;
    job.error = j.error;
    job.entry = j.currentArchiveEntry;
    job.received = static_cast<double>(j.bytesDone);
    job.total = static_cast<double>(j.sizeBytes);
    job.speed = static_cast<double>(j.speedBps > 0 ? j.speedBps : j.phaseSpeedBps);
    job.eta_seconds = j.etaSeconds > 0 ? static_cast<double>(j.etaSeconds) : -1.0;
    // Merging, extracting and finishing report their own progress; plain
    // downloading is bytes done over bytes total.
    if (j.status == wings::JobStatus::extracting && j.extractedTotalBytes > 0)
        job.progress = static_cast<double>(j.extractedBytesDone) /
                       static_cast<double>(j.extractedTotalBytes);
    else if ((j.status == wings::JobStatus::merging || j.status == wings::JobStatus::extracting ||
              j.status == wings::JobStatus::finalizing) &&
             j.phaseProgress > 0.0)
        job.progress = j.phaseProgress;
    job.attempts = static_cast<int>(j.attempts);
    job.max_attempts = static_cast<int>(j.maxAttempts);
    for (const wings::ArchiveCandidate &c : j.archiveCandidates)
        job.choices.push_back({static_cast<int>(c.index), c.name});
    return job;
}

std::vector<Job> to_jobs(const wings::QueueSnapshot &queue)
{
    std::vector<Job> jobs;
    jobs.reserve(queue.jobs.size());
    for (const wings::QueueJob &j : queue.jobs)
        jobs.push_back(to_job(j));
    return jobs;
}

Source to_source(const wings::Source &s)
{
    Source source;
    source.id = std::to_string(s.id);
    source.name = s.name;
    source.kind = s.managed && s.kind.empty() ? "default" : s.kind;
    source.url = s.url;
    source.enabled = s.enabled;
    source.managed = s.managed;
    source.deletable = s.deletable;
    source.is_new = s.isNew;
    source.package_count = static_cast<int>(s.packageCount);
    source.last_refresh_at = s.lastRefreshAt;
    source.status = s.status;
    return source;
}

std::vector<Source> to_sources(const std::vector<wings::Source> &list)
{
    std::vector<Source> out;
    out.reserve(list.size());
    for (const wings::Source &s : list)
        out.push_back(to_source(s));
    return out;
}

Package to_package(const wings::Package &p)
{
    Package out;
    out.id = std::to_string(p.id);
    out.title = p.title;
    out.title_id = p.titleId;
    out.source_name = p.sourceName;
    out.source_id = std::to_string(p.sourceId);
    out.version = p.version;
    out.poster_url = p.posterUrl;
    out.description = p.description;
    out.size_bytes = static_cast<double>(p.sizeBytes);
    out.link_count = static_cast<int>(p.downloadLinks.size());
    for (const wings::DownloadLink &link : p.downloadLinks)
    {
        const std::string url = wings::logic::downloadLinkUrl(link.url);
        if (url.empty())
            continue;
        if (wings::logic::urlHasPkgExtension(url))
            out.has_pkg = true;
        else
        {
            const wings::logic::DownloadSupport support = wings::logic::downloadSupportForUrl(url);
            if (support.automated)
                out.has_automated = true;
            else if (support.kind == wings::logic::SupportKind::directBrowser)
                out.has_supported = true;
        }
    }
    return out;
}

LinkRoute to_route(wings::logic::LinkActionKind kind)
{
    using wings::logic::LinkActionKind;
    switch (kind)
    {
    case LinkActionKind::nativePkg:
        return LinkRoute::native_pkg;
    case LinkActionKind::debrid:
        return LinkRoute::debrid;
    case LinkActionKind::chooseDebrid:
        return LinkRoute::choose_debrid;
    case LinkActionKind::browserOnly:
        return LinkRoute::browser_only;
    case LinkActionKind::blocked:
        return LinkRoute::blocked;
    case LinkActionKind::automatedCapture:
        return LinkRoute::automated;
    case LinkActionKind::manualCapture:
        return LinkRoute::capture;
    case LinkActionKind::linkNotFound:
        break;
    }
    return LinkRoute::missing;
}

Dispatch to_dispatch(const wings::QueueResponse &response, const std::string &queued_message)
{
    const wings::logic::DispatchOutcome outcome =
        wings::logic::finishDispatchResponse(response, queued_message);
    Dispatch d;
    d.has_jobs = outcome.queueIncluded;
    if (d.has_jobs)
        d.jobs = to_jobs(response.queue);
    d.queued = !outcome.queued.empty();
    d.toast = outcome.toast;
    d.native = !outcome.native.empty();
    if (outcome.showNativeConfirmation)
    {
        d.native_title = outcome.confirmation.title;
        d.native_message = outcome.confirmation.message;
    }
    return d;
}

DebridView to_debrid(const wings::DebridStatus &d)
{
    DebridView v;
    v.configured = d.configured;
    v.valid = d.valid;
    v.premium = d.premium;
    v.account_type = d.accountType;
    v.hosters = d.hosterCount;
    v.domains = d.domainCount;
    v.last_checked_at = d.lastCheckedAt;
    v.error = d.error;
    if (v.domains == 0)
        v.domains = static_cast<std::int64_t>(d.domains.size());
    return v;
}

SettingsView to_settings_view(const wings::Settings &s)
{
    SettingsView v;
    v.download_root = s.downloadRoot;
    v.connections = s.downloadConnections;
    v.real_debrid_ready = wings::logic::debridReady(s.realDebrid);
    v.torbox_ready = wings::logic::debridReady(s.torBox);
    v.real_debrid_configured = s.realDebrid.configured;
    v.torbox_configured = s.torBox.configured;
    v.dns_helper_running = s.dnsHelper.nanodnsRunning;
    v.real_debrid = to_debrid(s.realDebrid);
    v.torbox = to_debrid(s.torBox);
    v.dns_present = s.dnsHelper.present;
    v.dns_available = s.dnsHelper.available;
    v.dns_managed = s.dnsHelper.managed;
    v.dns_error = s.dnsHelper.startError;
    v.dns_primary = s.dnsHelper.recommendedPrimaryDns;
    v.download_only = s.downloadOnly;
    v.queue_persists = s.queuePersists;
    v.range_resume = s.rangeResume;
    for (const wings::logic::Provider &provider : wings::logic::automatedProviders())
        v.providers.push_back(provider.name);
    return v;
}

StorageMount to_mount(const wings::Mount &m)
{
    StorageMount out;
    out.path = m.path;
    out.name = m.name.empty() ? m.path : m.name;
    out.type = m.type;
    out.writable = m.writable;
    out.total_bytes = static_cast<double>(m.totalBytes);
    out.free_bytes = static_cast<double>(m.freeBytes);
    return out;
}

FileItem to_item(const wings::FileEntry &e)
{
    FileItem out;
    out.name = e.name;
    out.path = e.path;
    out.directory = e.directory || e.type == "dir";
    out.symlink = e.symlink;
    out.writable = e.writable;
    out.size_bytes = static_cast<double>(e.sizeBytes);
    out.modified = e.modifiedTime;
    return out;
}

FileOp to_op(const wings::FileOperation &o)
{
    FileOp op;
    op.id = std::to_string(o.operationId);
    op.type = o.type;
    // Pegasus sends waiting_password; one spelling for the whole app.
    op.status = o.statusRaw.empty() ? "running"
                : o.statusRaw == "waitingPassword" ? std::string("waiting_password")
                                                   : o.statusRaw;
    op.source = o.source;
    op.destination = o.destination;
    op.current = o.currentPath;
    op.error = o.error;
    op.total_items = o.totalItems;
    op.done_items = o.doneItems;
    op.total_bytes = static_cast<double>(o.totalBytes);
    op.done_bytes = static_cast<double>(o.doneBytes);
    op.cancelable = o.cancelable;
    op.cancel_requested = o.cancelRequested;
    return op;
}

AprView to_apr(const wings::AprEmu &a)
{
    AprView v;
    v.present = a.present;
    switch (wings::logic::aprEmuState(a))
    {
    case wings::logic::AprState::update:
        v.state = "update";
        break;
    case wings::logic::AprState::unavailable:
        v.state = "unavailable";
        break;
    case wings::logic::AprState::current:
        v.state = "current";
        break;
    case wings::logic::AprState::unsupported:
        v.state = "unsupported";
        break;
    case wings::logic::AprState::missing:
        v.state = "missing";
        break;
    case wings::logic::AprState::unknown:
        v.state = "unknown";
        break;
    }
    v.version_text = wings::logic::aprEmuVersionText(a);
    v.latest = a.latestVersion;
    v.update_available = a.updateAvailable;
    v.can_update = a.canUpdate;
    v.can_reinstall = a.canReinstall;
    v.remote_available = a.remoteAvailable;
    v.remount_required = a.remountRequired;
    v.reason = a.reason;
    v.manifest_error = a.manifestError;
    return v;
}

AdminJob to_admin(const wings::AprJob &j)
{
    AdminJob out;
    out.job_id = j.jobId;
    out.title_id = j.titleId;
    out.status = j.status.empty() ? "starting" : j.status;
    out.progress = j.progress;
    out.detail = j.detail;
    out.error = j.error;
    return out;
}

AdminJob to_admin(const wings::DeleteJob &j)
{
    AdminJob out;
    out.job_id = j.jobId;
    out.title_id = j.titleId;
    out.status = j.status.empty() ? "starting" : j.status;
    out.detail = j.detail;
    out.error = j.error;
    out.files_deleted = j.filesDeleted;
    return out;
}

// Takes the stripped packages from the streamed catalogue.
struct Collector final : wings::PackageSink
{
    std::vector<wings::Package> packages;
    void package(wings::Package &&p) override
    {
        p.downloadSource.clear();
        p.downloadSource.shrink_to_fit();
        p.description = plain_description(p.description, kDescriptionBytes);
        p.description.shrink_to_fit();
        p.downloadLinks.shrink_to_fit();
        packages.push_back(std::move(p));
    }
};

} // namespace

std::string plain_description(std::string_view html, std::size_t max_bytes)
{
    std::string out;
    out.reserve(std::min(html.size(), max_bytes + 8));
    bool in_tag = false;
    bool space = true; // a pending separator; true at the start swallows leading blanks
    const auto put_space = [&] { space = true; };
    const auto put = [&](char c)
    {
        if (space && !out.empty())
            out.push_back(' ');
        space = false;
        out.push_back(c);
    };
    for (std::size_t i = 0; i < html.size() && out.size() < max_bytes + 4; ++i)
    {
        const char c = html[i];
        if (in_tag)
        {
            if (c == '>')
                in_tag = false;
            continue;
        }
        if (c == '<')
        {
            in_tag = true;
            put_space(); // <br>, <p> and friends separate words
            continue;
        }
        if (c == '&')
        {
            const std::size_t end = html.find(';', i);
            if (end != std::string_view::npos && end - i <= 7)
            {
                const std::string_view entity = html.substr(i + 1, end - i - 1);
                const char *text = nullptr;
                if (entity == "amp")
                    text = "&";
                else if (entity == "lt")
                    text = "<";
                else if (entity == "gt")
                    text = ">";
                else if (entity == "quot")
                    text = "\"";
                else if (entity == "apos" || entity == "#39")
                    text = "'";
                else if (entity == "nbsp")
                    text = " ";
                if (text != nullptr)
                {
                    if (text[0] == ' ')
                        put_space();
                    else
                        put(text[0]);
                    i = end;
                    continue;
                }
            }
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            put_space();
        else
            put(c);
    }
    if (out.size() > max_bytes)
    {
        std::size_t cut = max_bytes;
        // Back up to a UTF-8 character boundary, then to a word end if one is close.
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        const std::size_t word = out.rfind(' ', cut);
        if (word != std::string::npos && cut - word < 40)
            cut = word;
        out.resize(cut);
        out += "\xE2\x80\xA6";
    }
    return out;
}

// ---------------------------------------------------------------- plumbing --

PegasusBackend::PegasusBackend(std::shared_ptr<wings::http::Transport> transport,
                               std::string endpoint)
    : transport_(std::move(transport)), endpoint_(std::move(endpoint))
{
}

void PegasusBackend::disconnect()
{
    Lock lock(mutex_);
    api_.reset();
}

template <typename T> void PegasusBackend::fail(Result<T> *out, const wings::ApiError &error)
{
    switch (error.kind)
    {
    case wings::ApiError::Kind::transport:
        out->status = 0;
        disconnect(); // reconnect (and probe again) on the next call
        break;
    case wings::ApiError::Kind::http:
        out->status = error.status;
        break;
    case wings::ApiError::Kind::parse:
        out->status = error.status != 0 ? error.status : 502;
        break;
    default:
        out->status = 400; // rejected locally: Pegasus is fine, the input was not
        break;
    }
    out->error = error.message.empty() ? "Pegasus DL reported an error" : error.message;
}

PegasusBackend::Api PegasusBackend::api(Result<bool> *error)
{
    {
        Lock lock(mutex_);
        if (api_)
            return api_;
    }
    wings::ConnectOptions options;
    options.probeAppDb = false; // /appdb is slow on a full console; read on demand
    wings::Connection connection = wings::connect(transport_, options);

    PegasusInfo info;
    info.version = connection.health.version;
    info.port = connection.health.port != 0 ? connection.health.port : 6970;
    info.download_only = connection.health.downloadOnly;
    info.native_pkg_handoff = connection.health.nativePkgHandoff;
    info.adapter = connection.adapter;
    info.notes = connection.caps.notes;
    {
        const wings::Capabilities &c = connection.caps;
        info.features = {{"Settings", c.settings},
                         {"Sources", c.sources},
                         {"Suggested sources", c.defaultSources},
                         {"Catalogue", c.library},
                         {"Queue", c.queue},
                         {"Debrid", c.debrid},
                         {"Provider resolve", c.providerResolve},
                         {"Browser capture", c.browserCapture},
                         {"Folder browser", c.browse && c.mounts},
                         {"Logs", c.logs},
                         {"Installed games", c.appdb},
                         {"APR Emu", c.aprEmu},
                         {"APR Emu versions", c.aprEmuVersions},
                         {"Delete installed game", c.installedGameDelete},
                         {"File manager", c.fileManager},
                         {"File operations", c.fileOperations},
                         {"DNS helper", c.dnsHelper}};
    }
    switch (connection.compat)
    {
    case wings::Compat::tested:
        info.compat = "tested";
        break;
    case wings::Compat::untestedMinor:
        info.compat = "untested version";
        break;
    case wings::Compat::unsupported:
        info.compat = "unsupported";
        info.supported = false;
        break;
    case wings::Compat::unreachable:
        break;
    }
    if (connection.compat == wings::Compat::unreachable)
    {
        error->status = 0;
        error->error = connection.error.message.empty() ? "no answer" : connection.error.message;
        return nullptr;
    }
    if (!connection.usable())
    {
        {
            Lock lock(mutex_);
            info_ = info;
        }
        error->status = 501;
        error->error = "Pegasus DL " + info.version + " is not supported by this version of Wings";
        return nullptr;
    }
    Lock lock(mutex_);
    if (!api_)
    {
        api_ = Api(std::move(connection.api));
        info_ = info;
    }
    return api_;
}

template <typename T> PegasusBackend::Api PegasusBackend::need_api(Result<T> *out)
{
    Result<bool> error;
    Api a = api(&error);
    if (!a)
    {
        out->status = error.status;
        out->error = error.error;
    }
    return a;
}

wings::Settings PegasusBackend::current_settings() const
{
    Lock lock(const_cast<Mutex &>(mutex_));
    return settings_;
}

std::shared_ptr<const PegasusBackend::Catalogue> PegasusBackend::catalogue() const
{
    Lock lock(const_cast<Mutex &>(mutex_));
    return catalogue_;
}

void PegasusBackend::remember_session(const std::string &id, const SessionInfo &info)
{
    Lock lock(mutex_);
    if (sessions_.size() > 8)
        sessions_.erase(sessions_.begin());
    sessions_[id] = info;
}

PegasusBackend::SessionInfo PegasusBackend::session_info(const std::string &id) const
{
    Lock lock(const_cast<Mutex &>(mutex_));
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? SessionInfo{} : it->second;
}

PegasusBackend::CacheStats PegasusBackend::cache_stats() const
{
    CacheStats stats;
    const std::shared_ptr<const Catalogue> cat = catalogue();
    if (!cat)
        return stats;
    stats.packages = cat->packages.size();
    stats.bytes = cat->packages.capacity() * sizeof(wings::Package) +
                  cat->index.size() * (sizeof(std::int64_t) + sizeof(std::size_t) + 32);
    for (const wings::Package &p : cat->packages)
    {
        const auto heap = [](const std::string &s) { return s.capacity() > 15 ? s.capacity() + 16 : 0; };
        stats.bytes += heap(p.sourceName) + heap(p.titleId) + heap(p.title) + heap(p.version) +
                       heap(p.posterUrl) + heap(p.description) +
                       p.downloadLinks.capacity() * sizeof(wings::DownloadLink);
        stats.links += p.downloadLinks.size();
        for (const wings::DownloadLink &l : p.downloadLinks)
            stats.bytes += heap(l.name) + heap(l.url);
    }
    return stats;
}

// ------------------------------------------------------------------- reads --

Result<PegasusInfo> PegasusBackend::health()
{
    Result<PegasusInfo> out;
    Result<bool> error;
    const Api a = api(&error);
    if (!a)
    {
        if (error.status == 501)
        {
            // A Pegasus we cannot drive: it is there, so say so (and why we won't use it).
            Lock lock(mutex_);
            out.status = 200;
            out.value = info_;
            return out;
        }
        out.status = error.status;
        out.error = error.error;
        return out;
    }
    const wings::Result<wings::Health> health = a->health();
    if (!health.ok())
    {
        fail(&out, health.error);
        return out;
    }
    Lock lock(mutex_);
    if (!health.value.version.empty() && health.value.version != info_.version)
        api_.reset(); // Pegasus was replaced under us: connect and probe again next time
    info_.version = health.value.version;
    info_.download_only = health.value.downloadOnly;
    info_.native_pkg_handoff = health.value.nativePkgHandoff;
    out.status = 200;
    out.value = info_;
    return out;
}

Result<std::vector<Job>> PegasusBackend::queue()
{
    Result<std::vector<Job>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const wings::Result<wings::QueueSnapshot> reply = a->getQueue();
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = to_jobs(reply.value);
    return out;
}

Result<std::vector<Source>> PegasusBackend::sources()
{
    Result<std::vector<Source>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->listSources();
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = to_sources(reply.value);
    return out;
}

Result<SettingsView> PegasusBackend::settings()
{
    Result<SettingsView> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->getSettings();
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    {
        Lock lock(mutex_);
        settings_ = reply.value;
    }
    out.status = 200;
    out.value = to_settings_view(reply.value);
    return out;
}

Result<std::vector<InstalledGame>> PegasusBackend::installed()
{
    Result<std::vector<InstalledGame>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->getInstalledGames();
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    if (!reply.value.databaseAvailable)
    {
        out.status = 503;
        out.error = reply.value.databaseError.empty() ? "The game database is not available"
                                                      : reply.value.databaseError;
        return out;
    }
    out.status = 200;
    for (const wings::InstalledGame &g : reply.value.titles)
    {
        InstalledGame game;
        game.title_id = g.titleId;
        game.title = !g.titleName.empty() ? g.titleName : (!g.title.empty() ? g.title : g.titleId);
        game.version = wings::logic::installedVersionText(g);
        game.path = wings::logic::installedLocationText(g);
        game.size_bytes = static_cast<double>(g.sizeBytes);
        game.apr_emu = g.aprEmu.present;
        game.storage = wings::logic::installedFormatText(g);
        game.firmware = wings::logic::installedFirmwareText(g.requiredSystemSoftwareVersion);
        game.apr = to_apr(g.aprEmu);
        out.value.push_back(std::move(game));
    }
    return out;
}

Result<std::vector<LogLine>> PegasusBackend::logs(std::int64_t since)
{
    Result<std::vector<LogLine>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->getLogs(since);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    for (const wings::LogEntry &e : reply.value.logs)
        out.value.push_back({e.seq, e.level, e.component, e.message});
    return out;
}

Result<std::vector<Package>> PegasusBackend::library()
{
    Result<std::vector<Package>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    Collector collector;
    const auto reply = a->streamLibrary(collector);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    auto cat = std::make_shared<Catalogue>();
    cat->packages = std::move(collector.packages);
    cat->packages.shrink_to_fit();
    cat->index.reserve(cat->packages.size());
    out.value.reserve(cat->packages.size());
    for (std::size_t i = 0; i < cat->packages.size(); ++i)
    {
        cat->index.emplace(cat->packages[i].id, i);
        out.value.push_back(to_package(cat->packages[i]));
    }
    {
        Lock lock(mutex_);
        catalogue_ = std::move(cat);
    }
    out.status = 200;
    return out;
}

Result<PackageDetail> PegasusBackend::package(std::string_view id)
{
    Result<PackageDetail> out;
    const std::shared_ptr<const Catalogue> cat = catalogue();
    if (!cat)
    {
        out.status = 404;
        out.error = "The catalogue has not loaded yet";
        return out;
    }
    const auto it = cat->index.find(to_id(id));
    if (it == cat->index.end())
    {
        out.status = 404;
        out.error = "That package is not in the catalogue any more";
        return out;
    }
    const wings::Package &pkg = cat->packages[it->second];
    const wings::Settings settings = current_settings();
    out.status = 200;
    out.value.package = to_package(pkg);
    for (const wings::DownloadLink &link : wings::logic::packageLinks(pkg))
    {
        const wings::logic::LinkAction action =
            wings::logic::decideLinkAction(pkg, link.index, settings);
        PackageLink entry;
        entry.index = static_cast<int>(link.index);
        entry.name = link.name;
        entry.host = wings::logic::hostFromDownloadUrl(link.url);
        entry.url = link.url;
        entry.route = to_route(action.kind);
        if (action.support.hasProvider)
            entry.provider = action.support.provider.name;
        entry.needs_browser = entry.route == LinkRoute::capture ||
                              entry.route == LinkRoute::browser_only ||
                              (entry.route == LinkRoute::automated && action.requiresBrowser);
        for (const wings::DebridService service : action.services)
            entry.services.push_back(wings::logic::debridServiceInfo(service).label);
        out.value.links.push_back(std::move(entry));
    }
    return out;
}

Result<std::vector<std::uint8_t>> PegasusBackend::package_tiers()
{
    // The same decision decideLinkAction makes for each link (and package()
    // reports as a route), without building a LinkAction per link: the host
    // rules (debrid domains, provider tables) depend on the host only, so they
    // are answered once per distinct host.
    Result<std::vector<std::uint8_t>> out;
    out.status = 200;
    const std::shared_ptr<const Catalogue> cat = catalogue();
    if (!cat)
        return out;
    const wings::Settings settings = current_settings();
    struct HostClass
    {
        LinkTier tier = LinkTier::unsupported;
        bool debrid = false;
    };
    std::unordered_map<std::string, HostClass> hosts;
    out.value.reserve(cat->packages.size());
    for (const wings::Package &pkg : cat->packages)
    {
        LinkTier best = LinkTier::unsupported;
        bool via_debrid = false;
        for (const wings::DownloadLink &link : pkg.downloadLinks)
        {
            if (link.url.empty())
                continue; // packageLinks() skips it
            const std::string url = wings::logic::downloadLinkUrl(link.url);
            LinkTier tier = LinkTier::unsupported; // linkNotFound
            if (url.empty())
            {
                // stays unsupported
            }
            else if (wings::logic::urlHasPkgExtension(url))
            {
                tier = LinkTier::native_pkg;
            }
            else
            {
                const std::string host = wings::logic::hostFromDownloadUrl(url);
                auto it = hosts.find(host);
                if (it == hosts.end())
                {
                    HostClass c;
                    if (!wings::logic::debridServicesForUrl(url, settings).empty())
                    {
                        c.tier = LinkTier::debrid;
                        c.debrid = true;
                    }
                    else
                    {
                        const wings::logic::DownloadSupport support =
                            wings::logic::downloadSupportForUrl(url);
                        if (support.browserOnly || support.blocked)
                            c.tier = LinkTier::unsupported;
                        else if (support.automated)
                            c.tier = LinkTier::automated;
                        else
                            c.tier = support.hasProvider ? LinkTier::supported : LinkTier::unknown;
                    }
                    it = hosts.emplace(host, c).first;
                }
                tier = it->second.tier;
                via_debrid = via_debrid || it->second.debrid;
            }
            best = std::min(best, tier);
        }
        out.value.push_back(pack_tier(best, via_debrid));
    }
    return out;
}

// ---------------------------------------------------------------- the queue --

Result<std::vector<Job>> PegasusBackend::jobs_after(
    const wings::Result<wings::QueueResponse> &reply, wings::IPegasus &api)
{
    Result<std::vector<Job>> out;
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    if (reply.value.hasQueue)
    {
        out.value = to_jobs(reply.value.queue);
        return out;
    }
    const wings::Result<wings::QueueSnapshot> queue = api.getQueue();
    if (!queue.ok())
    {
        fail(&out, queue.error);
        return out;
    }
    out.value = to_jobs(queue.value);
    return out;
}

Result<std::vector<Job>> PegasusBackend::job_action(std::string_view job_id, std::string_view action)
{
    Result<std::vector<Job>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const std::int64_t id = to_id(job_id);
    if (id <= 0)
    {
        out.status = 400;
        out.error = "Unknown download";
        return out;
    }
    wings::Result<wings::QueueResponse> reply;
    if (action == "pause")
        reply = a->pauseJob(id);
    else if (action == "resume")
        reply = a->resumeJob(id);
    else if (action == "cancel")
        reply = a->cancelJob(id);
    else if (action == "retry")
        reply = a->retryJob(id);
    else
    {
        out.status = 400;
        out.error = "Unknown action";
        return out;
    }
    return jobs_after(reply, *a);
}

Result<std::vector<Job>> PegasusBackend::clear_finished()
{
    Result<std::vector<Job>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    return jobs_after(a->clearFinished(), *a);
}

Result<std::vector<Job>> PegasusBackend::archive_password(std::string_view job_id,
                                                          std::string_view password)
{
    Result<std::vector<Job>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    return jobs_after(a->submitArchivePassword(to_id(job_id), std::string(password)), *a);
}

Result<std::vector<Job>> PegasusBackend::archive_choice(std::string_view job_id, int index)
{
    Result<std::vector<Job>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    return jobs_after(a->submitArchiveChoice(to_id(job_id), index), *a);
}

// -------------------------------------------------------------- link flows --

Result<LinkResult> PegasusBackend::start_link(std::string_view package_id, int link_index,
                                              std::string_view service)
{
    Result<LinkResult> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const std::shared_ptr<const Catalogue> cat = catalogue();
    const std::int64_t pid = to_id(package_id);
    const auto it = cat ? cat->index.find(pid) : decltype(cat->index.end()){};
    if (!cat || it == cat->index.end())
    {
        out.status = 404;
        out.error = "That package is not in the catalogue any more";
        return out;
    }
    const wings::Package &pkg = cat->packages[it->second];
    const wings::logic::LinkAction action =
        wings::logic::decideLinkAction(pkg, link_index, current_settings());
    using Kind = wings::logic::LinkActionKind;

    out.status = 200;
    switch (action.kind)
    {
    case Kind::linkNotFound:
        out.status = 404;
        out.error = "Download link not found.";
        return out;
    case Kind::nativePkg:
    {
        const auto reply = a->enqueuePackage(pid, action.linkIndex);
        if (!reply.ok())
        {
            fail(&out, reply.error);
            return out;
        }
        out.value.kind = LinkResult::Kind::dispatched;
        out.value.dispatch = to_dispatch(reply.value, "Download queued.");
        return out;
    }
    case Kind::debrid:
    case Kind::chooseDebrid:
    {
        wings::DebridService chosen = action.services.empty() ? wings::DebridService::realDebrid
                                                              : action.services.front();
        if (!service.empty())
        {
            bool found = false;
            for (const wings::logic::DebridServiceInfo &info : wings::logic::debridServices())
            {
                if (service == info.id &&
                    std::find(action.services.begin(), action.services.end(), info.service) !=
                        action.services.end())
                {
                    chosen = info.service;
                    found = true;
                }
            }
            if (!found)
            {
                out.status = 400;
                out.error = "That debrid service does not take this link.";
                return out;
            }
        }
        else if (action.kind == Kind::chooseDebrid)
        {
            out.status = 400;
            out.error = "Choose a debrid service first.";
            return out;
        }
        const auto reply = a->debridQueue(pid, action.linkIndex, chosen);
        if (!reply.ok())
        {
            fail(&out, reply.error);
            return out;
        }
        const wings::logic::DebridServiceInfo &info = wings::logic::debridServiceInfo(chosen);
        if (reply.value.needsSelection)
        {
            out.value.kind = LinkResult::Kind::select_files;
            out.value.selection_id = reply.value.selectionId;
            out.value.service = info.id;
            for (const wings::DebridOption &o : reply.value.options)
                out.value.files.push_back(
                    {static_cast<int>(o.index), o.filename, static_cast<double>(o.filesize)});
            return out;
        }
        out.value.kind = LinkResult::Kind::dispatched;
        out.value.dispatch =
            to_dispatch(reply.value.response, std::string(info.label) + " download queued.");
        return out;
    }
    case Kind::browserOnly:
    {
        const auto reply = a->browserOpen(action.url, false, std::nullopt, std::nullopt);
        if (!reply.ok())
        {
            fail(&out, reply.error);
            return out;
        }
        out.value.kind = LinkResult::Kind::browser_opened;
        out.value.message = "Link opened in the PS5 browser.";
        return out;
    }
    case Kind::blocked:
        out.value.kind = LinkResult::Kind::blocked;
        out.value.message =
            (action.support.hasProvider ? action.support.provider.name : std::string("Provider")) +
            " is not supported.";
        return out;
    case Kind::automatedCapture:
    case Kind::manualCapture:
    {
        const bool automated = action.kind == Kind::automatedCapture;
        const auto reply = automated
                               ? a->providerResolve(pid, action.linkIndex)
                               : a->browserOpen(action.url, action.guarded, pid, action.linkIndex);
        if (!reply.ok())
        {
            fail(&out, reply.error);
            return out;
        }
        SessionInfo info;
        info.automated = automated;
        info.requires_browser = action.requiresBrowser;
        info.title = pkg.title;
        info.host = wings::logic::hostFromDownloadUrl(action.url);
        const std::string session = std::to_string(reply.value.sessionId);
        remember_session(session, info);
        out.value.kind = LinkResult::Kind::capture;
        CaptureView &view = out.value.capture;
        view.active = true;
        view.session_id = session;
        view.automated = automated;
        view.requires_browser = action.requiresBrowser;
        view.title = info.title;
        view.host = info.host;
        view.state = (automated && !action.requiresBrowser) ? "queueing" : "watching";
        view.message = wings::logic::captureStatusText(
            automated && !action.requiresBrowser ? wings::CaptureState::queueing
                                                 : wings::CaptureState::watching,
            automated, action.requiresBrowser);
        return out;
    }
    }
    return out;
}

Result<LinkResult> PegasusBackend::debrid_select(std::string_view selection_id,
                                                 const std::vector<int> &indexes,
                                                 std::string_view service)
{
    Result<LinkResult> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const wings::logic::DebridServiceInfo *chosen = nullptr;
    for (const wings::logic::DebridServiceInfo &info : wings::logic::debridServices())
    {
        if (service == info.id)
            chosen = &info;
    }
    if (chosen == nullptr || indexes.empty())
    {
        out.status = 400;
        out.error = indexes.empty() ? "Pick at least one file." : "Unknown debrid service.";
        return out;
    }
    std::vector<std::int64_t> wide(indexes.begin(), indexes.end());
    const auto reply = a->debridQueueSelection(std::string(selection_id), wide, chosen->service);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value.kind = LinkResult::Kind::dispatched;
    out.value.dispatch = to_dispatch(reply.value.response, std::string(chosen->label) + " download queued.");
    return out;
}

CaptureView PegasusBackend::capture_from(const wings::CaptureStatus &status,
                                         const SessionInfo &info) const
{
    CaptureView view;
    view.session_id = std::to_string(status.sessionId);
    view.state = capture_name(status.state);
    view.automated = info.automated;
    view.requires_browser = info.requires_browser;
    view.title = info.title;
    view.host = info.host;
    view.message = !status.message.empty()
                       ? status.message
                       : wings::logic::captureStatusText(status.state, info.automated,
                                                         info.requires_browser);
    // Failed and cancelled sessions stay "active" so the panel can say so; the
    // player dismisses them. Only a completed one is done with.
    view.active = status.state != wings::CaptureState::completed;
    if (status.state == wings::CaptureState::completed)
    {
        view.result = to_dispatch(status.dispatch, wings::logic::captureStatusText(
                                                       status.state, info.automated,
                                                       info.requires_browser));
        view.active = false;
    }
    return view;
}

Result<CaptureView> PegasusBackend::capture_status(std::string_view session_id)
{
    Result<CaptureView> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->captureStatus(to_id(session_id));
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = capture_from(reply.value, session_info(std::string(session_id)));
    if (out.value.session_id == "0")
        out.value.session_id = std::string(session_id);
    return out;
}

Result<CaptureView> PegasusBackend::capture_active()
{
    Result<CaptureView> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->captureActive();
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    if (!reply.value.active || reply.value.sessionId == 0)
        return out; // nothing running: an inactive view
    // The session was started before this run: work out its kind from the URL.
    SessionInfo info = session_info(std::to_string(reply.value.sessionId));
    if (info.host.empty())
    {
        const wings::logic::DownloadSupport support =
            wings::logic::downloadSupportForUrl(reply.value.startUrl);
        info.automated = support.automated;
        info.requires_browser = support.automated ? support.provider.requiresBrowser : true;
        info.host = wings::logic::hostFromDownloadUrl(reply.value.startUrl);
    }
    out.value = capture_from(reply.value, info);
    return out;
}

Result<bool> PegasusBackend::capture_cancel(std::string_view session_id)
{
    Result<bool> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->captureCancel(to_id(session_id));
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = true;
    return out;
}

Result<Probe> PegasusBackend::probe_link(std::string_view url)
{
    Result<Probe> out;
    const std::string safe = wings::logic::safeHttpUrl(std::string(url));
    if (safe.empty())
    {
        out.status = 400;
        out.error = "Enter an http:// or https:// link.";
        return out;
    }
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->resolveProbe(safe);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value.url = safe;
    out.value.final_url = reply.value.finalUrl;
    out.value.file_name = reply.value.fileName;
    out.value.content_type = reply.value.contentType;
    out.value.length = reply.value.hasContentLength ? static_cast<double>(reply.value.contentLength) : -1.0;
    out.value.rejection = wings::logic::probeRejection(reply.value);
    return out;
}

Result<Dispatch> PegasusBackend::queue_direct(const Probe &probe, std::string_view file_name)
{
    Result<Dispatch> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const std::string url = probe.final_url.empty() ? probe.url : probe.final_url;
    const auto reply = a->queueDirect(url, file_name.empty() ? probe.file_name : std::string(file_name),
                                      probe.content_type);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = to_dispatch(reply.value, "Direct link queued.");
    return out;
}

// ----------------------------------------------------------------- sources --

Result<std::vector<Source>> PegasusBackend::source_add_url(std::string_view url)
{
    Result<std::vector<Source>> out;
    const std::string safe = wings::logic::safeAbsoluteHttpUrl(std::string(url));
    if (safe.empty())
    {
        out.status = 400;
        out.error = "Enter an http:// or https:// address.";
        return out;
    }
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->addSourceUrl(safe);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = to_sources(reply.value);
    return out;
}

Result<std::vector<Source>> PegasusBackend::source_delete(std::string_view source_id)
{
    Result<std::vector<Source>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->deleteSource(to_id(source_id));
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = to_sources(reply.value);
    return out;
}

Result<std::vector<Source>> PegasusBackend::source_refresh(std::string_view source_id)
{
    Result<std::vector<Source>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->refreshSource(to_id(source_id));
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = to_sources(reply.value);
    return out;
}

Result<std::vector<Source>> PegasusBackend::source_enable(std::string_view source_id, bool enabled)
{
    Result<std::vector<Source>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->setSourceEnabled(to_id(source_id), enabled);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = to_sources(reply.value);
    return out;
}

Result<DefaultSources> PegasusBackend::default_sources(bool force_refresh)
{
    Result<DefaultSources> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->getDefaultSources(force_refresh);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    const wings::DefaultSources &d = reply.value;
    out.status = 200;
    out.value.available = d.available;
    out.value.status = d.status;
    out.value.error = d.error;
    out.value.revision = d.revision;
    out.value.onboarding_required = d.onboardingRequired;
    out.value.has_new = d.hasNew;
    for (const wings::DefaultSource &s : d.sources)
    {
        DefaultSource source;
        source.id = s.id;
        source.name = s.name;
        source.description = s.description;
        source.default_enabled = s.defaultEnabled;
        source.configured = s.configured;
        source.enabled = s.enabled;
        source.is_new = s.isNew;
        source.package_count = static_cast<int>(s.packageCount);
        out.value.sources.push_back(std::move(source));
    }
    return out;
}

Result<std::vector<std::string>> PegasusBackend::default_sources_apply(
    std::int64_t revision, const std::vector<std::string> &ids)
{
    Result<std::vector<std::string>> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->applyDefaultSources(revision, ids);
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    for (const std::string &failure : reply.value.failures)
        out.value.push_back(failure.empty() ? std::string("A source could not be added") : failure);
    return out;
}

Result<bool> PegasusBackend::default_sources_seen()
{
    Result<bool> out;
    const Api a = need_api(&out);
    if (!a)
        return out;
    const auto reply = a->markDefaultSourcesSeen();
    if (!reply.ok())
    {
        fail(&out, reply.error);
        return out;
    }
    out.status = 200;
    out.value = true;
    return out;
}

// --------------------------------------------------------------------- art --

Result<Art> PegasusBackend::art(std::string_view key, ArtKind kind)
{
    Result<Art> out;
    // Installed games have art on the console itself (plain HTTP on loopback).
    // Store posters are https CDN links; the art module owns those ("pkg:" keys).
    if (key.substr(0, 4) != "app:" || key.size() <= 4)
    {
        out.status = 404;
        out.error = "no artwork";
        return out;
    }
    const Api a = need_api(&out);
    if (!a)
        return out;
    wings::InstalledGame game;
    game.titleId = std::string(key.substr(4));
    const auto reply =
        a->fetchAppdbArt(wings::logic::installedArtUrl(game, kind != ArtKind::cover ? "pic0" : "icon"));
    if (!reply.ok())
    {
        fail(&out, reply.error);
        if (out.status == 0 && reply.error.kind != wings::ApiError::Kind::transport)
            out.status = 400;
        return out;
    }
    out.status = 200;
    out.value.bytes = reply.value;
    return out;
}


// ----------------------------------------------- settings, storage, logs --

#define WB_API(T)                                                                                  \
    Result<T> out;                                                                                 \
    const Api a = need_api(&out);                                                                  \
    if (!a)                                                                                        \
        return out;
#define WB_CHECK(reply)                                                                            \
    if (!(reply).ok())                                                                             \
    {                                                                                              \
        fail(&out, (reply).error);                                                                 \
        return out;                                                                                \
    }

namespace
{

std::vector<std::string> unique_paths(const std::vector<std::string> &paths)
{
    return wings::logic::uniquePaths(paths);
}

Result<std::vector<FileOp>> ops_of(const std::vector<wings::FileOperation> &list)
{
    Result<std::vector<FileOp>> out;
    out.status = 200;
    for (const wings::FileOperation &o : list)
        out.value.push_back(to_op(o));
    return out;
}

FolderListing listing_of(const std::string &asked, const std::string &path, bool writable,
                         const std::vector<wings::FileEntry> &entries)
{
    FolderListing out;
    out.path = path.empty() ? asked : path;
    out.writable = writable;
    for (const wings::FileEntry &e : entries)
    {
        FileItem item = to_item(e);
        if (item.path.empty())
            item.path = wings::logic::pathJoin(out.path, item.name);
        out.entries.push_back(std::move(item));
    }
    return out;
}

} // namespace

Result<SettingsView> PegasusBackend::settings_update(const SettingsPatch &patch)
{
    WB_API(SettingsView)
    wings::SettingsUpdate update;
    update.hasDownloadRoot = patch.has_root;
    update.downloadRoot = patch.root;
    update.hasDownloadConnections = patch.has_connections;
    update.downloadConnections = patch.connections;
    update.hasRealDebridToken = patch.has_real_debrid;
    update.realDebridToken = patch.real_debrid_token;
    update.hasTorBoxToken = patch.has_torbox;
    update.torBoxToken = patch.torbox_token;
    const auto reply = a->updateSettings(update);
    WB_CHECK(reply)
    {
        Lock lock(mutex_);
        settings_ = reply.value;
    }
    out.status = 200;
    out.value = to_settings_view(reply.value);
    return out;
}

Result<SettingsView> PegasusBackend::debrid_refresh(std::string_view service)
{
    WB_API(SettingsView)
    const wings::DebridService which = wings::logic::debridServiceById(service).service;
    const auto refreshed = a->refreshDebrid(which);
    WB_CHECK(refreshed)
    const auto reply = a->getSettings();
    WB_CHECK(reply)
    {
        Lock lock(mutex_);
        settings_ = reply.value;
    }
    out.status = 200;
    out.value = to_settings_view(reply.value);
    return out;
}

Result<std::vector<StorageMount>> PegasusBackend::mounts()
{
    WB_API(std::vector<StorageMount>)
    const auto reply = a->getMounts();
    WB_CHECK(reply)
    out.status = 200;
    for (const wings::Mount &m : reply.value)
        out.value.push_back(to_mount(m));
    return out;
}

Result<FolderListing> PegasusBackend::browse(std::string_view path)
{
    WB_API(FolderListing)
    const auto reply = a->browse(std::string(path));
    WB_CHECK(reply)
    out.status = 200;
    out.value = listing_of(std::string(path), reply.value.path, true, reply.value.entries);
    return out;
}

Result<bool> PegasusBackend::mkdir(std::string_view full_path)
{
    WB_API(bool)
    const auto reply = a->mkdir(std::string(full_path));
    WB_CHECK(reply)
    out.status = 200;
    out.value = true;
    return out;
}

Result<bool> PegasusBackend::logs_clear()
{
    WB_API(bool)
    const auto reply = a->clearLogs();
    WB_CHECK(reply)
    out.status = 200;
    out.value = true;
    return out;
}

// ------------------------------------------------------ APR Emu and delete --

Result<std::vector<AprVersionItem>> PegasusBackend::apr_versions()
{
    WB_API(std::vector<AprVersionItem>)
    const auto reply = a->getAprVersions();
    WB_CHECK(reply)
    out.status = 200;
    for (const wings::AprVersion &v :
         wings::logic::sortedAprVersions(reply.value.versions, reply.value.latestReleaseId))
        out.value.push_back({v.id, wings::logic::aprVersionLabel(v)});
    return out;
}

Result<AdminJob> PegasusBackend::apr_update_start(std::string_view title_id,
                                                  std::string_view version_id)
{
    WB_API(AdminJob)
    const auto reply = a->aprUpdateStart(std::string(title_id), std::string(version_id));
    WB_CHECK(reply)
    out.status = 200;
    out.value = to_admin(reply.value);
    if (out.value.title_id.empty())
        out.value.title_id = std::string(title_id);
    return out;
}

Result<AdminJob> PegasusBackend::apr_update_status(std::string_view job_id,
                                                   std::string_view title_id)
{
    WB_API(AdminJob)
    wings::JobRef ref;
    ref.jobId = std::string(job_id);
    ref.titleId = std::string(title_id);
    const auto reply = a->aprUpdateStatus(ref);
    WB_CHECK(reply)
    out.status = 200;
    out.value = to_admin(reply.value);
    if (out.value.title_id.empty())
        out.value.title_id = std::string(title_id);
    return out;
}

Result<AdminJob> PegasusBackend::game_delete(std::string_view title_id)
{
    WB_API(AdminJob)
    const auto reply = a->installedGameDelete(std::string(title_id));
    WB_CHECK(reply)
    out.status = 200;
    out.value = to_admin(reply.value);
    if (out.value.title_id.empty())
        out.value.title_id = std::string(title_id);
    return out;
}

Result<AdminJob> PegasusBackend::game_delete_status(std::string_view job_id,
                                                    std::string_view title_id)
{
    WB_API(AdminJob)
    wings::JobRef ref;
    ref.jobId = std::string(job_id);
    ref.titleId = std::string(title_id);
    const auto reply = a->installedGameDeleteStatus(ref);
    WB_CHECK(reply)
    out.status = 200;
    out.value = to_admin(reply.value);
    if (out.value.title_id.empty())
        out.value.title_id = std::string(title_id);
    return out;
}

// ------------------------------------------------------------ file manager --

Result<std::vector<StorageMount>> PegasusBackend::file_locations()
{
    WB_API(std::vector<StorageMount>)
    const auto reply = a->fileLocations();
    WB_CHECK(reply)
    out.status = 200;
    for (const wings::Location &m : reply.value)
        out.value.push_back(to_mount(m));
    return out;
}

Result<FolderListing> PegasusBackend::file_list(std::string_view path)
{
    WB_API(FolderListing)
    const auto reply = a->fileList(std::string(path));
    WB_CHECK(reply)
    out.status = 200;
    out.value =
        listing_of(std::string(path), reply.value.path, reply.value.writable, reply.value.entries);
    return out;
}

Result<std::vector<FileOp>> PegasusBackend::file_operations()
{
    WB_API(std::vector<FileOp>)
    const auto reply = a->fileOperations();
    WB_CHECK(reply)
    return ops_of(reply.value);
}

Result<FileItem> PegasusBackend::file_mkdir(std::string_view parent, std::string_view name)
{
    WB_API(FileItem)
    const std::string clean = wings::logic::validateName(name);
    if (clean.empty())
    {
        out.status = 400;
        out.error = "Enter a folder name without slashes.";
        return out;
    }
    const auto reply = a->fileMkdir(std::string(parent), clean);
    WB_CHECK(reply)
    out.status = 200;
    out.value = to_item(reply.value);
    return out;
}

Result<FileItem> PegasusBackend::file_rename(std::string_view path, std::string_view name)
{
    WB_API(FileItem)
    const std::string clean = wings::logic::validateName(name);
    if (clean.empty())
    {
        out.status = 400;
        out.error = "Enter a name without slashes.";
        return out;
    }
    const auto reply = a->fileRename(std::string(path), clean);
    WB_CHECK(reply)
    out.status = 200;
    out.value = to_item(reply.value);
    return out;
}

Result<std::vector<FileOp>> PegasusBackend::file_delete(const std::vector<std::string> &paths)
{
    WB_API(std::vector<FileOp>)
    const std::vector<std::string> unique = unique_paths(paths);
    if (unique.empty())
    {
        out.status = 400;
        out.error = "Nothing selected.";
        return out;
    }
    const auto reply = a->fileDelete(unique);
    WB_CHECK(reply)
    return ops_of(reply.value);
}

Result<std::vector<FileOp>> PegasusBackend::file_copy(const std::vector<std::string> &paths,
                                                      std::string_view destination,
                                                      std::string_view collision)
{
    WB_API(std::vector<FileOp>)
    const auto reply = a->fileCopy(unique_paths(paths), std::string(destination),
                                   wings::logic::collisionFromString(collision));
    WB_CHECK(reply)
    return ops_of(reply.value);
}

Result<std::vector<FileOp>> PegasusBackend::file_move(const std::vector<std::string> &paths,
                                                      std::string_view destination,
                                                      std::string_view collision)
{
    WB_API(std::vector<FileOp>)
    const auto reply = a->fileMove(unique_paths(paths), std::string(destination),
                                   wings::logic::collisionFromString(collision));
    WB_CHECK(reply)
    return ops_of(reply.value);
}

Result<std::vector<FileOp>> PegasusBackend::file_extract(const std::vector<std::string> &paths,
                                                         std::string_view destination,
                                                         std::string_view collision)
{
    WB_API(std::vector<FileOp>)
    const auto reply = a->fileExtract(unique_paths(paths), std::string(destination),
                                      wings::logic::collisionFromString(collision));
    WB_CHECK(reply)
    return ops_of(reply.value);
}

Result<std::vector<FileOp>> PegasusBackend::file_extract_password(std::string_view operation_id,
                                                                  std::string_view password)
{
    WB_API(std::vector<FileOp>)
    const auto reply = a->fileExtractPassword(to_id(operation_id), std::string(password));
    WB_CHECK(reply)
    return ops_of(reply.value);
}

Result<std::vector<FileOp>> PegasusBackend::file_pkg_install(std::string_view path)
{
    WB_API(std::vector<FileOp>)
    const auto reply = a->filePkgInstall(std::string(path));
    WB_CHECK(reply)
    return ops_of(reply.value);
}

Result<bool> PegasusBackend::file_cancel(std::string_view operation_id)
{
    WB_API(bool)
    const auto reply = a->fileOperationCancel(to_id(operation_id));
    WB_CHECK(reply)
    out.status = 200;
    out.value = true;
    return out;
}

#undef WB_API
#undef WB_CHECK

} // namespace orbit
