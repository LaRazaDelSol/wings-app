// Wings TV app - A fake backend for the PC preview and tests.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generated art is adapted from Orbit Store's host/fixture.cpp
// (GPL-3.0-or-later).

#include "fake_backend.hpp"

#include "../third_party/stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <unistd.h>

namespace orbit::host
{

namespace
{

constexpr double kGB = 1e9;

std::uint32_t hash(const std::string &text)
{
    std::uint32_t h = 2166136261u;
    for (const char c : text)
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return h;
}

void hsv(float h, float s, float v, std::uint8_t *out)
{
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h * 6.0f) % 6)
    {
    case 0:
        r = c, g = x;
        break;
    case 1:
        r = x, g = c;
        break;
    case 2:
        g = c, b = x;
        break;
    case 3:
        g = x, b = c;
        break;
    case 4:
        r = x, b = c;
        break;
    default:
        r = c, b = x;
        break;
    }
    const float m = v - c;
    out[0] = static_cast<std::uint8_t>((r + m) * 255.0f);
    out[1] = static_cast<std::uint8_t>((g + m) * 255.0f);
    out[2] = static_cast<std::uint8_t>((b + m) * 255.0f);
}

void append(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data),
                                                static_cast<std::size_t>(size));
}

std::string make_art(const std::string &seed, bool hero)
{
    const int width = hero ? 960 : 512;
    const int height = hero ? 540 : 512;
    const std::uint32_t h = hash(seed);
    const float hue_a = static_cast<float>(h % 360) / 360.0f;
    const float hue_b = std::fmod(hue_a + 0.12f + static_cast<float>((h >> 9) % 20) / 100.0f, 1.0f);
    std::uint8_t a[3];
    std::uint8_t b[3];
    hsv(hue_a, 0.65f, 0.55f, a);
    hsv(hue_b, 0.55f, 0.95f, b);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    const float cx = width * (0.35f + static_cast<float>((h >> 4) % 30) / 100.0f);
    const float cy = height * 0.42f;
    const float radius = std::min(width, height) * 0.24f;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float t =
                static_cast<float>(x) / width * 0.4f + static_cast<float>(y) / height * 0.6f;
            std::uint8_t *p = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<std::uint8_t>(a[c] + (b[c] - a[c]) * t);
            // A soft sun and a horizon band: enough shape to judge cropping.
            const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d < radius)
            {
                const float k = std::clamp((radius - d) / 3.0f, 0.0f, 1.0f) * 0.85f;
                for (int c = 0; c < 3; ++c)
                    p[c] = static_cast<std::uint8_t>(p[c] + (255 - p[c]) * k);
            }
            if (y > height * 0.7f +
                        std::sin(static_cast<float>(x) * 0.02f + static_cast<float>(h % 7)) *
                            height * 0.02f)
                for (int c = 0; c < 3; ++c)
                    p[c] = static_cast<std::uint8_t>(p[c] * 0.45f);
            p[3] = 255;
        }
    }
    std::string png;
    stbi_write_png_to_func(append, &png, width, height, 4, pixels.data(), width * 4);
    return png;
}

struct SamplePackage
{
    const char *title;
    const char *title_id;
    int source; // index into kSources
    double gb;
    int links;
};

constexpr const char *kSources[] = {"Sample Source One", "Sample Source Two", "Community Mirror"};

constexpr SamplePackage kPackages[] = {
    {"Ember Hollow", "PPSA90001", 0, 38.4, 5},     {"Ironclad Skies", "PPSA90002", 0, 52.1, 2},
    {"Neon Courier", "PPSA90003", 1, 21.7, 4},     {"Quiet Harbour", "PPSA90004", 1, 12.3, 1},
    {"Tidewater", "PPSA90005", 0, 64.9, 2},        {"Copper Circuit", "CUSA90006", 2, 8.8, 3},
    {"Glass Meridian", "PPSA90007", 2, 44.0, 2},   {"Lantern Road", "PPSA90101", 1, 17.5, 1},
    {"Hollow Crown", "PPSA90009", 0, 71.2, 7},     {"Saltwind", "PPSA90102", 2, 29.9, 2},
    {"Paper Tigers", "PPSA90103", 1, 6.4, 1},      {"Orchard Station", "PPSA90104", 0, 15.0, 2},
    {"Static Bloom", "PPSA90013", 2, 33.3, 3},     {"Marrow & Moss", "PPSA90014", 1, 24.8, 2},
    {"Vesper Line", "PPSA90015", 0, 47.6, 4},      {"Kite Season", "PPSA90105", 2, 9.1, 1},
    {"Last Tram North", "PPSA90017", 1, 19.4, 2},  {"Brass Meridian", "CUSA90018", 0, 5.2, 1},
    {"Moth & Lantern", "PPSA90019", 2, 27.7, 3},   {"Cinder Fields", "PPSA90020", 1, 41.0, 2},
    {"Wren", "PPSA90021", 0, 3.9, 1},              {"Foxglove Deep: The Long Descent into the Hollow Orchard, Definitive Edition", "PPSA90022", 2, 58.3, 3},
    {"Harbour Lights", "PPSA90023", 1, 13.6, 2},   {"Skyward Ten", "PPSA90024", 0, 36.2, 2},
    // One game listed three times (two region SKUs, three sources) with links
    // named as the real catalogues name them: see rich_links().
    {"Starfall Odyssey", "PPSA90025", 0, 0.0, 0},  {"[exFAT] Starfall Odyssey", "PPSA90026", 1, 0.0, 0},
    {"Starfall Odyssey v01.005", "PPSA90025", 2, 61.2, 0},
};

struct SampleLink
{
    const char *name;
    const char *host;
    LinkRoute route;
    const char *provider;
    const char *services; // "Real-Debrid" or "Real-Debrid,TorBox"
};

// Link k of every package; the first links of a package are the interesting ones.
constexpr SampleLink kLinks[] = {
    {"Game (PKG)", "files.example", LinkRoute::native_pkg, "", ""},
    {"Game part 1 + 2", "mediafire.com", LinkRoute::automated, "MediaFire", ""},
    {"Mirror (needs a check)", "fileshare.example", LinkRoute::capture, "", ""},
    {"Premium mirror", "rapidgator.net", LinkRoute::debrid, "", "Real-Debrid"},
    {"Season pack (several files)", "1fichier.com", LinkRoute::choose_debrid, "",
     "Real-Debrid,TorBox"},
    {"Direct mirror (supported host)", "mega.nz", LinkRoute::capture, "MEGA", ""},
    {"Old mirror (not supported)", "dead-host.example", LinkRoute::browser_only, "Dead Host", ""},
};
constexpr int kLinkKinds = 7;

std::vector<std::string> split_services(const char *text)
{
    std::vector<std::string> out;
    std::string current;
    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p == ',')
        {
            out.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(*p);
        }
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

// Packages 25 to 27 ("Starfall Odyssey"): what the game page is about. A
// 22-part set a debrid service takes, a 4-part browser set, a set with a gap
// (part 3 missing), a set whose numbering restarts, a backport for old
// firmware, a build too new for 10.xx, decrypt-page and link-vault wrappers,
// update, DLC and fix links, and one URL listed by two sources.
bool is_rich(int n)
{
    return n >= 25 && n <= 27;
}

