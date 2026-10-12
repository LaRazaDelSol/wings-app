// Wings TV app - The app: top bar, tabs, the connection pill, toast and hints.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Orbit Store's app.cpp (GPL-3.0-or-later): the chrome (areas,
// L1/R1 tabs, top bar, toast, hint bar) is kept. The gate that held the whole
// UI behind the catalogue is gone: the shell always shows, and each screen
// says "Waiting for Pegasus DL" itself while Pegasus is not answering.

#include "orbit/mark.hpp"
#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"
#include "orbit/wings_update.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit
{

namespace
{

constexpr const char *kTabs[] = {/* i18n */ "Store",   /* i18n */ "Downloads", /* i18n */ "Library",
                                 /* i18n */ "Files",   /* i18n */ "Sources",   /* i18n */ "Settings",
                                 /* i18n */ "Logs"}; // by tab::
constexpr int kTabCount = tab::count;
// The wash's places are the tabs, in order.
static_assert(theme::place_count == tab::count && theme::place_store == tab::store &&
                  theme::place_settings == tab::settings && theme::place_logs == tab::logs,
              "theme::Place must follow tab::");
// The top bar's height line, sizes and spacing, from Orbit's header.
constexpr float kTopY = 52.0f;
constexpr float kTabSize = 22.0f;
constexpr float kTabPad = 22.0f;
constexpr float kTabHeight = 46.0f;
constexpr float kTabGap = 6.0f;
constexpr const char *kBrand = "Wings";

ui::Rect tab_rect(const ui::Type &type, int index)
{
    float x = ui::kGutter + 54.0f + type.regular.measure(kBrand, 22.0f) + 14.0f + 48.0f;
    for (int i = 0; i < index; ++i)
        x += type.regular.measure(tr(kTabs[i]), kTabSize) + 2.0f * kTabPad + kTabGap;
    return {x, kTopY - kTabHeight * 0.5f,
            type.regular.measure(tr(kTabs[index]), kTabSize) + 2.0f * kTabPad, kTabHeight};
}

int active_jobs(const Snapshot &state)
{
    int count = 0;
    for (const Job &job : state.jobs)
        count += job_active(job) ? 1 : 0;
    return count;
}

} // namespace

App::App(Context &context)
    : ctx_(context), store_(context), downloads_(context), library_(context), files_(context),
      sources_(context), settings_(context), logs_(context), after_install_(context.shadowmount)
{
    top_focus_.resize(kTabCount);
    context.after_install = &after_install_;
    // The Store ranks links with the preferences Settings > Wings keeps.
    store_.use_prefs(&settings_.local_prefs());
    // The first download asks about the download folder once (panels.hpp FolderAsk); the answer is a Wings pref.
    context.folder_confirmed = [this] { return settings_.local_prefs().folder_confirmed; };
    context.confirm_folder = [this]
    {
        WingsLocalPrefs prefs = settings_.local_prefs();
        prefs.folder_confirmed = true;
        settings_.set_local_prefs(prefs);
    };
    // Settings > Wings > Reset Store grid: sort and filters only, favourites stay.
    context.reset_store_grid = [this]
    {
        CatalogueState reset = store_.catalogue_state();
        reset.sort = SortMode::az;
        reset.reset_filters();
        store_.set_catalogue_state(reset);
    };
}

Screen &App::screen()
{
    switch (tab_)
    {
    case tab::downloads:
        return downloads_;
    case tab::library:
        return library_;
    case tab::files:
        return files_;
    case tab::sources:
        return sources_;
    case tab::settings:
        return settings_;
    case tab::logs:
        return logs_;
    default:
        return store_;
    }
}

const Screen &App::screen() const
{
    return const_cast<App *>(this)->screen();
}

void App::show_tab(int tab)
{
    tab_ = std::clamp(tab, 0, kTabCount - 1);
    top_index_ = tab_;
    area_ = Area::content;
    screen().focus();
}

void App::show_toast(std::string text, float seconds)
{
    toast_ = std::move(text);
    toast_time_ = seconds;
    toast_length_ = seconds;
}

void App::handle_intent(const Intent &intent, Feedback &feedback)
{
    switch (intent.kind)
    {
    case Intent::Kind::top_bar:
        area_ = Area::top;
        top_index_ = tab_;
        feedback.play(hui::audio::Cue::focus);
        break;
    case Intent::Kind::show_tab:
        show_tab(intent.tab);
        feedback.play(hui::audio::Cue::tab);
        break;
    default:
        break;
    }
    if (!intent.toast.empty())
        show_toast(intent.toast);
}

void App::update(const InputFrame &input, float dt, Feedback &feedback)
{
    ctx_.clock += dt;
    ctx_.store.tick();
    ctx_.art.tick();
    // Screens opt in to the faster poll and the log feed every frame.
    ctx_.store.set_fast(false);
    ctx_.store.want_logs(false);
    ctx_.store.want_installed(false);
    toast_time_ = std::max(0.0f, toast_time_ - dt);
    // Pegasus DL without a download folder gets the default one, once (shown in Settings > Pegasus DL).
    if (ctx_.store.state().have_settings)
        ctx_.store.ensure_default_download_folder();
    // A finished download asks ShadowMount to scan (whichever tab shows; the Downloads tab
    // draws the answer). The setting is Settings > Pegasus DL > After a download finishes.
    after_install_.set_mode(settings_.local_prefs().after_download);
    after_install_.update(static_cast<double>(ctx_.clock), ctx_.store.state().jobs,
                          ctx_.store.state().have_queue);
    // A newer Wings: ask GitHub once the network is ready (at most every 6 hours; the service decides),
    // and say so once per start. Checked every two seconds, not every frame.
    update_poll_ += dt;
    if (update_poll_ >= 2.0f)
    {
        update_poll_ = 0.0f;
        wings_update().auto_check();
        if (!update_announced_)
        {
            const WingsUpdateStatus up = wings_update().status();
            if (up.attached && up.enabled && up.advice == WingsAdvice::newer)
            {
                update_announced_ = true;
                show_toast(wings_update_toast(up), 8.0f);
                feedback.play(hui::audio::Cue::notify);
            }
        }
    }
    // The cover icon source (Settings > Art sources): covers are asked for again when what they resolve to changes.
    if (ctx_.posters != nullptr &&
        ctx_.posters->set_icon_source(settings_.local_prefs().icon_source == IconSource::prospero,
                                      popularity_hub().index()))
        ctx_.art.reset_covers();
    // The one wash: aimed at the tab's hue, then at the section the screen reports.
    wash_.retarget(tab_, screen().section_accent());
    wash_.update(dt);
    // File operations are followed whichever tab shows (and the Files tab is marked).
    files_.background(tab_ == tab::files);

    // The browser-capture panel sits above everything and takes the input.
    {
        Feedback &fb = feedback;
        const bool shown_before = capture_panel_open();
        update_capture(input, dt, fb);
        if (shown_before || capture_panel_open())
        {
            const InputFrame quiet{};
            screen().update(quiet, dt, feedback, false);
            for (int i = 0; i < kTabCount; ++i)
            {
                top_focus_[static_cast<std::size_t>(i)].target =
                    area_ == Area::top && i == top_index_ ? 1.0f : 0.0f;
                top_focus_[static_cast<std::size_t>(i)].update(dt, 16.0f);
            }
            return;
        }
    }

    const InputFrame idle{};
    // The first-run "load Pegasus DL" flow: it runs on every tab; its choices show on the waiting card.
    const bool offer_tab = tab_ != tab::settings && tab_ != tab::logs;
    const bool offline = !ctx_.store.state().online();
    bool offer_up = false; // the choices handed the focus to the top bar this frame
    {
        const OfferOutcome offer = pegasus_offer_.update(
            area_ == Area::content && !screen().modal() ? input : idle, offer_tab, area_ == Area::content,
            !offline, settings_.local_prefs().pegasus_start, ctx_.clock, feedback);
        if (offer.mode_changed)
        {
            WingsLocalPrefs prefs = settings_.local_prefs();
            prefs.pegasus_start = offer.mode;
            settings_.set_local_prefs(prefs);
        }
        if (!offer.toast.empty())
            show_toast(offer.toast, 6.0f);
        if (offer.up)
        {
            offer_up = true;
            area_ = Area::top;
            top_index_ = tab_;
            feedback.play(hui::audio::Cue::focus);
        }
    }
    const bool offer_has_input = offer_tab && offline && pegasus_offer_.takes_input();
    // L1 / R1 switch tabs from anywhere, as on the console's own apps.
    if (!screen().modal() &&
        (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next)))
    {
        const int next = tab_ + (input.is_pressed(Action::page_next) ? 1 : -1);
        if (next >= 0 && next < kTabCount)
        {
            tab_ = next;
            top_index_ = tab_;
            screen().focus();
            feedback.play(hui::audio::Cue::tab);
        }
        else
        {
            refuse(feedback, input);
        }
    }

    if (area_ == Area::top)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = top_index_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < kTabCount)
            {
                top_index_ = next;
                // Moving along the tabs shows each one, like the console's store.
                tab_ = top_index_;
                screen().focus();
                feedback.play(hui::audio::Cue::tab);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::down)
        {
            area_ = Area::content;
            screen().focus();
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.nav == Direction::up && !offer_up)
        {
            refuse(feedback, input);
        }
        if (input.is_pressed(Action::confirm))
        {
            area_ = Area::content;
            screen().focus();
            feedback.play(hui::audio::Cue::select);
        }
        screen().update(idle, dt, feedback, false);
    }
    else
    {
        // While the choices are on screen they own the D-pad and Cross; the screen only animates.
        handle_intent(screen().update(offer_has_input ? idle : input, dt, feedback, true), feedback);
    }

    for (int i = 0; i < kTabCount; ++i)
    {
        top_focus_[static_cast<std::size_t>(i)].target =
            area_ == Area::top && i == top_index_ ? 1.0f : 0.0f;
        top_focus_[static_cast<std::size_t>(i)].update(dt, 16.0f);
    }
}

