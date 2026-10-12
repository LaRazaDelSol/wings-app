// Wings TV app - PC preview: runs the app headless and writes PNG pictures.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: wings_preview <assets dir> <output dir> [scenario|all] [width height]
//
// The same App, Store, ArtCache and drawing code as the console, rendered by
// Mesa's software OpenGL through surfaceless EGL, with a fixed 60 Hz clock.
// Data comes from the fake backend in fake_backend.cpp (invented packages,
// queue jobs and installed games). Each scenario drives the app with scripted
// controller input and takes named pictures. Adapted from Orbit Store's
// host/preview_main.cpp (GPL-3.0-or-later), itself from ps5-homebrew-ui's
// host/snapshot_main.cpp.

#include "fake_backend.hpp"
#include "fake_cover_art.hpp"

#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "orbit/art.hpp"
#include "orbit/gl_textures.hpp"
#include "orbit/mark.hpp"
#include "orbit/screens.hpp"
#include "orbit/panels.hpp"
#include "orbit/popularity.hpp"
#include "orbit/store.hpp"
#include "orbit/i18n.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include "../third_party/stb/stb_image_write.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

using hui::Action;
using hui::Direction;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::string data;
    char buffer[65536];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        data.append(buffer, n);
    std::fclose(file);
    if (!font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

struct Step
{
    float wait = 0.4f; // simulated seconds before the step acts
    std::uint32_t press = 0;
    Direction nav = Direction::none;
    int real_ms = 0;               // also wait this long in real time (worker threads)
    const char *capture = nullptr; // picture taken after the wait, before the input
    std::function<void(orbit::App &, orbit::host::FakeBackend &)> act;
};

struct Scenario
{
    const char *name;
    orbit::host::FakeMode mode;
    std::vector<Step> steps;
};

Step pause(float seconds, const char *capture = nullptr)
{
    Step step;
    step.wait = seconds;
    step.capture = capture;
    return step;
}

Step move(Direction direction, float seconds = 0.3f)
{
    Step step;
    step.wait = seconds;
    step.nav = direction;
    return step;
}

Step tap(Action action, float seconds = 0.3f)
{
    Step step;
    step.wait = seconds;
    step.press = hui::action_bit(action);
    return step;
}

Step run(float seconds, std::function<void(orbit::App &, orbit::host::FakeBackend &)> act)
{
    Step step;
    step.wait = seconds;
    step.act = std::move(act);
    return step;
}

std::vector<Scenario> scenarios()
{
    using Mode = orbit::host::FakeMode;
    using orbit::App;
    using orbit::host::FakeBackend;
    std::vector<Scenario> list;
    const auto tab = [](int index)
    { return run(1.5f, [index](App &app, FakeBackend &) { app.show_tab(index); }); };
    // Waits in real time (the Store's workers run on real threads).
    const auto wait = [](int milliseconds, const char *capture = nullptr)
    {
        Step step;
        step.wait = 0.1f;
        step.real_ms = milliseconds;
        step.capture = capture;
        return step;
    };
    const auto open_package = [](const char *id)
    {
        return run(0.2f, [id](App &app, FakeBackend &) { app.store_screen().open_package(id); });
    };
    // The game page's card whose first link is this one (package, catalogue index).
    const auto focus_card = [](std::function<bool(const orbit::GameView &, int)> match)
    {
        return run(0.4f,
                   [match](App &app, FakeBackend &)
                   {
                       const orbit::GameView &page = app.store_screen().game_page();
                       for (int m = 0; m < static_cast<int>(page.page.mirrors.size()); ++m)
                           if (match(page, m) && app.store_screen().focus_page_mirror(m))
                               return;
                   });
    };
    const auto focus_link = [focus_card](int index)
    { return focus_card([index](const orbit::GameView &page, int m) { return page.first_ref(m).link_index == index; }); };
    const auto focus_host = [focus_card](const char *host, int parts)
    {
        return focus_card([host, parts](const orbit::GameView &page, int m)
                          {
                              const orbit::Mirror &mirror = page.page.mirrors[static_cast<std::size_t>(m)];
                              return mirror.host == host && mirror.parts() == parts;
                          });
    };
    const auto set_firmware = [](const char *firmware)
    {
        return run(0.2f,
                   [firmware](App &app, FakeBackend &)
                   {
                       orbit::WingsLocalPrefs prefs;
                       prefs.firmware = firmware;
                       app.store_screen().set_prefs(prefs);
                   });
    };

    // Boot flow: the shell shows at once, "Waiting for Pegasus DL" until it answers.
    list.push_back({"boot",
                    Mode::offline,
                    {pause(0.3f, "boot-connecting"), pause(2.5f, "boot-waiting"),
                     run(0.2f, [](App &, FakeBackend &backend) { backend.set_mode(Mode::normal); }),
                     pause(4.0f, "boot-connected")}});

    // First-run "load Pegasus DL" choices: fixed states of the flow (no manager, no network, no clock).
    // The grace period is 10 s of real time, which a preview never reaches, so the flow is built here.
    {
        using orbit::LoadChoice;
        using orbit::LoadFacts;
        using orbit::LoadFail;
        using orbit::PegasusLoadFlow;
        const auto facts = [](double now, bool net)
        {
            LoadFacts f;
            f.now = now;
            f.net_ok = net;
            return f;
        };
        // Offered: unreachable for longer than the grace period.
        const auto offered = [facts](bool net)
        {
            PegasusLoadFlow flow;
            flow.tick(facts(0.0, net));
            flow.tick(facts(orbit::kPegasusOfferGraceS + 1.0, net));
            return flow;
        };
        // Loading: the player chose "once"; `stage` moves it on.
        const auto loading = [offered, facts](void (PegasusLoadFlow::*stage)())
        {
            PegasusLoadFlow flow = offered(true);
            flow.choose(LoadChoice::once, facts(12.0, true));
            (flow.*stage)();
            return flow;
        };
        const auto scene = [](const char *name, std::function<PegasusLoadFlow()> make, std::uint64_t got,
                              std::uint64_t size, bool toast = false)
        {
            return Scenario{name,
                            Mode::offline,
                            {run(0.4f,
                                 [make, got, size, toast](App &app, FakeBackend &)
                                 {
                                     const std::string notice = app.pegasus_offer().preview_show(make(), got, size);
                                     if (toast && !notice.empty())
                                         app.show_toast(notice, 6.0f);
                                 }),
                             pause(0.8f, name)}};
        };
        const std::uint64_t mib = 1024u * 1024u;
        list.push_back(scene("offer-choices", [offered] { return offered(true); }, 0, 0));
        // Inside the grace: the choices are already there, with the spinner and "Looking for" line.
        list.push_back(scene("offer-grace",
                             [facts]
                             {
                                 PegasusLoadFlow flow;
                                 flow.tick(facts(0.0, true));
                                 return flow;
                             },
                             0, 0));
        // A choice pressed inside the grace, before the release list is read: spinner + progress line.
        list.push_back(scene("offer-grace-pressed",
                             [facts]
                             {
                                 PegasusLoadFlow flow;
                                 flow.tick(facts(0.0, true));
                                 flow.choose(LoadChoice::once, facts(2.0, true));
                                 flow.on_downloading();
                                 return flow;
                             },
                             0, 0));
        list.push_back(scene("offer-downloading", [loading] { return loading(&PegasusLoadFlow::on_downloading); },
                             4 * mib + mib / 3, 11 * mib));
        list.push_back(scene("offer-verifying", [loading] { return loading(&PegasusLoadFlow::on_verifying); },
                             11 * mib, 11 * mib));
        list.push_back(scene("offer-sending", [loading] { return loading(&PegasusLoadFlow::on_sending); },
                             11 * mib, 11 * mib));
        list.push_back(scene("offer-refused",
                             [loading]
                             {
                                 PegasusLoadFlow flow = loading(&PegasusLoadFlow::on_sending);
                                 flow.on_failed(LoadFail::no_loader);
                                 return flow;
                             },
                             11 * mib, 11 * mib));
        // The loader refused while the console was still starting: spinner, retried every 3 s.
        list.push_back(scene("offer-loader-starting",
                             [loading]
                             {
                                 PegasusLoadFlow flow = loading(&PegasusLoadFlow::on_sending);
                                 flow.on_loader_refused(12.0);
                                 return flow;
                             },
                             11 * mib, 11 * mib));
        // Pegasus DL answered earlier and stopped: Retry (white) and the gray choice.
        list.push_back(scene("offer-dropped",
                             [facts]
                             {
                                 PegasusLoadFlow flow;
                                 LoadFacts up = facts(0.0, true);
                                 up.health_ok = true;
                                 flow.tick(up);
                                 flow.tick(facts(10.0, true));
                                 flow.tick(facts(10.0 + orbit::kPegasusDroppedS + 1.0, true));
                                 return flow;
                             },
                             0, 0));
        list.push_back(scene("offer-offline", [offered] { return offered(false); }, 0, 0));
        list.push_back(scene("offer-failed",
                             [loading]
                             {
                                 PegasusLoadFlow flow = loading(&PegasusLoadFlow::on_verifying);
                                 flow.on_failed(LoadFail::verify);
                                 return flow;
                             },
                             11 * mib, 11 * mib));
        list.push_back(scene("offer-auto-on",
                             [offered, facts]
                             {
                                 PegasusLoadFlow flow = offered(true);
                                 flow.choose(LoadChoice::enable_auto, facts(12.0, true));
                                 flow.on_downloading();
                                 return flow;
                             },
                             2 * mib, 11 * mib, true));
    }

    // Store: the grid, focus and scrolling.
    list.push_back({"store",
                    Mode::normal,
                    {tab(orbit::tab::store), wait(600), pause(1.0f, "store"), move(Direction::right),
                     move(Direction::right), move(Direction::down), pause(0.6f, "store-focus"),
                     move(Direction::down), move(Direction::down), pause(0.8f, "store-scrolled"),
                     move(Direction::up), move(Direction::up), move(Direction::up),
                     move(Direction::up), pause(0.6f, "store-filters")}});
    list.push_back({"store-empty", Mode::empty, {wait(800), pause(1.0f, "store-empty")}});

    // The catalogue-style grid: five covers per row, status, favourites, tiers,
    // the round sort/filter button and the Utilities panel.
    const auto set_state = [](auto change)
    {
        return run(0.2f,
                   [change](App &app, FakeBackend &)
                   {
                       orbit::CatalogueState state = app.store_screen().catalogue_state();
                       change(state);
                       app.store_screen().set_catalogue_state(state);
                   });
    };
    list.push_back(
        {"store-catalogue",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), pause(1.0f, "store-grid"), move(Direction::right),
          move(Direction::right), move(Direction::down), pause(0.6f, "store-grid-focus"),
          tap(Action::west), move(Direction::right), tap(Action::west), move(Direction::up),
          pause(0.6f, "store-grid-favourites"), move(Direction::down), move(Direction::down),
          move(Direction::down), pause(0.8f, "store-grid-scrolled"),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query("foxglove"); }),
          wait(100), pause(0.6f, "store-grid-long-title"),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query(""); }),
          set_state([](orbit::CatalogueState &s) { s.sort = orbit::SortMode::installed_first; }),
          pause(0.8f, "store-grid-installed-first"),
          set_state([](orbit::CatalogueState &s)
                    { s.installed = orbit::InstalledFilter::installed; }),
          pause(0.8f, "store-grid-filter-installed"),
          set_state([](orbit::CatalogueState &s)
                    { s.installed = orbit::InstalledFilter::not_installed; s.sort = orbit::SortMode::tier_first; }),
          pause(0.8f, "store-grid-not-installed-tier"),
          set_state([](orbit::CatalogueState &s) { s.reset_filters(); s.sort = orbit::SortMode::az; }),
          pause(0.5f), move(Direction::left), move(Direction::left), move(Direction::left),
          pause(0.6f, "store-grid-button")}});
    list.push_back(
        {"store-utilities",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), tap(Action::menu), pause(0.8f, "store-utilities"),
          tap(Action::confirm), move(Direction::down), move(Direction::down),
          pause(0.8f, "store-sort-panel"), tap(Action::back, 0.4f), move(Direction::down),
          tap(Action::confirm), move(Direction::down), move(Direction::right),
          move(Direction::down), move(Direction::down), move(Direction::right),
          pause(0.8f, "store-filter-panel"), tap(Action::back, 0.4f), tap(Action::back, 0.4f),
          pause(0.8f, "store-grid-filter-tier-installed"),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query("zzzz"); }),
          pause(0.8f, "store-grid-empty")}});
    list.push_back(
        {"package-favourite",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), tap(Action::confirm), wait(300), pause(0.5f),
          tap(Action::west), pause(0.8f, "package-detail-favourite")}});

    // One tile per game: Starfall Odyssey (three listings) with its regions;
    // the grid filtered by region; the filter page with Region, firmware, browser.
    list.push_back(
        {"store-games",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), set_firmware("10.xx"),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query("starfall"); }),
          pause(0.8f, "games-grid-one-tile"),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query(""); }),
          move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down),
          pause(0.8f, "games-grid"), move(Direction::up), move(Direction::up), move(Direction::up),
          move(Direction::up),
          set_state([](orbit::CatalogueState &s) { s.region = "EUR"; }), pause(0.8f, "games-grid-region"),
          tap(Action::menu), tap(Action::confirm), move(Direction::down), move(Direction::down),
          pause(0.8f, "games-filter-panel"), tap(Action::back, 0.4f), tap(Action::back, 0.4f),
          set_state([](orbit::CatalogueState &s) { s.reset_filters(); })}});

    // The grid-level filter page with counts, region / firmware / single file on.
    list.push_back(
        {"store-grid-filters",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), set_firmware("10.xx"),
          set_state([](orbit::CatalogueState &s) { s.region = "USA"; s.single_file_only = true; s.runs_on_my_fw = true; }),
          tap(Action::menu), tap(Action::confirm), move(Direction::down), move(Direction::down),
          pause(0.8f, "grid-filters-panel"), tap(Action::back, 0.4f), tap(Action::back, 0.4f),
          pause(0.8f, "grid-filters-header"),
          set_state([](orbit::CatalogueState &s) { s.reset_filters(); })}});

    // The game page: Best, sections, part sets, Download all parts, filters.
    list.push_back(
        {"game-page",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), open_package("pkg25"), wait(400), pause(0.8f, "game-page"),
          move(Direction::down), wait(500), pause(0.8f, "game-page-downloads"), move(Direction::up), wait(500),
          focus_host("VikingFile", 22), pause(0.8f, "game-page-multipart"), tap(Action::confirm),
          pause(0.6f, "game-page-parts-menu"), tap(Action::confirm), pause(0.6f, "game-page-download-all"),
          tap(Action::back, 0.4f), focus_host("Rootz", 3), pause(0.8f, "game-page-incomplete-set"),
          focus_host("Akirabox", 5), pause(0.8f, "game-page-restarted-set"),
          focus_link(30), pause(0.8f, "game-page-sections"), focus_link(3), pause(0.8f, "game-page-other"),
          tap(Action::menu), move(Direction::down), move(Direction::down), pause(0.8f, "game-page-filters"),
          tap(Action::back, 0.4f),
          run(0.2f,
              [](App &app, FakeBackend &)
              {
                  orbit::MirrorFilter filter;
                  filter.region = "JPN";
                  app.store_screen().set_link_filter(filter);
              }),
          move(Direction::down), pause(0.8f, "game-page-empty-filter")}});
    // Download all parts, one at a time: confirm, queue, "n of N queued" on the card.
    list.push_back(
        {"game-page-download-all-run",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), open_package("pkg25"), wait(400), focus_host("VikingFile", 22),
          tap(Action::confirm), tap(Action::confirm), pause(0.6f, "game-page-download-all-confirm"),
          tap(Action::confirm), wait(1500), pause(0.8f, "game-page-download-all-done")}});
    list.push_back(
        {"game-page-best",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), set_firmware("10.xx"), open_package("pkg26"), wait(400),
          pause(0.8f, "game-page-best"), move(Direction::down), move(Direction::down),
          pause(0.8f, "game-page-best-scrolled")}});

    // Search with the on-screen keyboard, then the filtered grid; source filter.
    list.push_back(
        {"store-search",
         Mode::normal,
         {tab(orbit::tab::store), wait(600), tap(Action::north), pause(0.8f, "store-search-keyboard"),
          tap(Action::back, 0.5f),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query("ha"); }),
          pause(0.8f, "store-search-results"),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query("zzzz"); }),
          pause(0.6f, "store-search-none"),
          run(0.2f, [](App &app, FakeBackend &) { app.store_screen().set_query(""); }),
          move(Direction::up), move(Direction::right), tap(Action::confirm),
          pause(0.6f, "store-source-filter"), move(Direction::down), tap(Action::confirm),
          pause(0.8f, "store-source-chosen")}});

    // The search box: empty field with its button hints, live results while typing, recent searches.
    list.push_back(
        {"store-search-box",
         Mode::normal,
         {tab(orbit::tab::store), wait(600), tap(Action::north), pause(0.8f, "search-empty"),
          run(0.1f, [](App &app, FakeBackend &) { app.store_screen().type_into_prompt("ha"); }),
          pause(0.6f, "search-typing-results"), tap(Action::page_prev), tap(Action::page_prev),
          pause(0.5f, "search-caret-moved"), tap(Action::back, 0.5f),
          run(0.2f,
              [](App &app, FakeBackend &)
              {
                  orbit::CatalogueState state = app.store_screen().catalogue_state();
                  for (const char *term : {"god of war", "PPSA90101", "[exFAT] ha", "foxglove"})
                      state.add_recent_search(term);
                  app.store_screen().set_catalogue_state(state);
              }),
          tap(Action::north), pause(0.8f, "search-recents"), move(Direction::up),
          move(Direction::up), move(Direction::up), move(Direction::up),
          pause(0.6f, "search-recents-focus")}});

    // A package page: links with their routes and badges.
    list.push_back({"package",
                    Mode::normal,
                    {tab(orbit::tab::store), wait(600), open_package("pkg1"), wait(300),
                     pause(0.8f, "package-detail"), move(Direction::down), move(Direction::down),
                     move(Direction::down), pause(0.6f, "package-detail-focus")}});

    // Link support tiers (green / yellow / neutral / red, sorted best first).
    list.push_back({"package-tiers",
                    Mode::normal,
                    {tab(orbit::tab::store), wait(600), open_package("pkg9"), wait(300),
                     pause(0.8f, "package-tiers"), move(Direction::down), move(Direction::down),
                     move(Direction::down), move(Direction::down), move(Direction::down),
                     move(Direction::down), pause(0.8f, "package-tiers-last")}});

    // Every link route: .pkg hand-off, browser capture, debrid with file selection.
    list.push_back(
        {"link-native",
         Mode::normal,
         {tab(orbit::tab::store), wait(600), open_package("pkg1"), wait(300), move(Direction::down), wait(300),
          tap(Action::confirm),
          pause(0.6f, "link-native-confirm"), tap(Action::confirm), wait(500),
          pause(0.8f, "link-native-sent"), tap(Action::confirm)}});
    list.push_back(
        {"link-capture",
         Mode::normal,
         {tab(orbit::tab::store), wait(600),
          run(0.1f, [](App &, FakeBackend &backend) { backend.set_capture_steps(2); }),
          open_package("pkg1"), wait(300), focus_link(2),
          tap(Action::confirm), pause(0.6f, "link-capture-confirm"), tap(Action::confirm),
          wait(500), pause(0.8f, "link-capture-panel"), tap(Action::back), pause(0.6f, "link-capture-hidden"),
          wait(3500, "link-capture-done")}});
    list.push_back(
        {"link-debrid",
         Mode::normal,
         {tab(orbit::tab::store), wait(600), open_package("pkg1"), wait(300), focus_link(4),
          tap(Action::confirm), pause(0.6f, "link-debrid-service"), tap(Action::confirm), wait(500),
          pause(0.8f, "link-debrid-files"), move(Direction::down), tap(Action::confirm),
          pause(0.4f, "link-debrid-files-ticked")}});

    // Downloads: phases, actions, archive prompts, direct link.
    list.push_back(
        {"downloads",
         Mode::normal,
         {tab(orbit::tab::downloads), wait(800), pause(1.0f, "downloads"), tap(Action::confirm),
          pause(0.6f, "downloads-menu"), tap(Action::back), move(Direction::down),
          move(Direction::down), move(Direction::down), move(Direction::down),
          pause(0.8f, "downloads-scrolled"),
          tap(Action::confirm), pause(0.6f, "downloads-password-menu"), tap(Action::confirm),
          pause(0.8f, "downloads-password"), tap(Action::back, 0.5f), move(Direction::down),
          tap(Action::confirm), tap(Action::confirm), pause(0.6f, "downloads-choice"),
          tap(Action::back, 0.5f), tap(Action::west), pause(0.8f, "downloads-add-link")}});
    list.push_back({"downloads-cancel",
                    Mode::normal,
                    {tab(orbit::tab::downloads), wait(800), tap(Action::confirm),
                     move(Direction::down), tap(Action::confirm), pause(0.6f, "downloads-cancel-confirm")}});
    // The after-download banner (ShadowMount scan): fixed states, no request is made.
    const auto banner = [](orbit::ScanPhase phase, std::vector<std::string> registered = {})
    {
        return run(0.4f,
                   [phase, registered](App &app, FakeBackend &)
                   {
                       orbit::ScanBanner state;
                       state.phase = phase;
                       state.registered = registered;
                       app.after_install().debug_show(state);
                   });
    };
    list.push_back({"downloads-scan-deferred",
                    Mode::normal,
                    {tab(orbit::tab::downloads), wait(800), banner(orbit::ScanPhase::deferred),
                     move(Direction::up), pause(0.6f, "downloads-scan-deferred")}});
    list.push_back({"downloads-scan-registered",
                    Mode::normal,
                    {tab(orbit::tab::downloads), wait(800),
                     banner(orbit::ScanPhase::registered, {"Ember Hollow"}),
                     pause(0.6f, "downloads-scan-registered")}});
    list.push_back({"downloads-scan-unreachable",
                    Mode::normal,
                    {tab(orbit::tab::downloads), wait(800), banner(orbit::ScanPhase::unreachable),
                     move(Direction::up), pause(0.6f, "downloads-scan-unreachable")}});

    // Sources: list, menu, add by address, suggested list, first-run onboarding.
    list.push_back({"sources",
                    Mode::normal,
                    {tab(orbit::tab::sources), wait(800), pause(0.8f, "sources"), tap(Action::confirm),
                     pause(0.6f, "sources-menu"), tap(Action::back, 0.5f), tap(Action::west),
                     pause(0.8f, "sources-add"), tap(Action::back, 0.5f), tap(Action::north),
                     pause(0.8f, "sources-suggested")}});
    list.push_back({"sources-onboarding",
                    Mode::onboarding,
                    {tab(orbit::tab::sources), wait(1200), pause(1.0f, "sources-onboarding")}});
    list.push_back({"store-onboarding", Mode::onboarding, {wait(1200), pause(1.0f, "store-no-sources")}});

    list.push_back({"library",
                    Mode::normal,
                    {tab(orbit::tab::library), wait(800), pause(1.2f, "library"),
                     move(Direction::down), pause(0.6f, "library-focus")}});
    // Library: the action menu, an APR Emu update followed to the end, the
    // two-step delete, and a Pegasus whose APR version list answers 503.
    list.push_back(
        {"library-apr",
         Mode::normal,
         {tab(orbit::tab::library), wait(800), tap(Action::confirm), pause(0.6f, "library-menu"),
          tap(Action::confirm), pause(0.6f, "library-apr-confirm"), tap(Action::confirm), wait(300),
          pause(1.0f, "library-apr-running"), pause(4.0f, "library-apr-done")}});
    list.push_back(
        {"library-delete",
         Mode::normal,
         {tab(orbit::tab::library), wait(800), move(Direction::down), tap(Action::confirm),
          move(Direction::down), move(Direction::down), move(Direction::down),
          tap(Action::confirm), pause(0.6f, "library-delete-confirm"), move(Direction::right),
          tap(Action::confirm), pause(0.8f, "library-delete-type"),
          run(0.1f, [](App &app, FakeBackend &) { app.library_screen().type_into_prompt("PPSA90102"); }),
          pause(0.6f, "library-delete-typed")}});
    list.push_back(
        {"library-503",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_apr_manifest_ok(false); }),
          tab(orbit::tab::library), wait(1200), pause(0.8f, "library-503"), tap(Action::confirm),
          move(Direction::down), pause(0.6f, "library-503-menu")}});
    // Files: one list with drive chips and a breadcrumb, marks, the actions,
    // "Where to?", every listing state, and the operations strip and panel.
    const auto open_files = [](const char *path)
    { return run(0.1f, [path](App &app, FakeBackend &) { app.files_screen().open_path(path); }); };
    const auto file_op = [](const char *type, const char *status, const char *source, const char *destination,
                            double total, double done, const char *error = "")
    {
        orbit::FileOp op;
        op.type = type;
        op.status = status;
        op.source = source;
        op.destination = destination;
        op.total_items = 1;
        op.done_items = done >= total ? 1 : 0;
        op.total_bytes = total;
        op.done_bytes = done;
        op.error = error;
        return op;
    };
    list.push_back(
        {"files",
         Mode::normal,
         {tab(orbit::tab::files), wait(800), pause(0.8f, "files"), open_files("/data/pegasus-dl/downloads"),
          wait(500), pause(0.8f, "files-folder"), move(Direction::down), tap(Action::west), tap(Action::west),
          pause(0.6f, "files-selected"), tap(Action::north), pause(0.6f, "files-actions")}});
    list.push_back(
        {"files-large",
         Mode::normal,
         {tab(orbit::tab::files), wait(800), open_files("/mnt/ext0/homebrew"), wait(500),
          pause(0.8f, "files-ext0-homebrew"), move(Direction::up), pause(0.5f, "files-breadcrumb"),
          move(Direction::up), pause(0.5f, "files-chips"), tap(Action::back),
          run(0.1f, [](App &app, FakeBackend &) { app.files_screen().open_path("/mnt/ext0"); }), wait(400),
          tap(Action::back), wait(500), pause(0.8f, "files-up-past-mnt")}});
    list.push_back(
        {"files-states",
         Mode::normal,
         {tab(orbit::tab::files), wait(800),
          run(0.1f, [](App &, FakeBackend &backend) { backend.file_mkdir("/data", "Empty"); }),
          open_files("/data/Empty"), wait(500), pause(0.8f, "files-empty"),
          open_files("/data/pegasus-dl/downloads"), wait(500),
          run(0.1f, [](App &app, FakeBackend &) { app.files_screen().set_filter("zzz"); }),
          pause(0.6f, "files-filter-empty"),
          run(0.1f, [](App &app, FakeBackend &) { app.files_screen().set_filter("rar"); }),
          pause(0.6f, "files-filter"),
          run(0.1f, [](App &, FakeBackend &backend)
              { backend.set_slow_listing("/mnt/ext0/homebrew", 0, true); }),
          open_files("/mnt/ext0/homebrew"), wait(500), pause(0.8f, "files-error"),
          run(0.1f, [](App &, FakeBackend &backend)
              { backend.set_slow_listing("/mnt/ext0/homebrew", 6000, false); }),
          tap(Action::confirm), pause(0.5f, "files-loading"), pause(4.5f, "files-loading-slow")}});
    list.push_back(
        {"files-whereto",
         Mode::normal,
         {tab(orbit::tab::files), wait(800), open_files("/mnt/ext0/homebrew"), wait(500),
          move(Direction::down), tap(Action::west), tap(Action::west), tap(Action::north),
          tap(Action::confirm), wait(600), pause(0.8f, "files-whereto"), move(Direction::up),
          move(Direction::right), pause(0.5f, "files-whereto-chips"), move(Direction::down),
          tap(Action::west), pause(0.5f, "files-whereto-replace"), tap(Action::north),
          pause(0.6f, "files-replace-confirm")}});
    list.push_back(
        {"files-ops",
         Mode::normal,
         {run(0.1f,
              [file_op](App &, FakeBackend &backend)
              {
                  backend.add_file_op(file_op("copy", "completed", "/data/homebrew/tool.ffpkg", "/mnt/usb0", 43e6, 43e6));
                  backend.add_file_op(file_op("copy", "running", "/mnt/ext0/homebrew/PPSA06464.exfat", "/mnt/usb0",
                                              4127195136.0, 1.2e9));
                  // The newest: a move Pegasus takes 5 s to stop.
                  backend.add_file_op(file_op("move", "running", "/mnt/ext0/homebrew/PPSA02015.exfat",
                                              "/data/homebrew", 170804641792.0, 71.7e9),
                                      true, 5.0);
              }),
          tab(orbit::tab::files), wait(800), open_files("/mnt/ext0/homebrew"), wait(500), pause(2.0f),
          pause(1.0f, "files-ops-running"), tap(Action::menu), wait(300), pause(0.6f, "files-ops-panel"),
          tap(Action::west), pause(0.6f, "files-cancel-confirm"), tap(Action::west), wait(400), pause(2.0f),
          tap(Action::back), pause(0.6f, "files-cancelling"),
          run(0.1f, [](App &app, FakeBackend &) { app.show_tab(orbit::tab::store); }),
          pause(0.6f, "files-tab-badge")}});
    list.push_back(
        {"files-finished",
         Mode::normal,
         {run(0.1f,
              [file_op](App &, FakeBackend &backend)
              {
                  backend.add_file_op(file_op("copy", "completed", "/data/homebrew/tool.ffpkg", "/mnt/usb0", 43e6, 43e6));
                  backend.add_file_op(file_op("delete", "completed", "/data/pegasus-dl/downloads/old.part", "", 9870, 9870));
                  backend.add_file_op(file_op("pkg-install", "accepted", "/data/pegasus-dl/downloads/Ember Hollow.pkg", "",
                                              1e9, 1e9));
                  backend.add_file_op(file_op("copy", "failed", "/data/save.bin", "/mnt/usb0", 2e9, 0,
                                              "No space left on device"));
              }),
          tab(orbit::tab::files), wait(800), pause(1.0f, "files-ops-failed"), tap(Action::menu), wait(300),
          pause(0.6f, "files-ops-history"), tap(Action::back), pause(9.0f, "files-ops-idle")}});
    list.push_back(
        {"files-password",
         Mode::normal,
         {run(0.1f,
              [file_op](App &, FakeBackend &backend)
              {
                  backend.add_file_op(file_op("extract", "waiting_password", "/data/pegasus-dl/downloads/Locked Archive.rar",
                                              "/data/pegasus-dl/downloads", 2e9, 0));
              }),
          tab(orbit::tab::files), wait(900), pause(0.8f, "files-password-prompt"), tap(Action::back, 0.5f),
          pause(0.6f, "files-password-strip")}});
    // Settings has two groups: Wings (kept on the console by Wings) and Pegasus DL.
    list.push_back({"settings",
                    Mode::normal,
                    {tab(orbit::tab::settings), wait(500), pause(0.8f, "settings"),
                     move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), pause(0.8f, "settings-wings-2"), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down),
                     pause(0.8f, "settings-wings-about")}});
    list.push_back(
        {"settings-pegasus",
         Mode::normal,
         {tab(orbit::tab::settings), wait(500), move(Direction::right), wait(300),
          pause(0.8f, "settings-pegasus"), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), pause(0.8f, "settings-pegasus-runtime")}});
    list.push_back(
        {"settings-pegasus-offline",
         Mode::offline,
         {tab(orbit::tab::settings), wait(500), pause(0.8f, "settings-wings-offline"),
          move(Direction::right), wait(300), pause(0.8f, "settings-pegasus-offline")}});
    // Popularity badges (preview only: a small built-in sample index, nothing is fetched).
    list.push_back(
        {"store-popularity",
         Mode::normal,
         {run(0.1f,
              [](App &, FakeBackend &)
              {
                  std::string json = "{\"schemaVersion\":1,\"generatedAt\":\"2026-10-12T00:00:00Z\","
                                     "\"attribution\":{\"igdb\":\"Ratings and popularity data from IGDB.com\","
                                     "\"steam\":\"Review data from Steam.\",\"wikipedia\":\"Pageview data from Wikimedia (CC0).\"},"
                                     "\"titles\":{";
                  const char *ids[] = {"PPSA02015", "PPSA02530", "PPSA06464", "PPSA90001", "PPSA90002", "PPSA90003",
                                       "PPSA90004", "PPSA90005", "PPSA90007", "PPSA90009", "PPSA90013", "PPSA90014"};
                  for (int i = 0; i < 12; ++i)
                      json += std::string(i ? "," : "") + "\"" + ids[i] + "\":{\"score\":" + std::to_string(95 - i * 7) +
                              ",\"rating\":" + std::to_string(92 - i * 3) + ",\"ratingCount\":" +
                              std::to_string(4300 - i * 300) + ",\"confidence\":1.0,\"sources\":[\"igdb\"],\"fetchedAt\":\"2026-10-12\"}";
                  json += "}}";
                  auto index = std::make_shared<orbit::PopularityIndex>();
                  std::string why;
                  if (orbit::PopularityIndex::parse(json, index.get(), &why))
                      orbit::popularity_hub().set_index(index);
                  orbit::popularity_hub().set_enabled(true);
              }),
          tab(orbit::tab::store), wait(900), pause(0.8f, "store-popularity-grid"), open_package("pkg25"), wait(400),
          pause(0.8f, "store-popularity-page"),
          run(0.1f,
              [](App &, FakeBackend &)
              {
                  orbit::popularity_hub().set_enabled(false);
                  orbit::popularity_hub().set_index(nullptr);
              })}});
    // Link ranking choices, popularity placeholder, the Store grid reset.
    list.push_back(
        {"settings-wings-prefs",
         Mode::normal,
         {tab(orbit::tab::settings), wait(500), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), tap(Action::confirm),
          pause(0.6f, "settings-firmware-picker"), move(Direction::down), move(Direction::down),
          move(Direction::down), tap(Action::confirm), wait(300), move(Direction::down), move(Direction::down), move(Direction::down), tap(Action::confirm),
          pause(0.5f, "settings-debrid-off"), move(Direction::down), tap(Action::confirm),
          pause(0.6f, "settings-popularity-on")}});
    list.push_back(
        {"settings-reset-grid",
         Mode::normal,
         {tab(orbit::tab::settings), wait(500), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), tap(Action::confirm),
          pause(0.6f, "settings-reset-grid-confirm")}});
    list.push_back(
        {"settings-capabilities",
         Mode::normal,
         {tab(orbit::tab::settings), wait(500), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), move(Direction::down), tap(Action::confirm),
          pause(0.6f, "settings-capabilities")}});
    // Settings editing: capabilities, the download-folder picker, connections, tokens.
    list.push_back(
        {"settings-edit",
         Mode::normal,
         {tab(orbit::tab::settings), wait(600), move(Direction::right), wait(300),
          move(Direction::down), pause(0.6f, "settings-folder-row"), tap(Action::confirm), wait(500),
          pause(0.6f, "settings-folder-picker"), tap(Action::back, 0.4f), wait(300),
          tap(Action::back, 0.4f), wait(300), tap(Action::back, 0.4f), wait(300),
          pause(0.6f, "settings-folder-locations"), tap(Action::back, 0.4f),
          move(Direction::down), tap(Action::confirm), pause(0.6f, "settings-connections"),
          tap(Action::back, 0.4f), move(Direction::down), tap(Action::confirm),
          pause(0.6f, "settings-token-menu"), tap(Action::confirm), pause(0.6f, "settings-token-keyboard"),
          run(0.1f, [](App &app, FakeBackend &) { app.settings_screen().type_into_prompt("good-token-1234"); }),
          pause(0.6f, "settings-token-masked")}});
    list.push_back(
        {"settings-token-saved",
         Mode::normal,
         {tab(orbit::tab::settings), wait(600), move(Direction::right), wait(300),
          move(Direction::down), move(Direction::down), move(Direction::down),
          move(Direction::down), tap(Action::confirm), tap(Action::confirm),
          run(0.1f, [](App &app, FakeBackend &) { app.settings_screen().type_into_prompt("not-a-good-one"); }),
          move(Direction::down, 0.1f), move(Direction::down, 0.1f), move(Direction::down, 0.1f),
          move(Direction::down, 0.1f), move(Direction::down, 0.1f), move(Direction::down, 0.1f),
          move(Direction::down, 0.1f), move(Direction::down, 0.1f), move(Direction::down, 0.1f),
          move(Direction::down, 0.1f), tap(Action::confirm), wait(500),
          pause(0.8f, "settings-token-invalid")}});
    // Store covers: DNS-over-HTTPS is off by default (placeholders, the "store"
    // pictures); the player turns it on in Settings and covers arrive.
    list.push_back(
        {"settings-cover-art",
         Mode::normal,
         {tab(orbit::tab::settings), wait(500), move(Direction::down),
          pause(0.8f, "settings-cover-art-off"), tap(Action::confirm),
          pause(0.6f, "settings-cover-art-confirm"), tap(Action::confirm), pause(0.5f),
          pause(0.5f, "settings-cover-art-on"), move(Direction::down), move(Direction::down),
          tap(Action::confirm), pause(0.5f, "settings-cover-art-cleared")}});
    list.push_back(
        {"store-covers",
         Mode::normal,
         {run(0.1f, [](App &, FakeBackend &backend) { backend.set_store_art(true); }),
          tab(orbit::tab::store), wait(900), pause(1.0f, "store-covers"), move(Direction::right),
          move(Direction::down), pause(0.6f, "store-covers-focus"), move(Direction::down),
          move(Direction::down), pause(0.8f, "store-covers-scrolled"),
          open_package("pkg2"), wait(300), pause(0.8f, "package-detail-cover")}});
    list.push_back({"logs", Mode::normal, {tab(orbit::tab::logs), wait(800), pause(1.5f, "logs")}});
    list.push_back(
        {"logs-options",
         Mode::normal,
         {tab(orbit::tab::logs), wait(800), tap(Action::confirm), pause(0.6f, "logs-menu"),
          move(Direction::down), tap(Action::confirm), pause(0.8f, "logs-warnings"),
          tap(Action::confirm), move(Direction::down), move(Direction::down), move(Direction::down),
          move(Direction::down), move(Direction::down), tap(Action::confirm),
          pause(0.6f, "logs-clear-confirm"), tap(Action::back, 0.4f), tap(Action::north),
          pause(0.8f, "logs-diagnostics")}});
    // Waiting state on a tab other than the Store, and tab switching by L1/R1.
    list.push_back({"offline-tabs",
                    Mode::offline,
                    {tab(orbit::tab::downloads), pause(2.5f, "offline-downloads"),
                     tap(Action::page_next), tap(Action::page_next), tap(Action::page_next),
                     tap(Action::page_next), tap(Action::page_next), pause(0.6f, "offline-settings")}});
    return list;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir> [scenario|all] [width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const std::string only = argc > 3 && std::string(argv[3]) != "all" ? argv[3] : "";
    const int width = argc > 5 ? std::atoi(argv[4]) : 1920;
    const int height = argc > 5 ? std::atoi(argv[5]) : 1080;
    // WINGS_LANG=de renders the screens in German, from <assets>/i18n catalogues.
    if (const char *language = std::getenv("WINGS_LANG"))
    {
        if (!orbit::i18n::load(assets + "/i18n", language))
            std::fprintf(stderr, "language %s unavailable; using English\n", language);
    }

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font light;
    hui::gfx::Font regular;
    hui::gfx::Font medium;
    hui::gfx::Font semibold;
    orbit::ui::Type type;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/inter-display-light.huifont", &light, &type.light) ||
        !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &type.regular) ||
        !load_font(renderer, assets + "/fonts/inter-medium.huifont", &medium, &type.medium) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold, &type.semibold))
        return 1;
    hui::ui::Fonts fonts;
    fonts.regular = type.regular;
    fonts.semibold = type.semibold;
    fonts.display = type.light;
    fonts.mono = type.medium;
    fonts.pixel = type.regular;
    fonts.hand = type.regular;
    orbit::GlTextures textures(renderer.batch());
    const std::uint32_t mark = textures.create(orbit::mark::render(128));
    const std::uint32_t loader_mark = textures.create(orbit::mark::render(256, 0.32f));

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    stbi_flip_vertically_on_write(1);

    bool ok = true;
    int ran = 0;
    for (const Scenario &scenario : scenarios())
    {
        if (!only.empty() && only != scenario.name)
            continue;
        ++ran;
        orbit::host::FakeBackend backend(scenario.mode);
        orbit::Store store(backend);
        orbit::ArtCache art(backend, textures);
        store.start();
        art.start();
        orbit::Context context(type, fonts, store, art);
        // The console's Settings section, with the fake art service behind it:
        // off by default, like the real thing.
        orbit::host::FakeCoverArt cover_art(backend, false);
        context.cover_art = &cover_art;
        context.mark = mark;
        context.loader_mark = loader_mark;
        const char *preview_version = std::getenv("WINGS_APP_VERSION");
        context.app_version = preview_version != nullptr ? preview_version : "00.001.000";
        // Preview callbacks never open websites.
        context.open_pegasus_ui = [] { return true; };
        orbit::App app(context);
        orbit::Frame frame;
        frame.glass_texture = renderer.glass_texture();

        constexpr float kDt = 1.0f / 60.0f;
        hui::ui::Feedback feedback;
        const auto tick = [&](const hui::InputFrame &input)
        {
            feedback.clear();
            app.update(input, kDt, feedback);
            // The workers run in real time: give them a moment every frame.
            usleep(1500);
        };
        const auto render = [&](const std::string &name)
        {
            // Draw once so the screen asks for its art, then wait (in real
            // time) until it has arrived, so pictures are complete.
            frame.reset();
            app.draw(frame);
            for (int i = 0; i < 600 && !art.idle(); ++i)
            {
                tick({});
                frame.reset();
                app.draw(frame);
            }
            // Art fades in over real time: let it finish.
            for (int i = 0; i < 30; ++i)
                tick({});
            usleep(300000);
            tick({});
            frame.reset();
            app.draw(frame);
            renderer.begin();
            renderer.backdrop(frame.backdrop);
            renderer.draw(frame.scene);
            if (frame.glass)
                renderer.glass();
            renderer.draw(frame.overlay);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(framebuffer, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (std::size_t i = 3; i < pixels.size(); i += 4)
                pixels[i] = 255;
            const std::string path = output + "/" + name + ".png";
            ok =
                stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
            std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n",
                         path.c_str(), renderer.last_instances(), renderer.last_draw_calls(),
                         glGetError());
        };

        for (const Step &step : scenario.steps)
        {
            const int frames = static_cast<int>(step.wait / kDt);
            for (int i = 0; i < frames; ++i)
                tick({});
            for (int waited = 0; waited < step.real_ms; waited += 20)
            {
                usleep(20000);
                tick({});
            }
            if (step.act)
                step.act(app, backend);
            if (step.capture != nullptr)
                render(step.capture);
            hui::InputFrame input;
            input.connected = true;
            input.pressed = step.press;
            input.nav = step.nav;
            tick(input);
        }
        art.stop();
        store.stop();
        art.clear();
    }
    if (ran == 0)
    {
        std::fprintf(stderr, "no scenario named '%s'\n", only.c_str());
        return 2;
    }
    return ok ? 0 : 1;
}