std::vector<PackageLink> rich_links(int n)
{
    std::vector<PackageLink> out;
    const auto add = [&](const std::string &name, const char *host, LinkRoute route, const char *provider,
                         const char *services, const std::string &url, bool browser)
    {
        PackageLink link;
        link.index = static_cast<int>(out.size());
        link.name = name;
        link.host = host;
        link.route = route;
        link.provider = provider;
        link.services = split_services(services);
        link.url = url;
        link.needs_browser = browser;
        out.push_back(std::move(link));
    };
    const auto part = [](const char *prefix, int k) { return std::string(prefix) + std::to_string(k); };
    if (n == 25)
    {
        for (int k = 1; k <= 22; ++k)
            add(part("USA 12.xx Base - Vikingfile part ", k), "vikingfile.com", LinkRoute::debrid, "",
                "Real-Debrid", part("https://vikingfile.example/f/sf25-", k), false);
        for (int k = 1; k <= 4; ++k)
            add(part("USA 12.xx Base - Akirabox part ", k), "akirabox.com", LinkRoute::capture, "Akirabox", "",
                part("https://akirabox.example/sf25-", k), true);
        add("USA 7.xx BackPort 4.xx - 1fichier", "1fichier.com", LinkRoute::debrid, "", "Real-Debrid",
            "https://1fichier.example/?sf25bp", false);
        for (const int k : {1, 2, 4})
            add(part("USA 12.xx Base - Rootz part ", k), "rootz.so", LinkRoute::capture, "", "",
                part("https://rootz.example/sf25-", k), true);
        add("USA 9.xx Update 01.003 - Akirabox", "akirabox.to", LinkRoute::capture, "Akirabox", "",
            "https://akirabox.example/sf25-update", true);
        add("USA 10.xx DLC (@Pkykzhack) - Vikingfile", "vikingfile.com", LinkRoute::debrid, "", "Real-Debrid",
            "https://vikingfile.example/f/sf25-dlc", false);
    }
    else if (n == 26)
    {
        add("EUR exFAT 01.005 Base - AKIA (exFAT)", "pippo26442999.github.io", LinkRoute::capture, "", "",
            "https://pippo26442999.github.io/library-decrypt/#sf26-akia", true);
        add("EUR exFAT 01.005 Base - VIKI (exFAT)", "pippo26442999.github.io", LinkRoute::capture, "", "",
            "https://pippo26442999.github.io/library-decrypt/#sf26-viki", true);
        for (const int k : {1, 2, 3, 1, 2})
            add(part("EUR 10.xx Base - Akirabox part ", k), "akirabox.com", LinkRoute::capture, "Akirabox", "",
                part("https://akirabox.example/sf26-", static_cast<int>(out.size())), true);
        add("EUR 13.xx Base - Gofile", "gofile.io", LinkRoute::capture, "", "", "https://gofile.example/d/sf26",
            true);
    }
    else if (n == 27)
    {
        add("AkiraBox", "link-vault.org", LinkRoute::capture, "", "", "https://link-vault.example/c/sf27#Akirabox",
            true);
        add("Fpkg - VikingFile", "link-vault.org", LinkRoute::capture, "", "",
            "https://link-vault.example/c/sf27#VikingFile", true);
        add("4.xx BackPort - DataNodes", "datanodes.to", LinkRoute::automated, "DataNodes", "",
            "https://datanodes.example/sf27bp", false);
        add("Fix - DataNodes", "datanodes.to", LinkRoute::automated, "DataNodes", "",
            "https://datanodes.example/sf27fix", false);
        // The same file the first source lists (sources repost each other's links).
        add("Dump - 1fichier", "1fichier.com", LinkRoute::debrid, "", "Real-Debrid",
            "https://1fichier.example/?sf25bp", false);
    }
    return out;
}

const char *rich_description(int n)
{
    switch (n)
    {
    case 25:
        return "Starfall Odyssey is an invented game used to preview Wings. Nothing here is real.\nRegion: USA";
    case 26:
        return "Starfall Odyssey is an invented game used to preview Wings. Nothing here is real.\nRegion: EUR";
    default:
        break;
    }
    return "Starfall Odyssey is an invented game used to preview Wings. Nothing here is real.\n"
           "Tags: PPSA90025, v01.005, USA, 4.xx BackPort";
}

const SamplePackage *find_package(std::string_view id)
{
    if (id.substr(0, 3) != "pkg")
        return nullptr;
    const int n = std::atoi(std::string(id.substr(3)).c_str());
    if (n < 1 || n > static_cast<int>(sizeof(kPackages) / sizeof(kPackages[0])))
        return nullptr;
    return &kPackages[n - 1];
}

// Which link kind a package's links start at, so the grid shows every tier:
// most start with the .pkg; some with a debrid host, an unknown host, an
// unsupported one or a supported browser provider.
int kind_offset(int n)
{
    switch (n % 8)
    {
    case 0:
        return 3; // debrid first
    case 3:
        return 2; // manual capture: unknown
    case 5:
        return 6; // browser only: not supported
    case 6:
        return 5; // supported browser provider
    default:
        return 0;
    }
}

Package make_package(int n, const SamplePackage &sample)
{
    Package package;
    package.id = "pkg" + std::to_string(n);
    package.title = sample.title;
    package.title_id = sample.title_id;
    package.source_name = kSources[sample.source];
    package.source_id = "src" + std::to_string(sample.source + 1);
    package.version = "01.00";
    package.size_bytes = sample.gb * kGB;
    package.link_count = sample.links;
    if (is_rich(n))
    {
        const std::vector<PackageLink> links = rich_links(n);
        package.link_count = static_cast<int>(links.size());
        for (const PackageLink &link : links)
        {
            const LinkTier tier = link_tier(link);
            package.has_pkg = package.has_pkg || tier == LinkTier::native_pkg;
            package.has_automated = package.has_automated || tier == LinkTier::automated;
            package.has_supported = package.has_supported || tier == LinkTier::supported;
        }
        package.version = n == 27 ? "v01.005" : "01.00";
        package.description = rich_description(n);
        return package;
    }
    const int offset = kind_offset(n);
    for (int k = 0; k < sample.links; ++k)
    {
        const int kind = (k + offset) % kLinkKinds;
        package.has_pkg = package.has_pkg || kind == 0;
        package.has_automated = package.has_automated || kind == 1;
        package.has_supported = package.has_supported || kind == 5;
    }
    package.description = std::string(sample.title) +
                          " is an invented game used to preview Wings. Nothing here is real.";
    return package;
}

} // namespace

