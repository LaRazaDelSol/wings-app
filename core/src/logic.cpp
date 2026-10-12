// Wings core - Port of Pegasus web-UI logic (see logic.hpp for the JS references).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/logic.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace wings::logic
{

namespace
{

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = lower(c);
    return out;
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_word(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool ends_with(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

// JS String.prototype.trim for the ASCII whitespace that matters here.
std::string_view js_trim(std::string_view text)
{
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
    while (!text.empty() && ws(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && ws(text.back()))
        text.remove_suffix(1);
    return text;
}

bool starts_with_http_scheme(std::string_view text) // /^https?:\/\//i
{
    auto ci = [&](std::string_view prefix) {
        if (text.size() < prefix.size())
            return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (lower(text[i]) != prefix[i])
                return false;
        return true;
    };
    return ci("http://") || ci("https://");
}

void percent_encode_into(std::string &out, unsigned char c)
{
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned>(c));
    out += buf;
}

std::string encode_set(std::string_view text, const char *extra)
{
    std::string out;
    for (const char ch : text)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        bool enc = c < 0x21 || c > 0x7E;
        for (const char *p = extra; !enc && *p; ++p)
            enc = (ch == *p);
        if (enc)
            percent_encode_into(out, c);
        else
            out.push_back(ch);
    }
    return out;
}

bool is_dot_segment(std::string_view s) // "." or %2e (case-insensitive)
{
    const std::string l = lowered(s);
    return l == "." || l == "%2e";
}

bool is_double_dot_segment(std::string_view s)
{
    const std::string l = lowered(s);
    return l == ".." || l == ".%2e" || l == "%2e." || l == "%2e%2e";
}

std::string normalize_path(std::string_view raw)
{
    std::string path(raw);
    for (char &c : path)
        if (c == '\\')
            c = '/';
    if (path.empty())
        return "/";
    path = encode_set(path, "\"<>`{}");
    // Split into segments (path begins with '/').
    std::vector<std::string> out;
    std::size_t pos = path[0] == '/' ? 1 : 0;
    bool trailing_slash_needed = false;
    while (true)
    {
        const std::size_t next = path.find('/', pos);
        const std::string seg = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        const bool last = next == std::string::npos;
        if (is_double_dot_segment(seg))
        {
            if (!out.empty())
                out.pop_back();
            if (last)
                trailing_slash_needed = true;
        }
        else if (is_dot_segment(seg))
        {
            if (last)
                trailing_slash_needed = true;
        }
        else
        {
            out.push_back(seg);
        }
        if (last)
            break;
        pos = next + 1;
    }
    std::string result;
    for (const std::string &seg : out)
    {
        result += '/';
        result += seg;
    }
    if (trailing_slash_needed)
        result += '/';
    if (result.empty())
        result = "/";
    return result;
}

} // namespace

// ------------------------------------------------------------------ URLs ----

bool parseHttpUrl(std::string_view input, UrlParts *out)
{
    std::string s;
    s.reserve(input.size());
    for (const char c : input)
        if (c != '\t' && c != '\n' && c != '\r')
            s.push_back(c);
    std::size_t b = 0, e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= 0x20)
        ++b;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= 0x20)
        --e;
    s = s.substr(b, e - b);

    const std::size_t colon = s.find(':');
    if (colon == std::string::npos)
        return false;
    UrlParts parts;
    parts.scheme = lowered(s.substr(0, colon));
    if (parts.scheme != "http" && parts.scheme != "https")
        return false;
    std::size_t at = colon + 1;
    std::size_t slashes = 0;
    while (at < s.size() && (s[at] == '/' || s[at] == '\\'))
    {
        ++at;
        ++slashes;
    }
    if (slashes < 2)
        return false;

    std::size_t end = at;
    while (end < s.size() && s[end] != '/' && s[end] != '\\' && s[end] != '?' && s[end] != '#')
        ++end;
    std::string authority = s.substr(at, end - at);
    std::string tail = s.substr(end);

    const std::size_t atsign = authority.rfind('@');
    if (atsign != std::string::npos)
    {
        parts.userinfo = authority.substr(0, atsign);
        authority = authority.substr(atsign + 1);
    }
    std::string host, port;
    if (!authority.empty() && authority[0] == '[')
    {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos)
            return false;
        host = authority.substr(0, close + 1);
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty())
        {
            if (rest[0] != ':')
                return false;
            port = rest.substr(1);
        }
    }
    else
    {
        const std::size_t pc = authority.rfind(':');
        if (pc != std::string::npos)
        {
            host = authority.substr(0, pc);
            port = authority.substr(pc + 1);
        }
        else
        {
            host = authority;
        }
        for (const char ch : host)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c <= 0x20 || c == 0x7F || std::string_view(" #%/:<>?@[\\]^|").find(ch) != std::string_view::npos)
                return false;
        }
    }
    if (host.empty())
        return false;
    host = lowered(host);
    for (const char c : port)
        if (!is_digit(c))
            return false;
    if (!port.empty())
    {
        if (port.size() > 5)
            return false;
        const int value = std::stoi(port);
        if (value > 65535)
            return false;
        if ((parts.scheme == "http" && value == 80) || (parts.scheme == "https" && value == 443))
            port.clear();
        else
            port = std::to_string(value);
    }
    parts.host = host;
    parts.port = port;

    std::string rest = tail;
    const std::size_t hash = rest.find('#');
    if (hash != std::string::npos)
    {
        parts.hasFragment = true;
        parts.fragment = encode_set(rest.substr(hash + 1), "\"<>`");
        rest = rest.substr(0, hash);
    }
    const std::size_t q = rest.find('?');
    if (q != std::string::npos)
    {
        parts.hasQuery = true;
        parts.query = encode_set(rest.substr(q + 1), "\"<>'");
        rest = rest.substr(0, q);
    }
    parts.path = normalize_path(rest);

    parts.href = parts.scheme + "://";
    if (!parts.userinfo.empty())
        parts.href += parts.userinfo + "@";
    parts.href += parts.host;
    if (!parts.port.empty())
        parts.href += ":" + parts.port;
    parts.href += parts.path;
    if (parts.hasQuery)
        parts.href += "?" + parts.query;
    if (parts.hasFragment)
        parts.href += "#" + parts.fragment;
    *out = std::move(parts);
    return true;
}

