// Wings TV app - Console entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and sound, looks for Pegasus DL on this
// console's loopback address, then runs the shell every frame:
// read input, update, play the sounds it asked for, draw, present. Adapted
// from ps5-homebrew-ui's src/main.cpp.

#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/touch_keyboard.hpp"
#include "core/version.hpp"
#include "gfx/renderer.hpp"
#include "orbit/art.hpp"
#include "orbit/cover_art.hpp"
#include "orbit/pegasus_manager.hpp"
#include "orbit/popularity.hpp"
#include "orbit/steam_reviews.hpp"
#include "orbit/wings_update.hpp"
#include "orbit/prefs.hpp"
#include "orbit/diagnostics.hpp"
#include "orbit/gl_textures.hpp"
#include "orbit/http.hpp"
#include "orbit/i18n.hpp"
#include "orbit/mark.hpp"
#include "orbit/screens.hpp"
#include "orbit/shadowmount_scan.hpp"
#include "orbit/core_transport.hpp"
#include "orbit/pegasus_backend.hpp"
#include "orbit/store.hpp"
#include "orbit/ui.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"
#include "wings/art/art_service.hpp"
#include "wings/art/orbit_art_source.hpp"

#include <GL/glcorearb.h>
#include <openssl/rand.h>

#include <time.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" int sceSystemServiceLaunchWebBrowser(const char *uri, void *parameters);
extern "C" int sceSystemServiceParamGetInt(int param_id, int *value);
// src/runtime/app_heap.c
extern "C" void hui_heap_stats(std::size_t *live, std::size_t *peak, std::size_t *blocks,
                                std::size_t *failures);

namespace
{

constexpr const char *kAssets = "/app0/assets";
constexpr const char *kBackend = "127.0.0.1";
constexpr std::uint16_t kPort = 6970; // Pegasus DL
constexpr const char *kEndpoint = "127.0.0.1:6970";
constexpr const char *kBrowserUrl = "http://127.0.0.1:6970/";
constexpr const char *kTitleId = "PPSA99178";

// Cover art. The cache and the preferences live in the title's own storage
// (/download0 is where the title log already goes). Diagnostics are also put in
// /data/wings when that is writable, where Pegasus DL's file manager sees them.
constexpr const char *kWingsDir = "/download0/wings";
constexpr const char *kArtCacheDir = "/download0/wings/art";
constexpr const char *kCaOverride = "/download0/wings/ca-bundle.pem"; // tried before the shipped one
constexpr const char *kCaShipped = "/app0/assets/ca-bundle.crt";

// The Store grid's sort, filters and favourites (same container and places as
// the preferences).
std::vector<std::string> catalogue_paths()
{
    return {std::string(kWingsDir) + "/catalogue.v1", "/data/wings/catalogue.v1"};
}

// Wings' own link-ranking and popularity preferences (never sent to Pegasus DL).
std::vector<std::string> wings_prefs_paths()
{
    return {std::string(kWingsDir) + "/linkprefs.v1", "/data/wings/linkprefs.v1"};
}

std::vector<std::string> prefs_paths()
{
    return {std::string(kWingsDir) + "/settings.v1", "/data/wings/settings.v1"};
}

std::vector<std::string> diagnostics_paths()
{
    return {"/data/wings/diagnostics.txt", std::string(kWingsDir) + "/diagnostics.txt"};
}

std::int64_t wall_clock_seconds()
{
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return 0;
    return static_cast<std::int64_t>(now.tv_sec);
}

const char *phase_name(wings::art::Phase phase)
{
    using wings::art::Phase;
    switch (phase)
    {
    case Phase::stopped:
        return "stopped";
    case Phase::disabled_dns:
        return "off (DNS-over-HTTPS is not enabled)";
    case Phase::ca_error:
        return "ERROR: no usable certificate bundle";
    case Phase::entropy_error:
        return "ERROR: no random number source";
    case Phase::offline:
        return "offline (name server not reachable)";
    case Phase::paused_heap:
        return "paused (app is short of memory)";
    case Phase::ready:
        return "ready";
    }
    return "unknown";
}

// Creates the folder and proves a file can be written there. Answers the
// "cache dir writable" line of the start-up diagnostics.
bool folder_writable(const std::string &dir)
{
    hui::save::ensure_directory(kWingsDir);
    hui::save::ensure_directory(dir);
    const std::string probe = dir + "/.write-test";
    std::FILE *file = std::fopen(probe.c_str(), "wb");
    if (file == nullptr)
        return false;
    const bool ok = std::fputs("ok\n", file) >= 0;
    std::fclose(file);
    ::unlink(probe.c_str());
    return ok;
}

// Popularity data: one https GET through the art service's TLS client (worker thread).
class ConsoleDocFetcher final : public orbit::DocFetcher
{
  public:
    explicit ConsoleDocFetcher(wings::art::ArtService &service) : service_(service) {}
    orbit::DocResult get(const std::string &url, std::size_t max_bytes) override
    {
        const wings::art::ArtService::Document doc = service_.fetch_document(url, max_bytes);
        orbit::DocResult out;
        out.status = doc.status;
        out.body = doc.body;
        out.error = doc.error;
        if (doc.time_error)
            out.error += (out.error.empty() ? "" : " ") + std::string("(check the console clock)");
        hui::sys::log("[WINGS-POP] GET status=%d bytes=%zu %s", doc.status, doc.body.size(), out.error.c_str());
        return out;
    }