FakeBackend::FakeBackend(FakeMode mode) : mode_(mode)
{
    const auto job = [](const char *id, const char *name, const char *title_id, const char *status,
                        double total_gb, double fraction, double speed_mb, const char *error = "")
    {
        Job j;
        j.id = id;
        j.name = name;
        j.title_id = title_id;
        j.status = status;
        j.total = total_gb * kGB;
        j.received = j.total * fraction;
        j.speed = speed_mb * 1e6;
        j.eta_seconds = speed_mb > 0.0 ? (j.total - j.received) / j.speed : -1.0;
        j.error = error;
        return j;
    };
    if (mode != FakeMode::empty && mode != FakeMode::onboarding)
    {
        jobs_ = {job("job1", "Ember Hollow", "PPSA90001", "downloading", 38.4, 0.42, 96.0),
                 job("job2", "Neon Courier", "PPSA90003", "extracting", 21.7, 1.0, 0.0),
                 job("job3", "Ironclad Skies", "PPSA90002", "queued", 52.1, 0.0, 0.0),
                 job("job4", "Tidewater", "PPSA90005", "paused", 64.9, 0.17, 0.0),
                 job("job5", "Quiet Harbour", "PPSA90004", "completed", 12.3, 1.0, 0.0),
                 job("job6", "Copper Circuit", "CUSA90006", "failed", 8.8, 0.31, 0.0,
                     "Host stopped answering"),
                 job("job7", "Hollow Crown (archive)", "PPSA90009", "waitingPassword", 71.2, 1.0,
                     0.0),
                 job("job8", "Glass Meridian (archive)", "PPSA90007", "waitingChoice", 44.0, 1.0,
                     0.0)};
        jobs_[1].phase = "extract";
        jobs_[1].progress = 0.64;
        jobs_[1].entry = "data/stream_00.bin";
        jobs_[0].phase = "download";
        jobs_[0].source_name = kSources[0];
        jobs_[6].detail = "Archive password required";
        jobs_[7].choices = {{0, "Glass Meridian [base game].pkg"},
                            {1, "Glass Meridian [update 1.02].pkg"},
                            {2, "Glass Meridian [DLC pack].pkg"}};
        const struct
        {
            const char *level;
            const char *component;
            const char *message;
        } lines[] = {{"info", "server", "Pegasus DL 1.11.0 listening on port 6970"},
                     {"info", "sources", "Loaded 3 sources (2,962 packages)"},
                     {"info", "queue", "job1 started: Ember Hollow"},
                     {"warn", "queue", "job6 retry 2/3: host stopped answering"},
                     {"error", "queue", "job6 failed: Host stopped answering"},
                     {"info", "queue", "job5 completed: Quiet Harbour"},
                     {"info", "extract", "job2 extracting 3 archives"},
                     {"info", "appdb", "Found 5 installed titles"}};
        std::int64_t seq = 1;
        for (const auto &line : lines)
            logs_.push_back({seq++, line.level, line.component, line.message});
    }
    if (mode != FakeMode::empty && mode != FakeMode::onboarding)
    {
        const auto source = [](const char *id, const char *name, const char *kind, bool enabled,
                               bool managed, int count, const char *status)
        {
            Source s;
            s.id = id;
            s.name = name;
            s.kind = kind;
            s.enabled = enabled;
            s.managed = managed;
            s.deletable = !managed;
            s.package_count = count;
            s.status = status;
            s.last_refresh_at = 1790000000;
            return s;
        };
        sources_ = {source("src1", kSources[0], "url", true, false, 9, "ok"),
                    source("src2", kSources[1], "file", true, false, 8, "ok"),
                    source("src3", kSources[2], "default", false, true, 7, "ok")};
        sources_[0].url = "https://sources.example/one.json";
    }
    onboarding_ = mode == FakeMode::onboarding;
    seed_admin_locked();
    {
        const auto game = [](const char *id, const char *title, const char *version,
                             const char *path, double gb, bool apr)
        {
            InstalledGame g;
            g.title_id = id;
            g.title = title;
            g.version = version;
            g.path = path;
            g.size_bytes = gb * kGB;
            g.apr_emu = apr;
            g.storage = "folder";
            g.firmware = "FW 9.60";
            g.apr.present = apr;
            return g;
        };
        installed_ = {game("PPSA90101", "Lantern Road", "01.02", "/user/app/PPSA90101", 17.5, true),
                      game("PPSA90102", "Saltwind", "01.00", "/user/app/PPSA90102", 29.9, false),
                      game("PPSA90103", "Paper Tigers", "01.05", "/user/app/PPSA90103", 6.4, true),
                      game("PPSA90104", "Orchard Station", "02.00", "/mnt/ext0/PPSA90104", 15.0,
                           false),
                      game("PPSA90105", "Kite Season", "01.01", "/mnt/ext0/PPSA90105", 9.1, true)};
        installed_[3].storage = "image";
        installed_[0].apr.state = "update";
        installed_[0].apr.version_text = "1.3.2 release";
        installed_[0].apr.latest = "1.4.0";
        installed_[0].apr.update_available = true;
        installed_[0].apr.can_update = true;
        installed_[0].apr.can_reinstall = true;
        installed_[2].apr.state = "current";
        installed_[2].apr.version_text = "1.4.0 release";
        installed_[2].apr.latest = "1.4.0";
        installed_[2].apr.can_reinstall = true;
        installed_[4].apr.state = "unavailable";
        installed_[4].apr.version_text = "1.3.0 release";
        installed_[4].apr.remote_available = false;
        installed_[4].apr.reason = "Pegasus DL could not reach the APR Emu download server.";
    }
    defaults_ = {
        {"mirror-a", "Sample Mirror A", "A community catalogue of sample titles.", true, false,
         false, true, 1200},
        {"mirror-b", "Sample Mirror B", "Homebrew and utilities.", true, false, false, true, 310},
        {"mirror-c", "Sample Mirror C", "Large archive, slow to refresh.", false, false, false,
         false, 5400},
    };
}

void FakeBackend::set_mode(FakeMode mode)
{
    Lock lock(mutex_);
    mode_ = mode;
}

void FakeBackend::set_capture_steps(int steps)
{
    Lock lock(mutex_);
    capture_steps_ = steps;
}

int FakeBackend::cancelled_captures() const
{
    Lock lock(mutex_);
    return cancelled_;
}

void FakeBackend::set_failing_link(const std::string &package_id, int link_index)
{
    Lock lock(mutex_);
    failing_link_ = package_id + "#" + std::to_string(link_index);
}

int FakeBackend::started_links() const
{
    Lock lock(mutex_);
    return started_links_;
}

template <typename T> Result<T> FakeBackend::refuse() const
{
    Result<T> result;
    result.status = 0;
    result.error = "connection refused";
    return result;
}

void FakeBackend::log_locked(const char *level, const char *component, std::string message)
{
    logs_.push_back({static_cast<std::int64_t>(logs_.size()) + 1, level, component,
                     std::move(message)});
}

Job &FakeBackend::add_job_locked(const std::string &name, const char *status, double total_gb)
{
    Job job;
    job.id = "job" + std::to_string(next_job_++);
    job.name = name;
    job.status = status;
    job.total = total_gb * kGB;
    job.eta_seconds = -1.0;
    jobs_.push_back(std::move(job));
    return jobs_.back();
}

Result<PegasusInfo> FakeBackend::health()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<PegasusInfo>();
    Result<PegasusInfo> result;
    result.status = 200;
    result.value.version = "1.11.0";
    result.value.adapter = "fake";
    result.value.compat = "tested";
    result.value.features = {{"Settings", true},        {"Sources", true},
                             {"Catalogue", true},       {"Queue", true},
                             {"Debrid", true},          {"Folder browser", true},
                             {"Installed games", true}, {"APR Emu", true},
                             {"APR Emu versions", apr_manifest_ok_},
                             {"File manager", true}};
    if (!apr_manifest_ok_)
        result.value.notes = {"APR Emu versions: the manifest has too many versions (HTTP 503)"};
    return result;
}

void FakeBackend::advance_locked()
{
    // The running download creeps forward a little on every look at the queue.
    ++polls_;
    for (Job &job : jobs_)
    {
        if (job.status == "downloading")
        {
            job.received = std::min(job.total, job.received + job.speed * 1.0);
            job.eta_seconds = job.speed > 0.0 ? (job.total - job.received) / job.speed : -1.0;
        }
    }
}

Result<std::vector<Job>> FakeBackend::queue()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Job>>();
    advance_locked();
    Result<std::vector<Job>> result;
    result.status = 200;
    result.value = jobs_;
    return result;
}

Result<std::vector<Package>> FakeBackend::library()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Package>>();
    Result<std::vector<Package>> result;
    result.status = 200;
    if (mode_ == FakeMode::empty || sources_.empty())
        return result;
    int n = 1;
    for (const SamplePackage &sample : kPackages)
        result.value.push_back(make_package(n++, sample));
    return result;
}

Result<PackageDetail> FakeBackend::package(std::string_view id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<PackageDetail>();
    Result<PackageDetail> result;
    const SamplePackage *sample = find_package(id);
    if (sample == nullptr)
    {
        result.status = 404;
        result.error = "That package is not in the catalogue any more";
        return result;
    }
    result.status = 200;
    result.value.package = make_package(std::atoi(std::string(id.substr(3)).c_str()), *sample);
    if (is_rich(std::atoi(std::string(id.substr(3)).c_str())))
    {
        result.value.links = rich_links(std::atoi(std::string(id.substr(3)).c_str()));
        return result;
    }
    const int offset = kind_offset(std::atoi(std::string(id.substr(3)).c_str()));
    for (int k = 0; k < sample->links; ++k)
    {
        const SampleLink &link = kLinks[(k + offset) % kLinkKinds];
        PackageLink entry;
        entry.index = k;
        entry.name = link.name;
        if (k >= kLinkKinds)
            entry.name += " (" + std::to_string(k / kLinkKinds + 1) + ")";
        entry.host = link.host;
        entry.route = link.route;
        entry.needs_browser = link.route == LinkRoute::capture || link.route == LinkRoute::automated;
        entry.provider = link.provider;
        entry.services = split_services(link.services);
        result.value.links.push_back(std::move(entry));
    }
    return result;
}

Result<std::vector<std::uint8_t>> FakeBackend::package_tiers()
{
    // The best tier of each package from the same link kinds package() builds.
    Result<std::vector<std::uint8_t>> result;
    result.status = 200;
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<std::uint8_t>>();
    if (mode_ == FakeMode::empty || sources_.empty())
        return result;
    int n = 1;
    for (const SamplePackage &sample : kPackages)
    {
        LinkTier best = LinkTier::unsupported;
        bool debrid = false;
        const int offset = kind_offset(n);
        for (const PackageLink &link : is_rich(n) ? rich_links(n) : std::vector<PackageLink>{})
        {
            best = std::min(best, link_tier(link));
            debrid = debrid || link_tier(link) == LinkTier::debrid;
        }
        for (int k = 0; k < sample.links; ++k)
        {
            const SampleLink &link = kLinks[(k + offset) % kLinkKinds];
            PackageLink entry;
            entry.route = link.route;
            entry.provider = link.provider;
            const LinkTier tier = link_tier(entry);
            best = std::min(best, tier);
            debrid = debrid || tier == LinkTier::debrid;
        }
        result.value.push_back(pack_tier(best, debrid));
        ++n;
    }
    return result;
}