std::string safeHttpUrl(std::string_view value)
{
    UrlParts parts;
    return parseHttpUrl(value, &parts) ? parts.href : std::string();
}

std::string safeAbsoluteHttpUrl(std::string_view value)
{
    UrlParts parts;
    if (!parseHttpUrl(js_trim(value), &parts))
        return {};
    std::string href = parts.href;
    while (!href.empty() && href.back() == '/')
        href.pop_back();
    return href;
}

bool urlHasPkgExtension(std::string_view value)
{
    UrlParts parts;
    if (!parseHttpUrl(value, &parts))
        return false;
    return ends_with(lowered(parts.path), ".pkg");
}

std::string decodeBase64DownloadUrl(std::string_view value)
{
    const std::string raw(value);
    if (raw.empty() || starts_with_http_scheme(raw))
        return raw;
    // /^[A-Za-z0-9+/_-]+={0,2}$/
    std::size_t body = raw.size();
    int pad = 0;
    while (body > 0 && raw[body - 1] == '=' && pad < 2)
    {
        --body;
        ++pad;
    }
    if (body == 0)
        return raw;
    for (std::size_t i = 0; i < body; ++i)
    {
        const char c = raw[i];
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' || c == '/' ||
              c == '_' || c == '-'))
            return raw;
    }
    if (raw.size() % 4 == 1)
        return raw;

    std::string normalized = raw;
    for (char &c : normalized)
    {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }
    std::string padded = normalized + std::string((4 - normalized.size() % 4) % 4, '=');

    // atob(): forgiving-base64 decode (WHATWG infra).
    std::string data = padded;
    if (data.size() % 4 == 0)
    {
        if (!data.empty() && data.back() == '=')
            data.pop_back();
        if (!data.empty() && data.back() == '=')
            data.pop_back();
    }
    if (data.size() % 4 == 1)
        return raw;
    std::string decoded;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : data)
    {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (is_digit(c))
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else
            return raw; // atob throws => JS returns raw
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            decoded.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    if (!starts_with_http_scheme(decoded))
        return raw;
    return safeAbsoluteHttpUrl(decoded).empty() ? raw : decoded;
}

std::string downloadLinkUrl(std::string_view value)
{
    return decodeBase64DownloadUrl(value);
}

std::string hostFromUrl(std::string_view value)
{
    UrlParts parts;
    if (!parseHttpUrl(value, &parts))
        return {};
    std::string host = parts.host;
    if (starts_with(host, "www."))
        host = host.substr(4);
    return host;
}

std::string hostFromDownloadUrl(std::string_view value)
{
    const std::string url = downloadLinkUrl(value);
    return starts_with_http_scheme(url) ? hostFromUrl(url) : std::string();
}

namespace
{
std::string clean_host(std::string_view host)
{
    std::string h = lowered(host);
    if (starts_with(h, "www."))
        h = h.substr(4);
    return h;
}
} // namespace

bool providerHostMatches(std::string_view host, std::string_view providerHost)
{
    const std::string h = clean_host(host);
    const std::string p = clean_host(providerHost);
    return h == p || ends_with(h, "." + p);
}