  private:
    wings::art::ArtService &service_;
};

// Pegasus DL release download: the art service's TLS client and the same network gate as cover art.
class ConsolePegasusDownloader final : public orbit::PegasusDownloader
{
  public:
    explicit ConsolePegasusDownloader(wings::art::ArtService &service) : service_(service) {}
    bool ready(std::string *why) override
    {
        const wings::art::ServiceState state = service_.state();
        if (state.phase == wings::art::Phase::ready)
            return true;
        *why = state.detail.empty() ? std::string("The network is off. Turn on cover downloads first.")
                                    : state.detail;
        return false;
    }
    orbit::PegasusDownloadResult get(const std::string &url, std::size_t max_bytes,
                                     const orbit::PegasusDownloadHooks &hooks) override
    {
        wings::art::ArtService::DownloadHooks h;
        h.redirect_ok = hooks.redirect_ok;
        h.progress = hooks.progress;
        h.cancel = hooks.cancel;
        wings::art::ArtService::Download d = service_.fetch_download(url, max_bytes, h);
        orbit::PegasusDownloadResult out;
        out.status = d.status;
        out.body = std::move(d.body);
        out.error = d.error;
        if (d.time_error)
            out.error += (out.error.empty() ? "" : " ") + std::string("(check the console clock)");
        hui::sys::log("[WINGS-PEGASUS] GET status=%d bytes=%zu %s", d.status, out.body.size(), out.error.c_str());
        return out;
    }

  private:
    wings::art::ArtService &service_;
};

// Settings > Cover art on top of the art service.
class ConsoleCoverArt final : public orbit::CoverArtControl
{
  public:
    ConsoleCoverArt(wings::art::ArtService &service, orbit::WingsPrefs prefs)
        : service_(service), prefs_(prefs)
    {
    }
    // The ArtCache whose failed covers are asked for again after a change.
    void attach(orbit::ArtCache *cache)
    {
        cache_ = cache;
    }
    orbit::CoverArtStatus status() override
    {
        const wings::art::ServiceState state = service_.state();
        orbit::CoverArtStatus out;
        out.available = true;
        out.doh = service_.dns_mode() == wings::art::DnsMode::doh;
        out.working = state.phase == wings::art::Phase::ready;
        out.detail = state.detail;
        return out;
    }
    void set_doh(bool on) override
    {
        service_.set_dns_mode(on ? wings::art::DnsMode::doh : wings::art::DnsMode::off);
        prefs_.art_doh = on;
        const int saved = orbit::save_prefs(prefs_paths(), prefs_);
        hui::sys::log("[WINGS-ART] DNS-over-HTTPS %s by the player (saved to %d file(s))",
                      on ? "turned ON" : "turned OFF", saved);
        // Tiles that failed while it was off should not wait out the retry timer.
        if (cache_ != nullptr)
            cache_->retry_failed();
        changed_ = true;
    }
    void clear_cache() override
    {
        service_.clear_cache();
        hui::sys::log("[WINGS-ART] art cache cleared by the player");
        changed_ = true;
    }
    void retry_failed() override
    {
        service_.retry_failed();
        if (cache_ != nullptr)
            cache_->retry_failed();
        hui::sys::log("[WINGS-ART] retry failed covers requested by the player");
        changed_ = true;
    }
    // True once after the player changed something (the diagnostics are rewritten).
    bool take_changed()
    {
        const bool was = changed_;
        changed_ = false;
        return was;
    }

  private:
    wings::art::ArtService &service_;
    orbit::WingsPrefs prefs_;
    orbit::ArtCache *cache_ = nullptr;
    bool changed_ = false;
};

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[WINGS] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[WINGS] entry");