Result<std::vector<Source>> FakeBackend::sources()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Source>>();
    Result<std::vector<Source>> result;
    result.status = 200;
    result.value = sources_;
    return result;
}

Result<SettingsView> FakeBackend::settings()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<SettingsView>();
    ++settings_reads_;
    Result<SettingsView> result;
    result.status = 200;
    result.value = settings_;
    return result;
}

Result<std::vector<InstalledGame>> FakeBackend::installed()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<InstalledGame>>();
    Result<std::vector<InstalledGame>> result;
    result.status = 200;
    if (mode_ == FakeMode::empty || mode_ == FakeMode::onboarding)
        return result;
    result.value = installed_;
    return result;
}

Result<std::vector<LogLine>> FakeBackend::logs(std::int64_t since)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<LogLine>>();
    Result<std::vector<LogLine>> result;
    result.status = 200;
    for (const LogLine &line : logs_)
    {
        if (line.seq > since)
            result.value.push_back(line);
    }
    return result;
}

Result<std::vector<Job>> FakeBackend::job_action(std::string_view job_id, std::string_view action)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Job>>();
    Result<std::vector<Job>> result;
    result.status = 200;
    bool found = false;
    for (Job &job : jobs_)
    {
        if (job.id != job_id)
            continue;
        found = true;
        if (action == "pause")
            job.status = "paused";
        else if (action == "resume")
            job.status = "downloading";
        else if (action == "cancel")
            job.status = "cancelled";
        else if (action == "retry")
            job.status = "queued";
        job.speed = job.status == "downloading" ? 96e6 : 0.0;
        job.error.clear();
        log_locked("info", "queue", std::string(job_id) + " " + std::string(action));
    }
    if (!found)
    {
        result.status = 404;
        result.error = "Download not found";
        return result;
    }
    result.value = jobs_;
    return result;
}

Result<std::vector<Job>> FakeBackend::clear_finished()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Job>>();
    Result<std::vector<Job>> result;
    result.status = 200;
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return job_finished(j); }),
                jobs_.end());
    result.value = jobs_;
    return result;
}

Result<std::vector<Job>> FakeBackend::archive_password(std::string_view job_id,
                                                       std::string_view password)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Job>>();
    Result<std::vector<Job>> result;
    result.status = 200;
    for (Job &job : jobs_)
    {
        if (job.id != job_id)
            continue;
        if (password == "wrong")
        {
            job.detail = "That password did not open the archive";
            job.error = job.detail;
        }
        else
        {
            job.status = "extracting";
            job.phase = "extract";
            job.progress = 0.05;
            job.detail.clear();
            job.error.clear();
        }
    }
    result.value = jobs_;
    return result;
}

Result<std::vector<Job>> FakeBackend::archive_choice(std::string_view job_id, int index)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Job>>();
    Result<std::vector<Job>> result;
    result.status = 200;
    for (Job &job : jobs_)
    {
        if (job.id != job_id)
            continue;
        job.status = "extracting";
        job.phase = "extract";
        job.progress = 0.02;
        job.choices.clear();
        log_locked("info", "queue", std::string(job_id) + " chose entry " + std::to_string(index));
    }
    result.value = jobs_;
    return result;
}

Result<LinkResult> FakeBackend::start_link(std::string_view package_id, int link_index,
                                           std::string_view service)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<LinkResult>();
    Result<LinkResult> result;
    const SamplePackage *sample = find_package(package_id);
    const int n = sample == nullptr ? 0 : std::atoi(std::string(package_id.substr(3)).c_str());
    const std::vector<PackageLink> rich = is_rich(n) ? rich_links(n) : std::vector<PackageLink>{};
    const int count = is_rich(n) ? static_cast<int>(rich.size()) : (sample == nullptr ? 0 : sample->links);
    if (sample == nullptr || link_index < 0 || link_index >= count)
    {
        result.status = 404;
        result.error = "Download link not found.";
        return result;
    }
    if (failing_link_ == std::string(package_id) + "#" + std::to_string(link_index))
    {
        result.status = 502;
        result.error = "The host did not answer.";
        return result;
    }
    result.status = 200;
    ++started_links_;
    SampleLink link = kLinks[link_index % kLinkKinds];
    if (is_rich(n))
    {
        const PackageLink &r = rich[static_cast<std::size_t>(link_index)];
        link.route = r.route;
        link.host = r.host.c_str();
    }
    LinkResult &out = result.value;
    switch (link.route)
    {
    case LinkRoute::native_pkg:
        out.kind = LinkResult::Kind::dispatched;
        out.dispatch.native = true;
        out.dispatch.native_title = "Package download started";
        out.dispatch.native_message =
            std::string(sample->title) +
            " was handed to PS5 Downloads. Open Downloads on the home screen to watch it.";
        out.dispatch.toast = "Package download started in PS5 Downloads.";
        break;
    case LinkRoute::debrid:
    case LinkRoute::choose_debrid:
    {
        if (link.route == LinkRoute::choose_debrid && service.empty())
        {
            result.status = 400;
            result.error = "Choose a debrid service first.";
            return result;
        }
        const std::string label = service == "torbox" ? "TorBox" : "Real-Debrid";
        if (link.route == LinkRoute::choose_debrid)
        {
            out.kind = LinkResult::Kind::select_files;
            out.selection_id = "sel1";
            out.service = std::string(service);
            out.files = {{0, "Season 1 - part 1.rar", 4.2 * kGB},
                         {1, "Season 1 - part 2.rar", 4.2 * kGB},
                         {2, "Bonus features.rar", 0.9 * kGB}};
            break;
        }
        out.kind = LinkResult::Kind::dispatched;
        Job &job = add_job_locked(sample->title, "queued", sample->gb);
        job.title_id = sample->title_id;
        out.dispatch.queued = true;
        out.dispatch.has_jobs = true;
        out.dispatch.toast = label + " download queued.";
        out.dispatch.jobs = jobs_;
        break;
    }
    case LinkRoute::automated:
    case LinkRoute::capture:
        out.kind = LinkResult::Kind::capture;
        capture_step_ = 0;
        capture_title_ = sample->title;
        capture_automated_ = link.route == LinkRoute::automated;
        out.capture.active = true;
        out.capture.session_id = "7";
        out.capture.automated = capture_automated_;
        out.capture.requires_browser = true;
        out.capture.title = capture_title_;
        out.capture.host = link.host;
        out.capture.state = "watching";
        out.capture.message = capture_automated_
                                  ? "Browser opened. Waiting for Cloudflare verification..."
                                  : "Browser opened. Waiting for the download URL...";
        break;
    default:
        out.kind = LinkResult::Kind::browser_opened;
        out.message = "Link opened in the PS5 browser.";
        break;
    }
    return result;
}

Result<LinkResult> FakeBackend::debrid_select(std::string_view selection_id,
                                              const std::vector<int> &indexes,
                                              std::string_view service)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<LinkResult>();
    Result<LinkResult> result;
    if (selection_id != "sel1" || indexes.empty())
    {
        result.status = 400;
        result.error = "That selection has expired";
        return result;
    }
    result.status = 200;
    add_job_locked("Season 1 (" + std::to_string(indexes.size()) + " files)", "queued", 9.3);
    result.value.kind = LinkResult::Kind::dispatched;
    result.value.dispatch.queued = true;
    result.value.dispatch.has_jobs = true;
    result.value.dispatch.jobs = jobs_;
    result.value.dispatch.toast =
        std::string(service == "torbox" ? "TorBox" : "Real-Debrid") + " download queued.";
    return result;
}