std::vector<DownloadLink> packageLinks(const Package &pkg)
{
    std::vector<DownloadLink> out;
    for (const DownloadLink &link : pkg.downloadLinks)
    {
        if (link.url.empty())
            continue;
        DownloadLink item;
        item.hasIndex = true;
        item.index = link.hasIndex ? link.index : static_cast<std::int64_t>(out.size());
        item.name = link.name.empty() ? "Link " + std::to_string(out.size() + 1) : link.name;
        item.url = link.url;
        out.push_back(std::move(item));
    }
    return out;
}

const DownloadLink *packageLinkByIndex(const std::vector<DownloadLink> &links, std::int64_t linkIndex)
{
    for (const DownloadLink &link : links)
        if (link.index == linkIndex)
            return &link;
    return nullptr;
}

// -------------------------------------------------------------- providers ---

const std::vector<Provider> &automatedProviders()
{
    static const std::vector<Provider> table = [] {
        std::vector<Provider> t;
        t.push_back({"buzzheavier", "BuzzHeavier", {"buzzheavier.com", "bzzhr.co", "bzzhr.to"}, true, true, true});
        t.push_back({"datanodes", "DataNodes", {"datanodes.to"}, true, true, true});
        t.push_back({"mediafire", "MediaFire", {"mediafire.com"}, false, false, true}); // app.js:40
        return t;
    }();
    return table;
}

const std::vector<Provider> &directBrowserProviders()
{
    static const std::vector<Provider> table = [] {
        std::vector<Provider> t;
        t.push_back({"filek", "Filek", {"filekeeper.net"}, true, true, false});
        t.push_back({"rootz", "Rootz", {"rootz.so"}, true, true, true});
        t.push_back({"vikingfile", "VikingFile", {"vik1ngfile.site", "vikingfile.com"}, true, true, false});
        return t;
    }();
    return table;
}

const std::vector<Provider> &unsupportedProviders()
{
    static const std::vector<Provider> table; // app.js:47
    return table;
}

bool providerMatchesHost(const Provider &provider, std::string_view host)
{
    for (const std::string &providerHost : provider.hosts)
    {
        if (!provider.matchSubdomains)
        {
            if (clean_host(host) == clean_host(providerHost))
                return true;
        }
        else if (providerHostMatches(host, providerHost))
        {
            return true;
        }
    }
    return false;
}

DownloadSupport downloadSupportForUrl(std::string_view url)
{
    const std::string host = hostFromDownloadUrl(url);
    DownloadSupport s;
    for (const Provider &p : unsupportedProviders())
    {
        if (providerMatchesHost(p, host))
        {
            s.kind = SupportKind::unsupported;
            s.automated = false;
            s.blocked = false;
            s.browserOnly = true;
            s.guarded = false;
            s.state = "unsupported";
            s.label = "Not supported";
            s.hasProvider = true;
            s.provider = p;
            return s;
        }
    }
    for (const Provider &p : automatedProviders())
    {
        if (providerMatchesHost(p, host))
        {
            s.kind = SupportKind::automated;
            s.automated = true;
            s.guarded = false;
            s.state = "supported";
            s.label = "Supported";
            s.hasProvider = true;
            s.provider = p;
            return s;
        }
    }
    for (const Provider &p : directBrowserProviders())
    {
        if (providerMatchesHost(p, host))
        {
            s.kind = SupportKind::directBrowser;
            s.guarded = p.guarded; // `directProvider.guarded !== false`
            s.state = "supported";
            s.label = "Supported";
            s.hasProvider = true;
            s.provider = p;
            return s;
        }
    }
    s.kind = SupportKind::unknown;
    s.guarded = true;
    s.state = "manual";
    s.label = "Unknown";
    return s;
}

// ----------------------------------------------------------------- debrid ---

const std::vector<DebridServiceInfo> &debridServices()
{
    static const std::vector<DebridServiceInfo> table = {
        {DebridService::realDebrid, "real-debrid", "Real-Debrid", "realDebridToken"},
        {DebridService::torBox, "torbox", "TorBox", "torBoxToken"},
    };
    return table;
}

const DebridServiceInfo &debridServiceInfo(DebridService service)
{
    for (const DebridServiceInfo &info : debridServices())
        if (info.service == service)
            return info;
    return debridServices().front();
}

const DebridServiceInfo &debridServiceById(std::string_view id)
{
    for (const DebridServiceInfo &info : debridServices())
        if (id == info.id)
            return info;
    return debridServices().front();
}

const DebridStatus &debridStatusFor(const Settings &settings, DebridService service)
{
    return service == DebridService::torBox ? settings.torBox : settings.realDebrid;
}

bool debridReady(const DebridStatus &status)
{
    return status.configured && status.valid && !status.domains.empty();
}