bool App::capture_panel_open() const
{
    const CaptureView &cap = ctx_.store.state().capture;
    if (cap.state.empty() || cap.state == "completed")
        return false;
    if (cap.terminal())
        return true; // failed or cancelled: shown until the player closes it
    return cap.session_id != capture_hidden_;
}

void App::update_capture(const InputFrame &input, float dt, Feedback &feedback)
{
    const Snapshot &state = ctx_.store.state();
    const CaptureView &cap = state.capture;
    // A finished capture: say so once, and clear it.
    if (cap.state == "completed" && state.capture_serial != capture_toasted_)
    {
        capture_toasted_ = state.capture_serial;
        const std::string text = !cap.result.toast.empty() ? cap.result.toast : cap.message;
        show_toast(text.empty() ? std::string(tr("Download captured and queued.")) : text);
        ctx_.store.dismiss_capture();
        capture_toasted_ = ctx_.store.state().capture_serial;
        feedback.play(hui::audio::Cue::notify);
    }
    if (cap.state.empty())
        capture_hidden_.clear();
    const bool open = capture_panel_open();
    capture_in_.target = open ? 1.0f : 0.0f;
    capture_in_.update(dt, 18.0f);
    // Options brings a hidden panel back.
    if (!open && !cap.state.empty() && !cap.terminal() && input.is_pressed(Action::menu))
    {
        capture_hidden_.clear();
        capture_button_ = 0;
        feedback.play(hui::audio::Cue::modal_open);
        return;
    }
    if (!open)
        return;
    const bool terminal = cap.terminal();
    if (!terminal && (input.nav == Direction::left || input.nav == Direction::right))
    {
        // draw_capture() puts Hide (0) at the right and Cancel capture (1) to its
        // left: the D-pad follows what is on screen (it was reversed, bug F).
        const int next = input.nav == Direction::right ? 0 : 1;
        if (next != capture_button_)
        {
            capture_button_ = next;
            feedback.play(hui::audio::Cue::focus);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    if (terminal)
        capture_button_ = 0;
    const bool hide = input.is_pressed(Action::back) ||
                      (input.is_pressed(Action::confirm) && capture_button_ == 0);
    if (hide)
    {
        if (terminal)
            ctx_.store.dismiss_capture();
        else
            capture_hidden_ = cap.session_id; // the capture keeps running
        feedback.play(hui::audio::Cue::modal_close);
    }
    else if (input.is_pressed(Action::confirm) && capture_button_ == 1)
    {
        if (ctx_.store.cancel_capture())
            feedback.play(hui::audio::Cue::select);
        else
        {
            show_toast(tr("Link resolving is in process. Please wait."));
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
}

void App::draw_capture(Frame &frame) const
{
    const CaptureView &cap = ctx_.store.state().capture;
    hui::gfx::DrawList &list = frame.overlay;
    const ui::Type &type = ctx_.type;
    // Hidden but running: a small reminder under the top bar.
    if (!capture_panel_open() && !cap.state.empty() && !cap.terminal() && cap.state != "completed")
    {
        const std::string label = tr("Browser check in progress");
        const float width = type.medium.measure(label, 19.0f) + 200.0f;
        const ui::Rect pill{1920.0f - ui::kGutter - width, 104.0f, width, 40.0f};
        list.rounded_rect(pill, 20.0f, ui::color::night3);
        const float pulse = 0.5f + 0.5f * std::sin(ctx_.clock * 3.0f);
        list.circle(pill.x + 24.0f, pill.cy(), 6.0f, ui::Color::rgb(0xffffff, 0.4f + 0.6f * pulse));
        ui::text(list, type.medium, label, pill.x + 44.0f, pill.cy() + 6.5f, 19.0f, ui::color::text);
        ui::text(list, type.regular, tr("Options: show"), pill.x + pill.w - 18.0f, pill.cy() + 6.0f,
                 17.0f, ui::color::text3, ui::Align::right);
    }
    const float a = std::clamp(capture_in_.value, 0.0f, 1.0f);
    if (a < 0.01f)
        return;
    draw_scrim(list, a);
    const float width = 1040.0f;
    const float height = 500.0f;
    const ui::Rect r{960.0f - width * 0.5f, 540.0f - height * 0.5f + (1.0f - a) * 20.0f, width, height};
    list.push_opacity(a);
    ui::modal(list, r);
    const bool terminal = cap.terminal();
    std::string title = tr("Complete the check in the PS5 browser");
    if (cap.state == "failed")
        title = tr("The browser capture did not finish");
    else if (cap.state == "cancelled")
        title = tr("Browser capture cancelled");
    else if (cap.state == "queueing")
        title = tr("Adding the download");
    ui::text_fit(list, type.light, title, r.x + 52.0f, r.y + 92.0f, 40.0f, r.w - 104.0f,
                 ui::color::text);
    std::string what = cap.title;
    if (!cap.host.empty())
        what += (what.empty() ? "" : "  \xC2\xB7  ") + cap.host;
    ui::text_fit(list, type.regular, what, r.x + 52.0f, r.y + 134.0f, 22.0f, r.w - 104.0f,
                 ui::color::text3);
    ui::paragraph(list, type.medium, cap.message, r.x + 52.0f, r.y + 196.0f, 26.0f, r.w - 104.0f,
                  36.0f, ui::color::text, 2);
    if (!terminal)
    {
        ui::paragraph(list, type.regular,
                      tr("Pegasus DL opened a page in the PS5 browser. Solve the captcha or check "
                         "there, then press the PS button and come back to Wings. This panel closes "
                         "by itself once the download is in the queue."),
                      r.x + 52.0f, r.y + 280.0f, 23.0f, r.w - 104.0f, 33.0f, ui::color::text2, 4);
        for (int i = 0; i < 3; ++i)
        {
            const float phase =
                0.5f + 0.5f * std::sin(ctx_.clock * 5.0f - static_cast<float>(i) * 0.9f);
            list.circle(r.x + 60.0f + static_cast<float>(i) * 22.0f, r.y + r.h - 70.0f, 7.0f,
                        ui::Color::rgb(0xffffff, 0.3f + 0.7f * phase));
        }
    }
    const float by = r.y + r.h - 40.0f - ui::kButtonHeight;
    const ui::Rect first{r.x + r.w - 52.0f - 250.0f, by, 250.0f, ui::kButtonHeight};
    if (terminal)
    {
        ui::button(list, type, first, tr("Close"), ui::ButtonKind::primary, 1.0f);
    }
    else
    {
        const ui::Rect cancel{first.x - 20.0f - 270.0f, by, 270.0f, ui::kButtonHeight};
        ui::button(list, type, first, tr("Hide"), ui::ButtonKind::primary,
                   capture_button_ == 0 ? 1.0f : 0.0f);
        ui::button(list, type, cancel, tr("Cancel capture"), ui::ButtonKind::secondary,
                   capture_button_ == 1 ? 1.0f : 0.0f, cap.state != "queueing");
    }
    list.pop_opacity();
}

void App::draw_status_pill(hui::gfx::DrawList &list) const
{
    // The connection, always visible at the right of the top bar.
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    std::string label;
    float dot = 0.35f;
    if (state.online())
    {
        label = state.info.version.empty()
                    ? std::string(tr("Pegasus DL connected"))
                    : tr("Pegasus DL {version}", {{"version", state.info.version}});
        dot = 1.0f;
    }
    else if (state.link == Link::connecting)
    {
        label = tr("Connecting\xE2\x80\xA6");
    }
    else
    {
        label = tr("Waiting for Pegasus DL");
    }
    const float width = type.medium.measure(label, 19.0f) + 64.0f;
    const ui::Rect pill{1920.0f - ui::kGutter - width, kTopY - 22.0f, width, 44.0f};
    list.rounded_rect(pill, 22.0f, ui::Color::rgb(0xffffff, 0.08f));
    float alpha = dot;
    if (dot < 1.0f)
        alpha = 0.35f + 0.35f * (0.5f + 0.5f * std::sin(ctx_.clock * 3.0f));
    list.circle(pill.x + 26.0f, pill.cy(), 6.0f, ui::Color::rgb(0xffffff, alpha));
    ui::text(list, type.medium, label, pill.x + 44.0f, pill.cy() + 6.5f, 19.0f,
             state.online() ? ui::color::text : ui::color::text2);
}

void App::draw_top_bar(hui::gfx::DrawList &list) const
{
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    if (ctx_.mark != 0)
        list.image(ctx_.mark, {ui::kGutter - 2.0f, kTopY - 22.0f, 44.0f, 44.0f}, hui::gfx::kFullUv,
                   ui::Color::rgb(0xffffff));
    ui::text(list, type.regular, kBrand, ui::kGutter + 54.0f, kTopY + 8.0f, 22.0f, ui::color::text);

    // Tabs: text buttons, the current one in a pale pill.
    const int active = active_jobs(state);
    char count[16];
    std::snprintf(count, sizeof(count), "%d", active);
    // .queue-count sits inside the Downloads tab, which widens for it.
    const float count_width = std::max(24.0f, type.semibold.measure(count, 14.0f) + 18.0f);
    for (int i = 0; i < kTabCount; ++i)
    {
        ui::Rect r = tab_rect(type, i);
        // Later tabs shift right when the Downloads pill widens its tab.
        if (active > 0 && i > tab::downloads)
            r.x += 10.0f + count_width;
        const bool counted = i == tab::downloads && active > 0;
        if (counted)
            r.w += 10.0f + count_width;
        const bool current = i == tab_;
        const float focus = top_focus_[static_cast<std::size_t>(i)].value;
        ui::focus_ring(list, r, r.h * 0.5f, focus);
        if (current)
        {
            list.bordered_rect(r, r.h * 0.5f, ui::Color::rgb(0xffffff, 0.13f), 1.0f,
                               ui::Color::rgb(0xffffff, 0.2f));
            // The place dot: the wash's own colour, so it crossfades with the wash on a tab switch.
            list.circle(r.x + 12.0f, r.cy(), 3.5f,
                        hui::gfx::mix(wash_.color(), ui::Color::rgb(0xffffff), 0.35f));
        }
        ui::text(list, type.regular, tr(kTabs[i]), r.x + kTabPad, kTopY + 8.0f, kTabSize,
                 current || focus > 0.5f ? ui::color::text : ui::color::text2);
        // A file operation is running: a dot on the Files tab, seen from every tab.
        if (i == tab::files && files_.ops_active())
            list.circle(r.x + r.w - 10.0f, r.y + 9.0f, 5.0f,
                        ui::Color::rgb(0xffffff, 0.6f + 0.4f * std::sin(ctx_.clock * 3.0f)));
        if (counted)
        {
            const ui::Rect pill{r.x + r.w - kTabPad - count_width, kTopY - 11.0f, count_width,
                                22.0f};
            list.rounded_rect(pill, 11.0f, ui::color::action);
            ui::text(list, type.semibold, count, pill.cx(), kTopY + 5.0f, 14.0f, ui::color::night1,
                     ui::Align::center);
        }
    }
    draw_status_pill(list);
}

void App::draw(Frame &frame) const
{
    // The page: navy to near-black, lit from the top left (Orbit's backdrop).
    frame.backdrop.mode = hui::gfx::BackdropMode::gradient;
    frame.backdrop.colors[0] = ui::color::night1;
    frame.backdrop.colors[1] = ui::color::night0;
    frame.backdrop.colors[2] = ui::Color::rgb(0x101010);
    frame.backdrop.params[0] = 0.08f;
    frame.backdrop.params[1] = 0.0f;
    frame.backdrop.params[2] = 1.0f;
    frame.backdrop.time = ctx_.clock;

    hui::gfx::DrawList &scene = frame.scene;
    // Place and section: a soft wash across the top, fading out by 45 percent of the height.
    scene.gradient_rect({0.0f, 0.0f, 1920.0f, 1080.0f * theme::kWashReach}, 0.0f, wash_.top(), wash_.bottom());
    const bool offer_on = tab_ != tab::settings && tab_ != tab::logs && !ctx_.store.state().online() &&
                          pegasus_offer_.visible();
    ctx_.offer_showing = offer_on;
    ctx_.offer_grace = offer_on && pegasus_offer_.in_grace();
    screen().draw(frame, area_ == Area::content);
    if (offer_on)
        pegasus_offer_.draw(ctx_.type, scene, ctx_.clock, area_ == Area::content);
    draw_top_bar(scene);

    hui::gfx::DrawList &overlay = frame.overlay;
    const ui::Type &type = ctx_.type;
    if (toast_time_ > 0.0f && !toast_.empty())
    {
        const float alpha = std::min(1.0f, toast_time_ / 0.3f) *
                            std::min(1.0f, (toast_length_ - toast_time_) / 0.2f);
        const float width = type.medium.measure(toast_, 23.0f) + 110.0f;
        const ui::Rect pill{960.0f - width * 0.5f, 900.0f, width, 64.0f};
        overlay.push_opacity(std::clamp(alpha, 0.0f, 1.0f));
        overlay.shadow({pill.x, pill.y + 10.0f, pill.w, pill.h}, 32.0f, 30.0f,
                       ui::Color::rgb(0x000000, 0.5f));
        overlay.rounded_rect(pill, 32.0f, ui::color::night3);
        overlay.circle(pill.x + 40.0f, pill.cy(), 16.0f, ui::color::ok);
        ui::icon_check(overlay, pill.x + 40.0f, pill.cy(), 16.0f, ui::color::night1, 3.0f);
        ui::text(overlay, type.medium, toast_, pill.x + 70.0f, pill.cy() + 8.0f, 23.0f,
                 ui::color::text);
        overlay.pop_opacity();
    }

    draw_capture(frame);

    // ---- controller hints, over a fade that lets content scroll beneath ----
    std::vector<hui::ui::Hint> items;
    if (capture_panel_open())
        items = {{hui::ui::Button::cross, tr("Select")}, {hui::ui::Button::circle, tr("Hide")}};
    else if (area_ == Area::top)
        items = {{hui::ui::Button::cross, tr("Select")},
                 {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    else if (offer_on && pegasus_offer_.takes_input())
        items = {{hui::ui::Button::cross, tr("Choose")},
                 {hui::ui::Button::l1, tr("Switch Tabs"), hui::ui::Button::r1}};
    else
        items = screen().hints();
    if (items.empty())
        return;
    overlay.gradient_rect({0.0f, 960.0f, 1920.0f, 50.0f}, 0.0f, ui::color::night0.with_alpha(0.0f),
                          ui::color::night0.with_alpha(0.95f));
    overlay.rounded_rect({0.0f, 1010.0f, 1920.0f, 70.0f}, 0.0f, ui::color::night0.with_alpha(0.95f));
    ui::hints(overlay, type, items);
}

} // namespace orbit