Result<CaptureView> FakeBackend::capture_status(std::string_view session_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<CaptureView>();
    Result<CaptureView> result;
    result.status = 200;
    CaptureView &view = result.value;
    view.session_id = std::string(session_id);
    view.title = capture_title_;
    view.automated = capture_automated_;
    if (capture_step_ < 0)
    {
        view.state = "cancelled";
        view.message = "Browser capture cancelled.";
        return result;
    }
    ++capture_step_;
    if (capture_step_ < capture_steps_)
    {
        view.state = "watching";
        view.active = true;
        view.message = capture_automated_ ? "Browser opened. Waiting for Cloudflare verification..."
                                          : "Browser opened. Waiting for the download URL...";
    }
    else if (capture_step_ == capture_steps_)
    {
        view.state = "queueing";
        view.active = true;
        view.message = capture_automated_ ? "Link resolving is in process. Please wait."
                                          : "Download URL found. Adding it to the queue...";
    }
    else
    {
        view.state = "completed";
        view.message = capture_automated_ ? "Supported download queued." : "Download captured and queued.";
        add_job_locked(capture_title_, "queued", 20.0);
        view.result.queued = true;
        view.result.has_jobs = true;
        view.result.jobs = jobs_;
        view.result.toast = view.message;
        capture_step_ = -1;
    }
    return result;
}

Result<CaptureView> FakeBackend::capture_active()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<CaptureView>();
    Result<CaptureView> result;
    result.status = 200;
    if (capture_step_ >= 0)
    {
        result.value.active = true;
        result.value.session_id = "7";
        result.value.state = "watching";
        result.value.title = capture_title_;
        result.value.message = "Browser opened. Waiting for the download URL...";
    }
    return result;
}

Result<bool> FakeBackend::capture_cancel(std::string_view)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<bool>();
    Result<bool> result;
    result.status = 200;
    result.value = true;
    capture_step_ = -1;
    ++cancelled_;
    return result;
}

Result<Probe> FakeBackend::probe_link(std::string_view url)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<Probe>();
    Result<Probe> result;
    result.status = 200;
    const std::string text(url);
    if (text.rfind("http://", 0) != 0 && text.rfind("https://", 0) != 0)
    {
        result.status = 400;
        result.error = "Enter an http:// or https:// link.";
        return result;
    }
    result.value.url = text;
    result.value.final_url = text;
    const std::size_t slash = text.find_last_of('/');
    result.value.file_name = slash == std::string::npos ? "download.bin" : text.substr(slash + 1);
    if (result.value.file_name.empty())
        result.value.file_name = "download.bin";
    result.value.content_type = "application/octet-stream";
    result.value.length = 1.5 * kGB;
    if (text.find("page") != std::string::npos)
        result.value.rejection = "Link points to a web page, not a direct download.";
    return result;
}

Result<Dispatch> FakeBackend::queue_direct(const Probe &probe, std::string_view file_name)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<Dispatch>();
    Result<Dispatch> result;
    result.status = 200;
    add_job_locked(file_name.empty() ? probe.file_name : std::string(file_name), "queued", 1.5);
    result.value.queued = true;
    result.value.has_jobs = true;
    result.value.jobs = jobs_;
    result.value.toast = "Direct link queued.";
    return result;
}

Result<std::vector<Source>> FakeBackend::source_add_url(std::string_view url)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Source>>();
    Result<std::vector<Source>> result;
    const std::string text(url);
    if (text.find("bad") != std::string::npos)
    {
        result.status = 400;
        result.error = "The catalogue at that address could not be read";
        return result;
    }
    result.status = 200;
    Source s;
    s.id = "src" + std::to_string(next_source_++);
    s.name = "Added source " + std::to_string(next_source_ - 10);
    s.kind = "url";
    s.url = text;
    s.package_count = 12;
    s.status = "ok";
    s.last_refresh_at = 1790100000;
    sources_.push_back(std::move(s));
    result.value = sources_;
    return result;
}

Result<std::vector<Source>> FakeBackend::source_delete(std::string_view source_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Source>>();
    Result<std::vector<Source>> result;
    result.status = 200;
    sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                  [&](const Source &s) { return s.id == source_id && s.deletable; }),
                   sources_.end());
    result.value = sources_;
    return result;
}

Result<std::vector<Source>> FakeBackend::source_refresh(std::string_view source_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Source>>();
    Result<std::vector<Source>> result;
    result.status = 200;
    for (Source &s : sources_)
    {
        if (s.id == source_id)
        {
            s.last_refresh_at += 60;
            s.package_count += 1;
        }
    }
    result.value = sources_;
    return result;
}

Result<std::vector<Source>> FakeBackend::source_enable(std::string_view source_id, bool enabled)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<Source>>();
    Result<std::vector<Source>> result;
    result.status = 200;
    for (Source &s : sources_)
    {
        if (s.id == source_id)
            s.enabled = enabled;
    }
    result.value = sources_;
    return result;
}

Result<DefaultSources> FakeBackend::default_sources(bool)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<DefaultSources>();
    Result<DefaultSources> result;
    result.status = 200;
    DefaultSources &d = result.value;
    d.available = true;
    d.status = "remote";
    d.revision = 4;
    d.onboarding_required = onboarding_;
    d.sources = defaults_;
    for (const DefaultSource &s : defaults_)
        d.has_new = d.has_new || s.is_new;
    return result;
}

Result<std::vector<std::string>> FakeBackend::default_sources_apply(
    std::int64_t, const std::vector<std::string> &ids)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<std::string>>();
    Result<std::vector<std::string>> result;
    result.status = 200;
    for (const std::string &id : ids)
    {
        for (DefaultSource &d : defaults_)
        {
            if (d.id != id || d.configured)
                continue;
            d.configured = true;
            d.enabled = true;
            Source s;
            s.id = "src" + std::to_string(next_source_++);
            s.name = d.name;
            s.kind = "default";
            s.managed = true;
            s.deletable = false;
            s.package_count = d.package_count;
            s.status = "ok";
            s.last_refresh_at = 1790100000;
            sources_.push_back(std::move(s));
        }
    }
    onboarding_ = false;
    return result;
}

Result<bool> FakeBackend::default_sources_seen()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<bool>();
    Result<bool> result;
    result.status = 200;
    result.value = true;
    onboarding_ = false;
    return result;
}

void FakeBackend::set_store_art(bool on)
{
    Lock lock(mutex_);
    store_art_ = on;
}

bool FakeBackend::store_art() const
{
    Lock lock(mutex_);
    return store_art_;
}

int FakeBackend::art_asks(const std::string &key) const
{
    Lock lock(mutex_);
    const auto found = art_asks_.find(key);
    return found == art_asks_.end() ? 0 : found->second;
}

Result<Art> FakeBackend::art(std::string_view key, ArtKind kind)
{
    Result<Art> result;
    const std::string name(key);
    bool poster = false;
    int asks = 0;
    {
        Lock lock(mutex_);
        asks = ++art_asks_[name];
        poster = key.substr(0, 4) == "pkg:" && store_art_;
    }
    // Installed games have art on the console itself. Store posters come from
    // the https art service when it is on (store_art_), else they have none.
    if (key.substr(0, 4) != "app:" && !poster)
    {
        result.status = 404;
        result.error = "no artwork";
        return result;
    }
    if (poster)
    {
        if (hash(name) % 7 == 0)
        {
            result.status = 404;
            result.error = "artwork unavailable";
            return result;
        }
        if (asks <= 2)
        {
            result.status = 202;
            result.value.pending = true;
            return result;
        }
    }
    result.status = 200;
    result.value.content_type = "image/png";
    result.value.bytes = make_art(name, kind != ArtKind::cover);
    return result;
}



// ----------------------------------------------------------------- admin --
// Settings editing, folder browsing, APR Emu, game delete, the file manager.
// Everything is in memory; nothing is real.

namespace
{

std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string name_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string join_path(const std::string &base, const std::string &name)
{
    return base == "/" ? "/" + name : base + "/" + name;
}

} // namespace