std::vector<DebridService> debridServicesForUrl(std::string_view url, const Settings &settings)
{
    std::vector<DebridService> out;
    const std::string host = hostFromDownloadUrl(url);
    if (host.empty())
        return out;
    for (const DebridServiceInfo &info : debridServices())
    {
        const DebridStatus &status = debridStatusFor(settings, info.service);
        if (!debridReady(status))
            continue;
        for (const std::string &domain : status.domains)
        {
            if (providerHostMatches(host, domain))
            {
                out.push_back(info.service);
                break;
            }
        }
    }
    return out;
}

bool realDebridSupportsUrl(std::string_view url, const Settings &settings)
{
    for (const DebridService s : debridServicesForUrl(url, settings))
        if (s == DebridService::realDebrid)
            return true;
    return false;
}

LinkAction decideLinkAction(const Package &pkg, std::int64_t linkIndex, const Settings &settings)
{
    LinkAction action;
    action.packageId = pkg.id;
    const std::vector<DownloadLink> links = packageLinks(pkg);
    const DownloadLink *link = packageLinkByIndex(links, linkIndex);
    action.url = link ? downloadLinkUrl(link->url) : std::string();
    if (!link || action.url.empty())
    {
        action.kind = LinkActionKind::linkNotFound;
        return action;
    }
    action.linkIndex = link->index;
    if (urlHasPkgExtension(action.url)) // app.js:4863
    {
        action.kind = LinkActionKind::nativePkg;
        return action;
    }
    action.services = debridServicesForUrl(action.url, settings); // app.js:4881
    if (action.services.size() == 1)
    {
        action.kind = LinkActionKind::debrid;
        return action;
    }
    if (action.services.size() > 1)
    {
        action.kind = LinkActionKind::chooseDebrid;
        return action;
    }
    action.support = downloadSupportForUrl(action.url); // app.js:4891
    action.guarded = action.support.guarded;
    action.requiresBrowser = !action.support.hasProvider || action.support.provider.requiresBrowser;
    if (action.support.browserOnly)
        action.kind = LinkActionKind::browserOnly;
    else if (action.support.blocked)
        action.kind = LinkActionKind::blocked;
    else
        action.kind = action.support.automated ? LinkActionKind::automatedCapture : LinkActionKind::manualCapture;
    return action;
}

// ------------------------------------------------------- dispatch handling --

std::vector<DispatchResult> nativeDispatchResults(const QueueResponse &response)
{
    std::vector<DispatchResult> out;
    for (const DispatchResult &r : response.dispatchResults)
        if (r.ok && r.kind == DispatchKind::nativePkg && r.accepted)
            out.push_back(r);
    return out;
}

std::vector<DispatchResult> queuedDispatchResults(const QueueResponse &response)
{
    std::vector<DispatchResult> out;
    for (const DispatchResult &r : response.dispatchResults)
        if (r.ok && r.kind == DispatchKind::queued)
            out.push_back(r);
    return out;
}

NativePkgConfirmation nativePkgConfirmation(const std::vector<DispatchResult> &results)
{
    NativePkgConfirmation c;
    if (results.empty())
        return c;
    const DispatchResult &first = results.front();
    if (results.size() == 1)
    {
        c.title = first.title.empty() ? "Package download started" : first.title;
        c.message = first.message.empty()
                        ? "Download started and was added to PS5 Downloads. Track progress from the game tile or Downloads."
                        : first.message;
    }
    else
    {
        c.title = std::to_string(results.size()) + " package downloads started";
        c.message = "The package downloads started and were added to PS5 Downloads. Track progress from the game tiles or Downloads.";
    }
    for (const DispatchResult &r : results)
        if (!r.contentId.empty())
            c.contentIds.push_back(r.contentId);
    return c;
}

DispatchOutcome finishDispatchResponse(const QueueResponse &response, const std::string &queuedMessage)
{
    DispatchOutcome o;
    o.native = nativeDispatchResults(response);
    o.queued = queuedDispatchResults(response);
    if (response.hasQueue)
        o.queueIncluded = true;
    else if (!o.queued.empty())
        o.needsQueueReload = true;
    if (!o.queued.empty())
        o.schedulePoll = true;
    if (!o.native.empty())
    {
        o.toast = o.native.size() == 1 ? "Package download started in PS5 Downloads."
                                       : std::to_string(o.native.size()) + " package downloads started in PS5 Downloads.";
        o.showNativeConfirmation = true;
        o.confirmation = nativePkgConfirmation(o.native);
    }
    else if (!queuedMessage.empty())
    {
        o.toast = queuedMessage;
    }
    return o;
}

std::string probeRejection(const ResolveProbe &probe)
{
    if (probe.status != 0 && (probe.status < 200 || probe.status >= 300))
        return "Link returned HTTP " + std::to_string(probe.status) + ".";
    if (probe.isHtml && !probe.isDownload)
        return "Link points to a web page, not a direct download.";
    return {};
}