    // 4K when the OpenGL runtime can choose its size, else its fixed profile.
    ps5::Display display;
    const bool modes = ps5::Display::supports_display_modes();
    if (!(modes && display.open(3840, 2160)) && !display.open(1920, 1080))
    {
        sys::log("[WINGS] fatal: display open failed");
        sys::park();
    }
    sys::log("[WINGS] display %dx%d", display.width(), display.height());

    gfx::Renderer renderer;
    gfx::Font light;
    gfx::Font regular;
    gfx::Font medium;
    gfx::Font semibold;
    orbit::ui::Type type;
    if (!renderer.init() ||
        !load_font(renderer, "inter-display-light.huifont", &light, &type.light) ||
        !load_font(renderer, "inter-regular.huifont", &regular, &type.regular) ||
        !load_font(renderer, "inter-medium.huifont", &medium, &type.medium) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &type.semibold))
    {
        sys::log("[WINGS] fatal: renderer init failed");
        sys::park();
    }
    // The kit's six slots, for controller glyphs and the on-screen keyboard.
    ui::Fonts fonts;
    fonts.regular = type.regular;
    fonts.semibold = type.semibold;
    fonts.display = type.light;
    fonts.mono = type.medium;
    fonts.pixel = type.regular;
    fonts.hand = type.regular;

    orbit::GlTextures textures(renderer.batch());
    const std::uint32_t mark = textures.create(orbit::mark::render(128));
    const std::uint32_t loader_mark = textures.create(orbit::mark::render(256, 0.32f));

    ps5::Pad pad;
    pad.open();
    pad.set_light_bar(0x2a, 0x9d, 0xf4);
    InputTracker tracker;
    audio::Mixer mixer;
    mixer.set_bus_gain(audio::Bus::ui, 0.7f);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[WINGS] sounds files=%d rejected=%d", bank.files, bank.rejected);

    // Pegasus DL through the adapter library in wings/core. Wings only waits for
    // Pegasus: the backend connects (and reconnects) by itself whenever it answers.
    orbit::http::SocketTransport transport(kBackend, kPort);
    auto core_transport = std::make_shared<orbit::CoreTransport>(transport);
    orbit::PegasusBackend backend(core_transport, kEndpoint);
    // The console's own language, before anything shows or reports text.
    // SCE_SYSTEM_SERVICE_PARAM_ID_LANG is 1; English when there is no catalogue.
    int system_language = 1;
    if (sceSystemServiceParamGetInt(1, &system_language) != 0)
        system_language = 1;
    const std::string language(orbit::i18n::from_system(system_language));
    if (!orbit::i18n::load(std::string(kAssets) + "/i18n", language))
        hui::sys::log("[WINGS] language %s unavailable; using English", language.c_str());
    hui::sys::log("[WINGS] language=%s system=%d", orbit::i18n::language().data(), system_language);

    orbit::Store store(backend);

    // ---- Cover art (wings/art): https posters, fetched by the app itself ----
    // Start-up facts first, so the very first console run is informative even
    // if everything after this fails.
    const std::int64_t boot_clock = wall_clock_seconds();
    sys::log("[WINGS-ART] clock %s UTC plausible=%d", orbit::format_utc(boot_clock).c_str(),
             orbit::clock_plausible(boot_clock) ? 1 : 0);
    int rand_status = RAND_status();
    sys::log("[WINGS-ART] RAND_status=%d (before start)", rand_status);
    if (rand_status != 1)
    {
        // Best effort top-up from the kernel; the service refuses HTTPS if it stays 0.
        if (std::FILE *urandom = std::fopen("/dev/urandom", "rb"))
        {
            unsigned char seed[64];
            const std::size_t got = std::fread(seed, 1, sizeof(seed), urandom);
            std::fclose(urandom);
            if (got > 0)
                RAND_add(seed, static_cast<int>(got), static_cast<double>(got));
            rand_status = RAND_status();
            sys::log("[WINGS-ART] seeded from /dev/urandom (%zu bytes): RAND_status=%d", got,
                     rand_status);
        }
        else
        {
            sys::log("[WINGS-ART] /dev/urandom unavailable");
        }
    }
    const bool cache_writable = folder_writable(kArtCacheDir);
    sys::log("[WINGS-ART] cache dir %s writable=%d", kArtCacheDir, cache_writable ? 1 : 0);

    // DNS-over-HTTPS is on by default; a saved choice of off is kept.
    orbit::WingsPrefs prefs;
    const bool prefs_loaded = orbit::load_prefs(prefs_paths(), &prefs);
    sys::log("[WINGS-ART] preferences %s: DNS-over-HTTPS %s", prefs_loaded ? "loaded" : "default",
             prefs.art_doh ? "ON" : "off");

    orbit::ProblemLog problems;
    wings::art::ArtConfig art_config;
    art_config.cache_root = kArtCacheDir;
    art_config.ca_bundle_paths = {kCaOverride, kCaShipped};
    art_config.dns_mode = prefs.art_doh ? wings::art::DnsMode::doh : wings::art::DnsMode::off;
    art_config.heap_used = []
    {
        std::size_t live = 0;
        hui_heap_stats(&live, nullptr, nullptr, nullptr);
        return static_cast<std::uint64_t>(live);
    };
    art_config.log = [&problems](int level, const std::string &line)
    {
        hui::sys::log("%s", line.c_str());
        if (level <= 1)
            problems.add(line);
    };
    wings::art::ArtService art_service(art_config);
    std::string art_problem;
    const bool art_started = art_service.start(&art_problem);
    {
        const wings::art::ServiceState st = art_service.state();
        sys::log("[WINGS-ART] start=%d phase=%s ca=%d (%s) disk_ok=%d dns=%s", art_started ? 1 : 0,
                 phase_name(st.phase), st.ca_certs, st.ca_source.c_str(), st.disk_ok ? 1 : 0,
                 wings::art::name(art_service.dns_mode()));
        sys::log("[WINGS-ART] %s %s", st.detail.c_str(), art_problem.c_str());
    }
    ConsoleCoverArt cover_art(art_service, prefs);

    // Popularity data: off unless the Wings preference is on; then a background fetch (never
    // blocks the frame). The saved copy and the local download counter live beside the art cache.
    ConsoleDocFetcher doc_fetcher(art_service);
    // Which Wings this is (param.json) and which build of it (assets/build-info.json, written by the build).
    // Saved data is tagged with the build, so a new build fetches fresh data on its first start.
    const std::string app_content_version = read_content_version("/app0/sce_sys/param.json");
    const BuildInfo app_build = read_build_info(std::string(kAssets) + "/build-info.json");
    orbit::PopularityServiceConfig pop_config;
    pop_config.cache_paths = {std::string(kWingsDir) + "/popularity.v1"};
    pop_config.app_tag = build_tag(app_content_version, app_build);
    orbit::PopularityService popularity(orbit::popularity_hub(), doc_fetcher, pop_config);
    orbit::popularity_hub().apply_hook = [&popularity](bool on, const std::string &url)
    { popularity.apply(on, url); };
    orbit::popularity_hub().refresh_hook = [&popularity] { popularity.refresh(); };
    orbit::popularity_hub().set_downloads_paths({std::string(kWingsDir) + "/downloads.v1"});
    // Pegasus DL on this console: the release list on demand (6 h cache), a download only on a confirm.
    ConsolePegasusDownloader pegasus_downloader(art_service);
    {
        orbit::PegasusManagerConfig pm;
        pm.docs = &doc_fetcher;
        pm.downloader = &pegasus_downloader;
        pm.cache_paths = {std::string(kWingsDir) + "/pegasus_releases.v1", "/data/wings/pegasus_releases.v1"};
        orbit::pegasus_manager().attach(std::move(pm));
    }
    // A newer Wings: a look at the project's latest release once the network is ready (wings_update.hpp).
    {
        orbit::WingsUpdateConfig wu;
        wu.docs = &doc_fetcher;
        wu.ready = [&art_service](std::string *why)
        {
            const wings::art::ServiceState state = art_service.state();
            if (state.phase == wings::art::Phase::ready)
                return true;
            *why = state.detail.empty() ? std::string("The network is off. Turn on cover downloads first.")
                                        : state.detail;
            return false;
        };
        wu.cache_paths = {std::string(kWingsDir) + "/wings_update.v1", "/data/wings/wings_update.v1"};
        wu.running = app_content_version;
        orbit::wings_update().attach(std::move(wu));
    }
    struct UpdateStop
    {
        ~UpdateStop()
        {
            orbit::wings_update().wait_idle();
        }
    } update_stop;
    // The worker uses the downloader above: stop it before that goes away.
    struct PegasusStop
    {
        ~PegasusStop()
        {
            orbit::pegasus_manager().cancel();
            orbit::pegasus_manager().wait_idle();
        }
    } pegasus_stop;
    // Live Steam review fallback for games the index has not seen (game page only, opt-in with popularity).
    orbit::SteamReviewConfig steam_config;
    steam_config.cache_paths = {std::string(kWingsDir) + "/steam_reviews.v1"};
    orbit::SteamReviewService steam_reviews(orbit::popularity_hub(), doc_fetcher, steam_config);
    orbit::set_steam_reviews(&steam_reviews);
    {
        orbit::WingsLocalPrefs local;
        orbit::load_local_prefs(wings_prefs_paths(), &local);
        orbit::wings_update().set_enabled(local.update_check);
        popularity.apply(local.popularity_enabled, local.popularity_url);
    }

    // Package id -> poster URL, refreshed from the Store's snapshot in the frame
    // loop (the resolver runs on the art worker thread). Anything it does not
    // know (the "app:" keys of installed games) goes to the backend, which
    // serves /appdb/icon.
    orbit::PosterIndex posters;
    wings::art::OrbitArtSource art_source(
        art_service,
        [&posters](std::string_view key, orbit::ArtKind kind)
        { return posters.resolve(key, kind); },
        &backend);

    orbit::ArtCache art(art_source, textures);
    cover_art.attach(&art);
    store.start();
    art.start();

    orbit::Context context(type, fonts, store, art);
    context.cover_art = &cover_art;
    context.posters = &posters;
    context.catalogue_paths = catalogue_paths();
    context.wings_prefs_paths = wings_prefs_paths();
    // Files tab diagnostics (request ids, drops, errors) go to the title log.
    context.log_line = [](const std::string &line) { sys::log("[WINGS-FILES] %s", line.c_str()); };
    context.prefetch_art = [&art_source](const std::vector<std::string> &keys)
    { art_source.prefetch_keys(keys, orbit::ArtKind::cover); };
    // The focused tile's 16:9 picture, into the disk cache only (the background lane: never ahead of what is on screen).
    context.prefetch_hero = [&art_service](const std::string &url)
    {
        const std::size_t queued = art_service.prefetch({url}, wings::art::Kind::hero, wings::art::Lane::prefetch);
        hui::sys::log("[WINGS-ART] hero prefetch %s", queued > 0 ? "queued" : "skipped (cached or not allowed)");
    };
    context.mark = mark;
    context.loader_mark = loader_mark;
    context.app_version = app_content_version;
    context.app_label = version_label(app_content_version, app_build);
    context.open_pegasus_ui = []
    { return sceSystemServiceLaunchWebBrowser(kBrowserUrl, nullptr) == 0; };
    // ShadowMount's local API (loopback only): after a download finishes, Wings asks it to scan.
    // Wings is mounted by ShadowMount, so it waits until Wings closes; Wings never closes itself.
    orbit::ThreadRunner shadowmount_runner;
    context.shadowmount.transport = [](std::uint16_t port, int timeout_ms)
    { return std::make_unique<orbit::http::SocketTransport>("127.0.0.1", port, timeout_ms); };
    context.shadowmount.read_config = []
    {
        std::string text;
        if (std::FILE *file = std::fopen("/data/shadowmount/config.ini", "rb"))
        {
            char buffer[2048];
            std::size_t got = 0;
            while (text.size() < 16384 && (got = std::fread(buffer, 1, sizeof buffer, file)) > 0)
                text.append(buffer, got);
            std::fclose(file);
        }
        return text;
    };
    context.shadowmount.run = [&shadowmount_runner](std::function<void()> job)
    { shadowmount_runner(std::move(job)); };
    context.close_app = [] { sys::quit(); };
    orbit::App app(context);
    orbit::Frame frame;
    frame.glass_texture = renderer.glass_texture();
    sys::log("[WINGS] ready version=%s build=%s channel=%s", context.app_version.c_str(),
             app_build.known ? app_build.build.c_str() : "unknown",
             app_build.known ? (app_build.release ? "release" : "test") : "unknown");

    // The diagnostics file: at start, 10 s later (Pegasus and the catalogue have
    // answered by then), then every minute and whenever the player changes a
    // cover art setting.
    const std::int64_t start_us = sys::monotonic_us();
    std::int64_t next_diagnostics_us = start_us;
    int diagnostics_written = 0;
    const auto write_diagnostics = [&](std::int64_t now_us)
    {
        const orbit::Snapshot &snapshot = store.state();
        const wings::art::ServiceState st = art_service.state();
        const wings::art::ArtStats stats = art_service.stats();
        orbit::DiagnosticsInput in;
        in.app_version = context.app_label.empty() ? context.app_version : context.app_label;
        in.app_build = app_build.known ? app_build.build + (app_build.release ? " (release)" : " (test)") : std::string();
        in.title_id = kTitleId;
        in.unix_time = wall_clock_seconds();
        in.uptime_s = static_cast<double>(now_us - start_us) / 1e6;
        in.link = snapshot.link == orbit::Link::online
                      ? "online"
                      : (snapshot.link == orbit::Link::offline ? "waiting for Pegasus DL"
                                                               : "connecting");
        in.endpoint = kEndpoint;
        in.pegasus_version = snapshot.have_info ? snapshot.info.version : std::string();
        in.catalogue_packages =
            snapshot.have_catalog ? static_cast<int>(snapshot.packages.size()) : -1;
        in.poster_urls = static_cast<int>(posters.size());
        in.art_present = true;
        in.art_phase = phase_name(st.phase);
        in.art_detail = st.detail;
        in.dns_mode = wings::art::name(st.dns_mode);
        in.doh_enabled = st.dns_mode == wings::art::DnsMode::doh;
        in.ca_certs = st.ca_certs;
        in.ca_source = st.ca_source;
        in.rand_status = RAND_status();
        in.cache_dir = kArtCacheDir;
        in.cache_dir_writable = cache_writable;
        in.disk_ok = st.disk_ok;
        in.cache_entries = stats.disk.entries;
        in.cache_bytes = stats.disk.bytes;
        in.clock_suspect = st.clock_suspect;
        in.queued = st.queued;
        in.running = st.running;
        in.requests = stats.requests;
        in.disk_hits = stats.disk_hits;
        in.net_ok = stats.net_ok;
        in.net_bytes = stats.net_bytes;
        in.fail_transient = stats.fail_transient;
        in.fail_permanent = stats.fail_permanent;
        in.refused_by_guard = stats.refused_by_guard;
        std::size_t live = 0;
        std::size_t peak = 0;
        hui_heap_stats(&live, &peak, nullptr, nullptr);
        in.touch = tracker.touch_stats();
        in.touch_known = true;
        in.heap_live = live;
        in.heap_peak = peak;
        in.recent_problems = problems.snapshot();
        // First pass lists every candidate; when one cannot be written the text
        // is made again with only the places that worked.
        const std::vector<std::string> candidates = diagnostics_paths();
        in.written_to = candidates;
        std::string text = orbit::format_diagnostics(in);
        context.diagnostics_text = text;
        std::vector<std::string> written;
        for (const std::string &path : candidates)
        {
            hui::save::ensure_directory(path.substr(0, path.rfind('/')));
            if (hui::save::write_atomic(path, text).empty())
                written.push_back(path);
        }
        if (written.size() != candidates.size() && !written.empty())
        {
            in.written_to = written;
            text = orbit::format_diagnostics(in);
            for (const std::string &path : written)
                hui::save::write_atomic(path, text);
        }
        if (diagnostics_written == 0)
        {
            sys::log("[WINGS-ART] diagnostics written to %zu of %zu places%s%s", written.size(),
                     candidates.size(), written.empty() ? "" : ": ",
                     written.empty() ? "" : written[0].c_str());
            sys::log("[WINGS-ART] state: %s | %s", phase_name(st.phase), st.detail.c_str());
        }
        ++diagnostics_written;
    };

    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;
    PadSample samples[64];
    ui::Feedback feedback;
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        apply_keyboard_touch_mode(tracker);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                                static_cast<std::uint64_t>(now));
        feedback.clear();
        app.update(input, dt, feedback);
        {
            // Frame thread: hand the catalogue's poster URLs to the art resolver.
            const orbit::Snapshot &snapshot = store.state();
            if (snapshot.have_catalog)
                posters.sync(snapshot.catalog_serial, snapshot.packages);
            if (cover_art.take_changed() || now >= next_diagnostics_us)
            {
                write_diagnostics(now);
                next_diagnostics_us = now + (diagnostics_written <= 1 ? 10000000 : 60000000);
            }
        }
        for (const audio::CueEvent &event : feedback.cues)
            sounds.play(mixer, audio::SoundSet::glass, event);
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);

        frame.reset();
        app.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass)
            renderer.glass();
        renderer.draw(frame.overlay);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[WINGS] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        if (++frames == 1)
            sys::log("[WINGS] first frame, splash hidden=%d", sys::hide_splash_screen() ? 1 : 0);
    }
}