void FakeBackend::seed_admin_locked()
{
    settings_ = SettingsView{};
    settings_.download_root = "/data/pegasus-dl/downloads";
    settings_.connections = 16;
    settings_.real_debrid.configured = true;
    settings_.real_debrid.valid = true;
    settings_.real_debrid.premium = true;
    settings_.real_debrid.account_type = "premium";
    settings_.real_debrid.hosters = 120;
    settings_.real_debrid.domains = 300;
    settings_.real_debrid.last_checked_at = 1790050000;
    settings_.real_debrid_configured = true;
    settings_.real_debrid_ready = true;
    settings_.dns_helper_running = true;
    settings_.dns_present = true;
    settings_.dns_available = true;
    settings_.dns_managed = true;
    settings_.dns_primary = "127.0.0.1";
    settings_.range_resume = true;
    settings_.providers = {"BuzzHeavier", "DataNodes", "MediaFire"};
    settings_.queue_persists = true;

    mounts_ = {{"/data", "Internal storage", "ufs", true, 800.0 * kGB, 412.0 * kGB},
               {"/mnt/usb0", "USB storage 1", "exfat", true, 1000.0 * kGB, 640.0 * kGB},
               {"/system", "System", "ufs", false, 8.0 * kGB, 1.0 * kGB},
               // The real console's extended storage (recon/samples/files/mounts.json).
               {"/mnt/ext0", "/mnt/ext0", "ufs", true, 255546310656.0, 21641224192.0}};
    fs_.clear();
    // The console refuses to list /mnt itself; its drives below it list fine.
    unlistable_.clear();
    unlistable_["/mnt"] = true;
    const auto dir = [&](const char *path) { fs_[path] = {true, 0.0}; };
    const auto file = [&](const char *path, double bytes) { fs_[path] = {false, bytes}; };
    dir("/");
    dir("/data");
    dir("/data/pegasus-dl");
    dir("/data/pegasus-dl/downloads");
    dir("/data/pegasus-dl/downloads/Extracted");
    file("/data/pegasus-dl/downloads/Ember Hollow.pkg", 38.4 * kGB);
    file("/data/pegasus-dl/downloads/Neon Courier.part1.rar", 4.0 * kGB);
    file("/data/pegasus-dl/downloads/Locked Archive.rar", 2.0 * kGB);
    file("/data/pegasus-dl/downloads/notes.txt", 2048.0);
    file("/data/pegasus-dl/downloads/Extracted/readme.nfo", 900.0);
    dir("/data/homebrew");
    file("/data/homebrew/tool.ffpkg", 43.0e6);
    dir("/mnt");
    dir("/mnt/usb0");
    dir("/mnt/usb0/Backups");
    file("/mnt/usb0/Backups/save-2026-10.bin", 18.0e6);
    file("/mnt/usb0/Quiet Harbour.pkg", 12.3 * kGB);
    // The live listing of /mnt/ext0/homebrew (recon/samples/files/list_ext0_homebrew.json).
    dir("/mnt/ext0");
    dir("/mnt/ext0/homebrew");
    file("/mnt/ext0/homebrew/PPSA02015.exfat", 170804641792.0);
    file("/mnt/ext0/homebrew/PPSA02530.ffpfsc", 38503055360.0);
    file("/mnt/ext0/homebrew/PPSA06464.exfat", 4127195136.0);
    dir("/system");
    file("/system/boot.cfg", 512.0);
}

Result<SettingsView> FakeBackend::settings_update(const SettingsPatch &patch)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<SettingsView>();
    Result<SettingsView> result;
    if (patch.has_connections && (patch.connections < 1 || patch.connections > 64))
    {
        result.status = 400;
        result.error = "Connections must be a whole number from 1 to 64.";
        return result;
    }
    if (patch.has_root)
    {
        const auto node = fs_.find(patch.root);
        if (node == fs_.end() || !node->second.dir)
        {
            result.status = 400;
            result.error = "That folder does not exist.";
            return result;
        }
        settings_.download_root = patch.root;
        last_root_written_ = patch.root;
    }
    if (patch.has_connections)
        settings_.connections = patch.connections;
    const auto token = [&](DebridView &view, bool &configured, bool &ready, const std::string &value,
                           std::string &remembered)
    {
        remembered = value;
        view = DebridView{};
        configured = !value.empty();
        ready = false;
        if (value.empty())
            return;
        view.configured = true;
        view.valid = value.rfind("good", 0) == 0;
        view.last_checked_at = 1790200000;
        if (view.valid)
        {
            view.premium = true;
            view.account_type = "premium";
            view.hosters = 90;
            view.domains = 210;
            ready = true;
        }
        else
        {
            view.error = "The service did not accept this token.";
        }
    };
    if (patch.has_real_debrid)
        token(settings_.real_debrid, settings_.real_debrid_configured, settings_.real_debrid_ready,
              patch.real_debrid_token, last_rd_token_);
    if (patch.has_torbox)
        token(settings_.torbox, settings_.torbox_configured, settings_.torbox_ready,
              patch.torbox_token, last_tb_token_);
    ++settings_writes_;
    result.status = 200;
    result.value = settings_;
    return result;
}

Result<SettingsView> FakeBackend::debrid_refresh(std::string_view service)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<SettingsView>();
    Result<SettingsView> result;
    DebridView &view = service == "torbox" ? settings_.torbox : settings_.real_debrid;
    if (!view.configured)
    {
        result.status = 400;
        result.error = "No token is set for this service.";
        return result;
    }
    view.last_checked_at += 600;
    result.status = 200;
    result.value = settings_;
    return result;
}

Result<std::vector<StorageMount>> FakeBackend::mounts()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<StorageMount>>();
    Result<std::vector<StorageMount>> result;
    result.status = 200;
    result.value = mounts_;
    return result;
}

FolderListing FakeBackend::list_locked(const std::string &path, bool dirs_only, bool *found) const
{
    FolderListing out;
    out.path = path;
    const auto self = fs_.find(path);
    const auto refused = unlistable_.find(path);
    *found = self != fs_.end() && self->second.dir && (refused == unlistable_.end() || !refused->second);
    if (!*found)
        return out;
    out.writable = true;
    for (const StorageMount &m : mounts_)
    {
        if (path == m.path || path.rfind(m.path + "/", 0) == 0)
            out.writable = m.writable;
    }
    for (const auto &[child, node] : fs_)
    {
        if (child == path || parent_of(child) != path)
            continue;
        if (dirs_only && !node.dir)
            continue;
        FileItem item;
        item.name = name_of(child);
        item.path = child;
        item.directory = node.dir;
        item.size_bytes = node.size;
        item.writable = out.writable;
        item.modified = 1790000000;
        out.entries.push_back(std::move(item));
    }
    std::sort(out.entries.begin(), out.entries.end(),
              [](const FileItem &a, const FileItem &b)
              { return a.directory != b.directory ? a.directory : a.name < b.name; });
    return out;
}

Result<FolderListing> FakeBackend::browse(std::string_view path)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<FolderListing>();
    Result<FolderListing> result;
    bool found = false;
    result.value = list_locked(std::string(path), true, &found);
    if (!found)
    {
        result.status = 404;
        result.error = "That folder is not available.";
        return result;
    }
    result.status = 200;
    return result;
}

Result<bool> FakeBackend::mkdir(std::string_view full_path)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<bool>();
    Result<bool> result;
    const std::string path(full_path);
    mkdir_calls_.push_back(path);
    if (mkdir_fails_)
    {
        result.status = 403;
        result.error = "That location is read-only.";
        return result;
    }
    bool found = false;
    const FolderListing parent = list_locked(parent_of(path), true, &found);
    if (!found || !parent.writable)
    {
        result.status = 403;
        result.error = "That location is read-only.";
        return result;
    }
    if (fs_.count(path) != 0)
    {
        result.status = 409;
        result.error = "A folder with that name already exists.";
        return result;
    }
    fs_[path] = {true, 0.0};
    result.status = 200;
    result.value = true;
    return result;
}

Result<bool> FakeBackend::logs_clear()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<bool>();
    logs_.clear();
    ++logs_cleared_;
    Result<bool> result;
    result.status = 200;
    result.value = true;
    return result;
}

Result<std::vector<AprVersionItem>> FakeBackend::apr_versions()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<AprVersionItem>>();
    Result<std::vector<AprVersionItem>> result;
    if (!apr_manifest_ok_)
    {
        result.status = 503;
        result.error = "APR Emu manifest has too many versions";
        return result;
    }
    result.status = 200;
    result.value = {{"v140", "1.4.0 release (latest)"},
                    {"v132", "1.3.2 release"},
                    {"v130", "1.3.0 release"},
                    {"v140d", "1.4.0 debug"}};
    return result;
}