std::string captureStatusText(CaptureState state, bool automated, bool requiresBrowser)
{
    const bool usesBrowser = !automated || requiresBrowser;
    switch (state)
    {
    case CaptureState::watching:
        if (!usesBrowser)
            return "Resolving download link...";
        return automated ? "Browser opened. Waiting for Cloudflare verification..."
                         : "Browser opened. Waiting for the download URL...";
    case CaptureState::queueing:
        return automated ? "Link resolving is in process. Please wait." : "Download URL found. Adding it to the queue...";
    case CaptureState::completed:
        return automated ? "Supported download queued." : "Download captured and queued.";
    case CaptureState::failed:
        return automated ? "Supported download failed." : "Browser capture failed.";
    case CaptureState::cancelled:
        return automated ? "Supported download cancelled." : "Browser capture cancelled.";
    default:
        return automated ? "Starting supported download..." : "Starting browser capture...";
    }
}

bool captureIsTerminal(CaptureState state)
{
    return state == CaptureState::completed || state == CaptureState::failed || state == CaptureState::cancelled;
}

CaptureState captureStateFromString(std::string_view t)
{
    if (t == "idle")
        return CaptureState::idle;
    if (t == "priming")
        return CaptureState::priming;
    if (t == "watching")
        return CaptureState::watching;
    if (t == "queueing")
        return CaptureState::queueing;
    if (t == "completed")
        return CaptureState::completed;
    if (t == "failed")
        return CaptureState::failed;
    if (t == "cancelled")
        return CaptureState::cancelled;
    return CaptureState::unknown;
}

// ------------------------------------------------------------------ queue ---

const char *toString(JobStatus status)
{
    switch (status)
    {
    case JobStatus::queued: return "queued";
    case JobStatus::starting: return "starting";
    case JobStatus::downloading: return "downloading";
    case JobStatus::merging: return "merging";
    case JobStatus::extracting: return "extracting";
    case JobStatus::finalizing: return "finalizing";
    case JobStatus::waitingPassword: return "waiting_password";
    case JobStatus::waitingChoice: return "waiting_choice";
    case JobStatus::pausing: return "pausing";
    case JobStatus::cancelling: return "cancelling";
    case JobStatus::paused: return "paused";
    case JobStatus::completed: return "completed";
    case JobStatus::failed: return "failed";
    case JobStatus::cancelled: return "cancelled";
    default: return "unknown";
    }
}

JobStatus jobStatusFromString(std::string_view t)
{
    static const JobStatus all[] = {JobStatus::queued,      JobStatus::starting,    JobStatus::downloading,
                                    JobStatus::merging,     JobStatus::extracting,  JobStatus::finalizing,
                                    JobStatus::waitingPassword, JobStatus::waitingChoice, JobStatus::pausing,
                                    JobStatus::cancelling,  JobStatus::paused,      JobStatus::completed,
                                    JobStatus::failed,      JobStatus::cancelled};
    for (const JobStatus s : all)
        if (t == toString(s))
            return s;
    return JobStatus::unknown;
}

JobPhase jobPhaseFromString(std::string_view t)
{
    if (t.empty())
        return JobPhase::none;
    if (t == "download")
        return JobPhase::download;
    if (t == "merge")
        return JobPhase::merge;
    if (t == "extract")
        return JobPhase::extract;
    if (t == "finish")
        return JobPhase::finish;
    return JobPhase::unknown;
}

const char *toString(JobPhase phase)
{
    switch (phase)
    {
    case JobPhase::download: return "download";
    case JobPhase::merge: return "merge";
    case JobPhase::extract: return "extract";
    case JobPhase::finish: return "finish";
    case JobPhase::none: return "";
    default: return "unknown";
    }
}

bool isActiveJobStatus(JobStatus s)
{
    switch (s)
    {
    case JobStatus::queued: case JobStatus::starting: case JobStatus::downloading: case JobStatus::merging:
    case JobStatus::extracting: case JobStatus::finalizing: case JobStatus::waitingPassword:
    case JobStatus::waitingChoice: case JobStatus::pausing: case JobStatus::cancelling: case JobStatus::paused:
        return true;
    default:
        return false;
    }
}

bool isRunningJobStatus(JobStatus s)
{
    switch (s)
    {
    case JobStatus::starting: case JobStatus::downloading: case JobStatus::merging: case JobStatus::extracting:
    case JobStatus::finalizing: case JobStatus::pausing: case JobStatus::cancelling:
        return true;
    default:
        return false;
    }
}