Result<AdminJob> FakeBackend::apr_update_start(std::string_view title_id,
                                               std::string_view version_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<AdminJob>();
    Result<AdminJob> result;
    for (const InstalledGame &g : installed_)
    {
        if (g.title_id != title_id)
            continue;
        if (!g.apr.present || (!g.apr.can_update && !g.apr.can_reinstall))
        {
            result.status = 409;
            result.error = g.apr.reason.empty() ? "APR Emu cannot be updated for this title."
                                                : g.apr.reason;
            return result;
        }
        apr_jobs_[std::string(title_id)] = {0, std::string(version_id)};
        result.status = 200;
        result.value.job_id = "apr-" + std::string(title_id);
        result.value.title_id = std::string(title_id);
        result.value.status = "starting";
        result.value.detail = "Starting";
        return result;
    }
    result.status = 404;
    result.error = "That title is not installed.";
    return result;
}

Result<AdminJob> FakeBackend::apr_update_status(std::string_view, std::string_view title_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<AdminJob>();
    Result<AdminJob> result;
    const auto it = apr_jobs_.find(std::string(title_id));
    if (it == apr_jobs_.end())
    {
        result.status = 404;
        result.error = "No APR Emu job for that title.";
        return result;
    }
    it->second.first += 1;
    result.status = 200;
    result.value.job_id = "apr-" + std::string(title_id);
    result.value.title_id = std::string(title_id);
    if (it->second.first >= 3)
    {
        result.value.status = "completed";
        result.value.progress = 100;
        result.value.detail = "APR Emu is up to date";
        for (InstalledGame &g : installed_)
        {
            if (g.title_id == title_id)
            {
                g.apr.state = "current";
                g.apr.update_available = false;
                g.apr.can_update = false;
                g.apr.version_text = "1.4.0 release";
            }
        }
        ++apr_updates_;
        apr_jobs_.erase(it);
    }
    else
    {
        result.value.status = "running";
        result.value.progress = it->second.first * 35;
        result.value.detail = "Downloading APR Emu";
    }
    return result;
}

Result<AdminJob> FakeBackend::game_delete(std::string_view title_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<AdminJob>();
    Result<AdminJob> result;
    const bool known = std::any_of(installed_.begin(), installed_.end(),
                                   [&](const InstalledGame &g) { return g.title_id == title_id; });
    if (!known)
    {
        result.status = 404;
        result.error = "That title is not installed.";
        return result;
    }
    delete_jobs_[std::string(title_id)] = 0;
    result.status = 200;
    result.value.job_id = "del-" + std::string(title_id);
    result.value.title_id = std::string(title_id);
    result.value.status = "starting";
    result.value.detail = "Starting delete";
    return result;
}

Result<AdminJob> FakeBackend::game_delete_status(std::string_view, std::string_view title_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<AdminJob>();
    Result<AdminJob> result;
    const auto it = delete_jobs_.find(std::string(title_id));
    if (it == delete_jobs_.end())
    {
        result.status = 404;
        result.error = "No delete job for that title.";
        return result;
    }
    it->second += 1;
    result.status = 200;
    result.value.job_id = "del-" + std::string(title_id);
    result.value.title_id = std::string(title_id);
    if (it->second >= 3)
    {
        result.value.status = "completed";
        result.value.files_deleted = 1842;
        installed_.erase(std::remove_if(installed_.begin(), installed_.end(),
                                        [&](const InstalledGame &g)
                                        { return g.title_id == title_id; }),
                         installed_.end());
        deleted_.push_back(std::string(title_id));
        delete_jobs_.erase(it);
    }
    else
    {
        result.value.status = "running";
        result.value.files_deleted = it->second * 600;
        result.value.detail = "Deleting files";
    }
    return result;
}

Result<std::vector<StorageMount>> FakeBackend::file_locations()
{
    return mounts();
}

Result<FolderListing> FakeBackend::file_list(std::string_view path)
{
    std::pair<int, bool> slow{0, false};
    {
        Lock lock(mutex_);
        ++file_list_calls_;
        const auto it = slow_lists_.find(std::string(path));
        if (it != slow_lists_.end())
            slow = it->second;
    }
    // A busy Pegasus: the answer takes a while (the lock is not held meanwhile).
    if (slow.first > 0)
        usleep(static_cast<useconds_t>(slow.first) * 1000u);
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<FolderListing>();
    Result<FolderListing> result;
    if (slow.second)
    {
        result.status = 504;
        result.error = "timed out after 8 s";
        return result;
    }
    bool found = false;
    result.value = list_locked(std::string(path), false, &found);
    if (!found)
    {
        result.status = 404;
        result.error = "That folder is not available.";
        return result;
    }
    result.status = 200;
    return result;
}

Result<std::vector<FileOp>> FakeBackend::file_operations()
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<FileOp>>();
    Result<std::vector<FileOp>> result;
    if (ops_failing_)
    {
        result.status = 504;
        result.error = "timed out after 8 s";
        return result;
    }
    result.status = 200;
    const double now = now_seconds();
    for (FileOp &op : ops_)
    {
        // A cancel that Pegasus takes its time to honour.
        const auto late = cancel_at_.find(op.id);
        if (late != cancel_at_.end() && op.active() && now >= late->second)
        {
            op.status = "cancelled";
            cancel_at_.erase(late);
            continue;
        }
        if (frozen_ops_.count(op.id) != 0)
            continue;
        if (op.active() && op.status != "waiting_password")
        {
            int &age = op_age_[op.id];
            ++age;
            op.done_items = std::min(op.total_items, static_cast<std::int64_t>(age));
            op.done_bytes = op.total_bytes * static_cast<double>(op.done_items) /
                            static_cast<double>(std::max<std::int64_t>(1, op.total_items));
            if (age >= 3)
            {
                // A pkg-install ends "accepted" (handed to the installer), like Pegasus.
                op.status = op.type == "pkg-install" ? "accepted" : "completed";
                op.done_items = op.total_items;
                op.done_bytes = op.total_bytes;
            }
        }
    }
    result.value = ops_;
    return result;
}

FileOp &FakeBackend::add_op_locked(const char *type, const std::string &source,
                                   const std::string &destination, double bytes)
{
    FileOp op;
    op.id = std::to_string(next_op_++);
    op.type = type;
    op.status = "running";
    op.source = source;
    op.destination = destination;
    op.total_items = 3;
    op.total_bytes = bytes;
    ops_.push_back(std::move(op));
    return ops_.back();
}

Result<FileItem> FakeBackend::file_mkdir(std::string_view parent, std::string_view name)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<FileItem>();
    Result<FileItem> result;
    bool found = false;
    const FolderListing list = list_locked(std::string(parent), false, &found);
    if (!found || !list.writable)
    {
        result.status = 403;
        result.error = "That location is read-only.";
        return result;
    }
    const std::string path = join_path(std::string(parent), std::string(name));
    if (fs_.count(path) != 0)
    {
        result.status = 409;
        result.error = "Something with that name already exists.";
        return result;
    }
    fs_[path] = {true, 0.0};
    result.status = 200;
    result.value.name = std::string(name);
    result.value.path = path;
    result.value.directory = true;
    return result;
}

Result<FileItem> FakeBackend::file_rename(std::string_view path, std::string_view name)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<FileItem>();
    Result<FileItem> result;
    if (name.empty() || name.find('/') != std::string_view::npos || name == "." || name == "..")
    {
        result.status = 400;
        result.error = "Enter a name without slashes.";
        return result;
    }
    const std::string from(path);
    const auto it = fs_.find(from);
    const std::string to = join_path(parent_of(from), std::string(name));
    if (it == fs_.end())
    {
        result.status = 404;
        result.error = "That item no longer exists.";
        return result;
    }
    if (fs_.count(to) != 0)
    {
        result.status = 409;
        result.error = "Something with that name already exists.";
        return result;
    }
    // Move the node and everything below it.
    std::vector<std::pair<std::string, FsNode>> moved;
    for (const auto &[p, node] : fs_)
    {
        if (p == from || p.rfind(from + "/", 0) == 0)
            moved.push_back({to + p.substr(from.size()), node});
    }
    for (auto e = fs_.begin(); e != fs_.end();)
        e = (e->first == from || e->first.rfind(from + "/", 0) == 0) ? fs_.erase(e) : std::next(e);
    for (auto &[p, node] : moved)
        fs_[p] = node;
    result.status = 200;
    result.value.name = std::string(name);
    result.value.path = to;
    result.value.directory = fs_[to].dir;
    return result;
}