bool isPausableJobStatus(JobStatus s)
{
    switch (s)
    {
    case JobStatus::queued: case JobStatus::starting: case JobStatus::downloading: case JobStatus::merging:
    case JobStatus::extracting:
        return true;
    default:
        return false;
    }
}

bool isTerminalJobStatus(JobStatus s)
{
    return s == JobStatus::completed || s == JobStatus::failed || s == JobStatus::cancelled;
}

int queuePollDelayMs(const std::vector<QueueJob> &jobs)
{
    for (const QueueJob &j : jobs)
        if (isActiveJobStatus(j.status))
            return 1000;
    return 2500;
}

bool queueJobStreamMode(const QueueJob &job)
{
    if (job.hasStreamMode)
        return job.streamMode; // streamMode === true / === false
    return job.archiveMode == "stream";
}

// ------------------------------------------------------------ installed/APR -

AprState aprEmuState(const AprEmu &apr)
{
    std::string reason = lowered(apr.reason);
    if (apr.updateAvailable && apr.canUpdate)
        return AprState::update;
    if (apr.present && !apr.remoteAvailable)
        return AprState::unavailable;
    if (apr.present && !apr.updateAvailable)
        return AprState::current;
    if (reason.find("unsupported") != std::string::npos)
        return AprState::unsupported;
    if (!apr.present)
        return AprState::missing;
    return AprState::unknown;
}

std::string aprEmuVersionText(const AprEmu &apr)
{
    if (!apr.present)
        return {};
    std::string version = (!apr.version.empty() && apr.version != "unknown") ? "v" + apr.version : "unknown";
    std::string build = apr.build.empty() ? "" : " " + apr.build;
    return version + build;
}

std::string aprVersionLabel(const AprVersion &v)
{
    return "v" + v.version + " " + (v.build == "debug" ? "debug" : "release");
}

namespace
{
struct VersionPart
{
    double number = 0;
    int suffix = 0;
};

std::vector<VersionPart> aprVersionParts(const std::string &value)
{
    std::vector<VersionPart> parts;
    std::size_t pos = 0;
    while (true)
    {
        const std::size_t dot = value.find('.', pos);
        const std::string piece = value.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        VersionPart p;
        std::size_t i = 0;
        while (i < piece.size() && is_digit(piece[i]))
            ++i;
        if (i > 0)
        {
            p.number = std::strtod(piece.substr(0, i).c_str(), nullptr); // no throw on absurd digit runs
            p.suffix = i < piece.size() ? -1 : 0;
        }
        parts.push_back(p);
        if (dot == std::string::npos)
            break;
        pos = dot + 1;
    }
    return parts;
}

// app.js:1022 compareAprVersionDesc (localeCompare replaced by byte order).
int compareAprVersionDesc(const AprVersion &l, const AprVersion &r)
{
    const auto lp = aprVersionParts(l.version);
    const auto rp = aprVersionParts(r.version);
    const std::size_t n = std::max(lp.size(), rp.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const VersionPart a = i < lp.size() ? lp[i] : VersionPart{};
        const VersionPart b = i < rp.size() ? rp[i] : VersionPart{};
        if (a.number != b.number)
            return b.number > a.number ? 1 : -1;
        if (a.suffix != b.suffix)
            return b.suffix - a.suffix;
    }
    const int lb = l.build == "release" ? 1 : 0;
    const int rb = r.build == "release" ? 1 : 0;
    if (lb != rb)
        return rb - lb;
    return l.id < r.id ? -1 : (l.id > r.id ? 1 : 0);
}
} // namespace

std::vector<AprVersion> sortedAprVersions(std::vector<AprVersion> versions, const std::string &latestReleaseId)
{
    std::stable_sort(versions.begin(), versions.end(), [&](const AprVersion &l, const AprVersion &r) {
        if (l.id == latestReleaseId && r.id != latestReleaseId)
            return true;
        if (r.id == latestReleaseId && l.id != latestReleaseId)
            return false;
        return compareAprVersionDesc(l, r) < 0;
    });
    return versions;
}

std::string installedArtUrl(const InstalledGame &game, const std::string &kind)
{
    const bool pic0 = kind == "pic0";
    const std::string &given = pic0 ? game.pic0Url : game.iconUrl;
    if (!given.empty())
        return given;
    return std::string("/appdb/") + (pic0 ? "pic0" : "icon") + "?id=" + encodeURIComponent(game.titleId);
}

std::string installedVersionText(const InstalledGame &game)
{
    if (!game.contentVersion.empty())
        return game.contentVersion;
    if (!game.version.empty())
        return game.version;
    return game.masterVersion;
}

namespace
{
std::string firmwareVersionFromQuartet(const std::string &q)
{
    if (q.size() != 4)
        return {};
    for (const char c : q)
        if (!is_digit(c))
            return {};
    const int major = std::stoi(q.substr(0, 2));
    if (major == 0 || major > 25)
        return {};
    return std::to_string(major) + "." + q.substr(2, 2);
}

std::string pad_start(const std::string &s, std::size_t width)
{
    return s.size() >= width ? s : std::string(width - s.size(), '0') + s;
}

std::string firmwareVersionFromHexLike(std::string value)
{
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
        value = value.substr(2);
    std::vector<std::string> candidates;
    auto push = [&](std::string c) {
        c = lowered(c);
        if (!c.empty() && std::find(candidates.begin(), candidates.end(), c) == candidates.end())
            candidates.push_back(c);
    };
    push(value);
    std::size_t z = 0;
    while (z < value.size() && value[z] == '0')
        ++z;
    push(value.substr(z));
    if (value.size() > 8)
        push(value.substr(value.size() - 8));
    for (const std::string &c : candidates)
    {
        std::vector<std::string> forms = {c};
        if (c.size() < 8)
            forms.push_back(pad_start(c, 8));
        if (c.size() < 6)
            forms.push_back(pad_start(c, 6));
        for (const std::string &f : forms)
        {
            const std::string v = firmwareVersionFromQuartet(f.substr(0, 4));
            if (!v.empty())
                return v;
        }
    }
    return {};
}
} // namespace

std::string psFirmwareVersion(std::string_view rawIn)
{
    const std::string source(js_trim(rawIn));
    // /\b(\d{1,2})\.(\d{2})\b/ : leftmost match, greedy 2 digits then 1.
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        if (!is_digit(source[i]) || (i > 0 && is_word(source[i - 1])))
            continue;
        for (std::size_t len = 2; len >= 1; --len)
        {
            if (len == 2 && !(i + 1 < source.size() && is_digit(source[i + 1])))
                continue;
            const std::size_t j = i + len;
            if (j + 2 < source.size() && source[j] == '.' && is_digit(source[j + 1]) && is_digit(source[j + 2]) &&
                (j + 3 == source.size() || !is_word(source[j + 3])))
                return std::to_string(std::stoi(source.substr(i, len))) + "." + source.substr(j + 1, 2);
            if (len == 1)
                break;
        }
    }
    // matchAll(/0x[0-9a-f]+|[0-9a-f]{4,}/gi)
    std::size_t i = 0;
    while (i < source.size())
    {
        std::size_t matchLen = 0;
        if (source[i] == '0' && i + 2 < source.size() + 0 && (source[i + 1] == 'x' || source[i + 1] == 'X') &&
            is_hex(source[i + 2]))
        {
            std::size_t k = i + 2;
            while (k < source.size() && is_hex(source[k]))
                ++k;
            matchLen = k - i;
        }
        else
        {
            std::size_t k = i;
            while (k < source.size() && is_hex(source[k]))
                ++k;
            if (k - i >= 4)
                matchLen = k - i;
        }
        if (matchLen == 0)
        {
            ++i;
            continue;
        }
        const std::string token = source.substr(i, matchLen);
        i += matchLen;
        const bool hadPrefix = token.size() > 1 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
        const std::string clean = hadPrefix ? token.substr(2) : token;
        bool allDigits = clean.size() >= 9;
        for (const char c : clean)
            allDigits = allDigits && is_digit(c);
        if (!hadPrefix && allDigits && clean.size() <= 16)
        {
            const unsigned long long n = std::stoull(clean);
            if (n <= 9007199254740991ULL) // Number.isSafeInteger
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%llx", n);
                const std::string v = firmwareVersionFromHexLike(buf);
                if (!v.empty())
                    return v;
            }
        }
        const std::string direct = firmwareVersionFromHexLike(clean);
        if (!direct.empty())
            return direct;
    }
    return {};
}

std::string installedFirmwareText(std::string_view raw)
{
    if (raw.empty())
        return {};
    const std::string v = psFirmwareVersion(raw);
    return v.empty() ? std::string() : "FW " + v;
}

std::string installedFormatText(const InstalledGame &game)
{
    if (!game.format.empty())
        return game.format;
    return game.storageKind == "folder" ? "folder" : "";
}

std::string installedLocationText(const InstalledGame &game)
{
    if (!game.backingPath.empty())
        return game.backingPath;
    if (!game.installPath.empty())
        return game.installPath;
    return game.metadataPath;
}

// --------------------------------------------------------- settings/browse --

bool validDownloadConnections(std::int64_t value)
{
    return value >= 1 && value <= kMaxDownloadConnections;
}

std::string pathParent(std::string_view path)
{
    std::string value(path.empty() ? "/" : path);
    if (value == "/")
        return value;
    const std::size_t slash = value.rfind('/');
    // value.replace(/\/[^/]+$/, '') — only strips when a final non-empty segment exists.
    if (slash != std::string::npos && slash + 1 < value.size())
        value = value.substr(0, slash);
    return value.empty() ? "/" : value;
}