Result<std::vector<FileOp>> FakeBackend::file_delete(const std::vector<std::string> &paths)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<FileOp>>();
    Result<std::vector<FileOp>> result;
    for (const std::string &p : paths)
    {
        for (auto e = fs_.begin(); e != fs_.end();)
            e = (e->first == p || e->first.rfind(p + "/", 0) == 0) ? fs_.erase(e) : std::next(e);
        deleted_paths_.push_back(p);
    }
    add_op_locked("delete", paths.empty() ? "" : paths.front(), "", 0.0);
    result.status = 200;
    result.value = ops_;
    return result;
}

Result<std::vector<FileOp>> FakeBackend::copy_or_move(const std::vector<std::string> &paths,
                                                      const std::string &destination,
                                                      const std::string &collision, bool move,
                                                      const char *type)
{
    Result<std::vector<FileOp>> result;
    bool found = false;
    const FolderListing target = list_locked(destination, false, &found);
    if (!found || !target.writable)
    {
        result.status = 403;
        result.error = "The destination is read-only or missing.";
        return result;
    }
    for (const std::string &p : paths)
    {
        const auto it = fs_.find(p);
        if (it == fs_.end())
            continue;
        std::string to = join_path(destination, name_of(p));
        if (fs_.count(to) != 0)
        {
            if (collision == "skip")
                continue;
            if (collision == "keep-both")
                to = join_path(destination, name_of(p) + " (2)");
        }
        std::vector<std::pair<std::string, FsNode>> items;
        for (const auto &[q, node] : fs_)
        {
            if (q == p || q.rfind(p + "/", 0) == 0)
                items.push_back({to + q.substr(p.size()), node});
        }
        if (move)
        {
            for (auto e = fs_.begin(); e != fs_.end();)
                e = (e->first == p || e->first.rfind(p + "/", 0) == 0) ? fs_.erase(e) : std::next(e);
        }
        for (auto &[q, node] : items)
            fs_[q] = node;
    }
    add_op_locked(type, paths.empty() ? "" : paths.front(), destination, 1.0e9);
    last_collision_ = collision;
    result.status = 200;
    result.value = ops_;
    return result;
}

Result<std::vector<FileOp>> FakeBackend::file_copy(const std::vector<std::string> &paths,
                                                   std::string_view destination,
                                                   std::string_view collision)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<FileOp>>();
    return copy_or_move(paths, std::string(destination), std::string(collision), false, "copy");
}

Result<std::vector<FileOp>> FakeBackend::file_move(const std::vector<std::string> &paths,
                                                   std::string_view destination,
                                                   std::string_view collision)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<FileOp>>();
    return copy_or_move(paths, std::string(destination), std::string(collision), true, "move");
}

Result<std::vector<FileOp>> FakeBackend::file_extract(const std::vector<std::string> &paths,
                                                      std::string_view destination,
                                                      std::string_view collision)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<FileOp>>();
    Result<std::vector<FileOp>> result;
    for (const std::string &p : paths)
    {
        FileOp &op = add_op_locked("extract", p, std::string(destination), 2.0e9);
        if (name_of(p).find("Locked") != std::string::npos)
            op.status = "waiting_password"; // as Pegasus spells it
        else
            fs_[join_path(std::string(destination), name_of(p) + " (extracted)")] = {true, 0.0};
    }
    last_collision_ = std::string(collision);
    result.status = 200;
    result.value = ops_;
    return result;
}

Result<std::vector<FileOp>> FakeBackend::file_extract_password(std::string_view operation_id,
                                                               std::string_view password)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<FileOp>>();
    Result<std::vector<FileOp>> result;
    for (FileOp &op : ops_)
    {
        if (op.id != operation_id)
            continue;
        if (password != "secret")
        {
            // Pegasus keeps the operation waiting, with the reason on it.
            op.error = "Wrong archive password";
            result.status = 200;
            result.value = ops_;
            return result;
        }
        op.error.clear();
        op.status = "running";
        fs_[join_path(op.destination, name_of(op.source) + " (extracted)")] = {true, 0.0};
    }
    result.status = 200;
    result.value = ops_;
    return result;
}

Result<std::vector<FileOp>> FakeBackend::file_pkg_install(std::string_view path)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<std::vector<FileOp>>();
    Result<std::vector<FileOp>> result;
    FileOp &op = add_op_locked("pkg-install", std::string(path), "", 1.0e9);
    op.status = "installing";
    installs_.push_back(std::string(path));
    result.status = 200;
    result.value = ops_;
    return result;
}

Result<bool> FakeBackend::file_cancel(std::string_view operation_id)
{
    Lock lock(mutex_);
    if (mode_ == FakeMode::offline)
        return refuse<bool>();
    Result<bool> result;
    for (FileOp &op : ops_)
    {
        if (op.id == operation_id && op.active())
        {
            if (!op.cancelable)
            {
                result.status = 409;
                result.error = "This operation cannot be cancelled.";
                return result;
            }
            ++cancelled_ops_;
            const auto delay = cancel_delay_.find(op.id);
            if (delay != cancel_delay_.end() && delay->second > 0.0)
            {
                op.cancel_requested = true;
                cancel_at_[op.id] = now_seconds() + delay->second;
            }
            else
            {
                op.status = "cancelled";
            }
        }
    }
    result.status = 200;
    result.value = true;
    return result;
}

void FakeBackend::set_unlistable(const std::string &path, bool unlistable)
{
    Lock lock(mutex_);
    unlistable_[path] = unlistable;
}

void FakeBackend::set_slow_listing(const std::string &path, int milliseconds, bool timeout)
{
    Lock lock(mutex_);
    if (milliseconds <= 0 && !timeout)
        slow_lists_.erase(path);
    else
        slow_lists_[path] = {milliseconds, timeout};
}

std::string FakeBackend::add_file_op(FileOp op, bool frozen, double cancel_delay_s)
{
    Lock lock(mutex_);
    if (op.id.empty())
        op.id = std::to_string(next_op_++);
    if (frozen)
        frozen_ops_[op.id] = true;
    if (cancel_delay_s > 0.0)
        cancel_delay_[op.id] = cancel_delay_s;
    const std::string id = op.id;
    ops_.push_back(std::move(op));
    return id;
}

void FakeBackend::set_ops_failing(bool failing)
{
    Lock lock(mutex_);
    ops_failing_ = failing;
}

int FakeBackend::file_list_calls() const
{
    Lock lock(mutex_);
    return file_list_calls_;
}

void FakeBackend::set_apr_manifest_ok(bool ok)
{
    Lock lock(mutex_);
    apr_manifest_ok_ = ok;
}

std::vector<std::string> FakeBackend::deleted_games() const
{
    Lock lock(mutex_);
    return deleted_;
}

std::vector<std::string> FakeBackend::deleted_paths() const
{
    Lock lock(mutex_);
    return deleted_paths_;
}

std::vector<std::string> FakeBackend::pkg_installs() const
{
    Lock lock(mutex_);
    return installs_;
}

std::string FakeBackend::last_token(bool torbox) const
{
    Lock lock(mutex_);
    return torbox ? last_tb_token_ : last_rd_token_;
}

int FakeBackend::settings_reads() const
{
    Lock lock(mutex_);
    return settings_reads_;
}

void FakeBackend::set_download_root(const std::string &root)
{
    Lock lock(mutex_);
    settings_.download_root = root;
}

std::vector<std::string> FakeBackend::mkdir_calls() const
{
    Lock lock(mutex_);
    return mkdir_calls_;
}

void FakeBackend::set_mkdir_fails(bool fails)
{
    Lock lock(mutex_);
    mkdir_fails_ = fails;
}

std::string FakeBackend::last_root_written() const
{
    Lock lock(mutex_);
    return last_root_written_;
}

int FakeBackend::settings_writes() const
{
    Lock lock(mutex_);
    return settings_writes_;
}

int FakeBackend::logs_cleared() const
{
    Lock lock(mutex_);
    return logs_cleared_;
}

int FakeBackend::apr_updates() const
{
    Lock lock(mutex_);
    return apr_updates_;
}

int FakeBackend::cancelled_ops() const
{
    Lock lock(mutex_);
    return cancelled_ops_;
}

std::string FakeBackend::last_collision() const
{
    Lock lock(mutex_);
    return last_collision_;
}

bool FakeBackend::path_exists(const std::string &path) const
{
    Lock lock(mutex_);
    return fs_.count(path) != 0;
}

} // namespace orbit::host