std::string pathJoin(std::string_view base, std::string_view name)
{
    return base == "/" ? "/" + std::string(name) : std::string(base) + "/" + std::string(name);
}

const Mount *mountForPath(const std::vector<Mount> &mounts, std::string_view path)
{
    const std::string value(path.empty() ? "/" : path);
    const Mount *best = nullptr;
    for (const Mount &m : mounts)
    {
        if (value == m.path || starts_with(value, m.path + "/"))
            if (!best || m.path.size() > best->path.size())
                best = &m;
    }
    return best;
}

bool canSelectBrowsePath(const std::vector<Mount> &mounts, std::string_view path)
{
    const Mount *m = mountForPath(mounts, path);
    return m && m->writable;
}

std::string validateName(std::string_view name)
{
    const std::string clean(js_trim(name));
    if (clean.empty() || clean.find('/') != std::string::npos || clean == "." || clean == "..")
        return {};
    return clean;
}

// -------------------------------------------------------------- file manager -

std::string normalizeFilePath(std::string_view path)
{
    if (path.empty() || path[0] != '/')
        return {};
    std::string value;
    for (const char c : path)
        if (!(c == '/' && !value.empty() && value.back() == '/'))
            value.push_back(c);
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    return value.empty() ? "/" : value;
}

std::vector<std::string> uniquePaths(const std::vector<std::string> &paths)
{
    std::vector<std::string> out;
    for (const std::string &p : paths)
    {
        const std::string n = normalizeFilePath(p);
        if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
    }
    return out;
}

bool isPkgEntry(const FileEntry &entry)
{
    return entry.type == "file" && ends_with(lowered(entry.name), ".pkg");
}

bool isExtractableRarEntry(const FileEntry &entry)
{
    if (entry.type != "file")
        return false;
    const std::string name = lowered(entry.name);
    if (!ends_with(name, ".rar"))
        return false;
    // /\.part\d+\.rar$/
    const std::string stem = name.substr(0, name.size() - 4);
    std::size_t k = stem.size();
    while (k > 0 && is_digit(stem[k - 1]))
        --k;
    const bool hasDigits = k < stem.size();
    return !(hasDigits && k >= 5 && stem.compare(k - 5, 5, ".part") == 0);
}

FileOpStatus fileOpStatusFromString(std::string_view t)
{
    if (t == "queued") return FileOpStatus::queued;
    if (t == "starting") return FileOpStatus::starting;
    if (t == "running") return FileOpStatus::running;
    if (t == "copying") return FileOpStatus::copying;
    if (t == "installing") return FileOpStatus::installing;
    if (t == "waiting_password") return FileOpStatus::waitingPassword;
    if (t == "accepted") return FileOpStatus::accepted;
    if (t == "completed") return FileOpStatus::completed;
    if (t == "failed") return FileOpStatus::failed;
    if (t == "cancelled") return FileOpStatus::cancelled;
    return FileOpStatus::unknown;
}

bool isActiveFileOp(FileOpStatus s)
{
    return s == FileOpStatus::queued || s == FileOpStatus::running || s == FileOpStatus::copying ||
           s == FileOpStatus::installing || s == FileOpStatus::waitingPassword;
}

bool isTerminalFileOp(FileOpStatus s)
{
    return s == FileOpStatus::accepted || s == FileOpStatus::completed || s == FileOpStatus::failed ||
           s == FileOpStatus::cancelled;
}

bool canCancelFileOp(const FileOperation &op)
{
    return (op.status == FileOpStatus::running || op.status == FileOpStatus::waitingPassword) && op.cancelable;
}

const char *toString(Collision c)
{
    switch (c)
    {
    case Collision::replace: return "replace";
    case Collision::skip: return "skip";
    default: return "keep-both";
    }
}

Collision collisionFromString(std::string_view t)
{
    if (t == "replace")
        return Collision::replace;
    if (t == "skip")
        return Collision::skip;
    return Collision::keepBoth;
}

// ------------------------------------------------------------ URL encoding --

std::string encodeURIComponent(std::string_view text)
{
    std::string out;
    for (const char ch : text)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (is_digit(ch) || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            std::string_view("-_.!~*'()").find(ch) != std::string_view::npos)
            out.push_back(ch);
        else
            percent_encode_into(out, c);
    }
    return out;
}

std::string formUrlEncode(std::string_view text)
{
    std::string out;
    for (const char ch : text)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (is_digit(ch) || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            std::string_view("*-._").find(ch) != std::string_view::npos)
            out.push_back(ch);
        else if (ch == ' ')
            out.push_back('+');
        else
            percent_encode_into(out, c);
    }
    return out;
}

} // namespace wings::logic
