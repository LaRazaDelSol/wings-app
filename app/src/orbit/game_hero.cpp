// Wings TV app - The game page's hero: see game_hero.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/game_hero.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>

namespace orbit::hero
{

namespace
{

std::string joined(const std::vector<std::string> &items, std::size_t most)
{
    std::string out;
    for (std::size_t i = 0; i < items.size() && i < most; ++i)
        out += (out.empty() ? "" : ", ") + items[i];
    if (items.size() > most)
        out += " +" + std::to_string(items.size() - most);
    return out;
}

} // namespace

std::vector<Chip> chips(const HeaderFacts &f)
{
    std::vector<Chip> out;
    const auto add = [&](std::string text, Tone tone = Tone::plain, LinkTier tier = LinkTier::unknown,
                         FwFit fit = FwFit::runs)
    {
        if (text.empty() || out.size() >= kMaxChips)
            return;
        out.push_back({std::move(text), tone, tier, fit});
    };
    add(!f.region.empty() ? f.region : joined(f.regions, 2));
    if (f.fit_known)
    {
        switch (f.fit)
        {
        case FwFit::runs:
            add(tr("Runs on your firmware"), Tone::fit, LinkTier::unknown, f.fit);
            break;
        case FwFit::too_new:
            add(f.firmware.empty() ? std::string(tr("Needs newer firmware"))
                                   : std::string(tr("Needs")) + " " + f.firmware,
                Tone::fit, LinkTier::unknown, f.fit);
            break;
        case FwFit::unknown:
            add(tr("Firmware unclear"), Tone::fit, LinkTier::unknown, f.fit);
            break;
        }
    }
    else if (!f.firmware.empty())
    {
        add(std::string(tr("Firmware")) + " " + f.firmware);
    }
    add(f.format);
    if (f.have_download && f.tier != LinkTier::unknown)
        add(tier_label(f.tier), Tone::tier, f.tier);
    return out;
}

int chips_that_fit(const std::vector<float> &widths, float max_width, float gap)
{
    float x = 0.0f;
    int n = 0;
    for (const float w : widths)
    {
        const float next = x + (n > 0 ? gap : 0.0f) + w;
        if (next > max_width)
            break;
        x = next;
        ++n;
    }
    return n;
}

std::vector<Fact> fact_rows(const HeaderFacts &f)
{
    std::vector<Fact> rows;
    const auto add = [&](Icon icon, const char *label, std::string value)
    {
        if (value.empty() || rows.size() >= kMaxFactRows)
            return;
        Fact row;
        row.icon = icon;
        row.label = label;
        row.value = std::move(value);
        rows.push_back(std::move(row));
    };
    std::string source = joined(f.sources, 2);
    if (source.empty())
        source = f.source;
    add(Icon::source, f.sources.size() > 1 ? tr("Sources") : tr("Source"), std::move(source));
    add(Icon::size, tr("Size"), f.size_text);
    add(Icon::version, tr("Version"), f.version);
    {
        std::string firmware = f.firmware.empty() ? f.firmware_note : f.firmware;
        const std::size_t before = rows.size();
        add(Icon::firmware, tr("Firmware"), std::move(firmware));
        if (rows.size() > before && f.fit_known)
        {
            Fact &row = rows.back();
            row.tone = Tone::fit;
            row.fit = f.fit;
            row.accent = f.fit == FwFit::runs      ? tr("Fits")
                         : f.fit == FwFit::too_new ? tr("Needs newer")
                                                   : tr("Unclear");
        }
    }
    add(Icon::region, f.regions.size() > 1 ? tr("Regions") : tr("Region"),
        !f.regions.empty() ? joined(f.regions, 3) : f.region);
    add(Icon::files, tr("Files"), f.parts);
    if (f.link_count > 0)
    {
        Fact row;
        row.icon = Icon::links;
        row.label = tr("Links");
        row.value = trn(f.link_count, "{count} link", "{count} links");
        if (f.have_download && f.tier != LinkTier::unknown)
        {
            row.accent = tier_label(f.tier);
            row.tone = Tone::tier;
            row.tier = f.tier;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool has_links_row(const std::vector<Fact> &rows)
{
    return !rows.empty() && rows.back().icon == Icon::links;
}

int rows_above_divider(const std::vector<Fact> &rows)
{
    return static_cast<int>(rows.size()) - (has_links_row(rows) ? 1 : 0);
}

namespace
{

// Top of the facts panel (it sits on the buttons' bottom and is as tall as its rows); 0 when none.
float panel_top(int rows, bool links_row)
{
    if (rows <= 0 && !links_row)
        return 0.0f;
    float h = 2.0f * kPanelPad + static_cast<float>(rows) * kRowPitch;
    if (links_row)
        h += kDividerGap + kRowPitch;
    return kButtonsTop + kButtonH - h;
}

} // namespace

float title_width(float width, int rows, bool links_row)
{
    const float title_x = ui::kGutter + kIconSize + kIconGap;
    const float top = panel_top(rows, links_row);
    float right = width - ui::kGutter;
    if (top > 0.0f && top < kTitleBlockBottom)
        right = width - ui::kGutter - kPanelW - 30.0f; // the panel's left edge, with room
    return right - title_x;
}

Layout layout(float width, bool two_line_title, bool chips_shown, bool rating, bool offer, bool why, int rows,
              bool links_row)
{
    Layout l;
    const float gutter = ui::kGutter;
    l.icon = {gutter, kIconTop, kIconSize, kIconSize};
    l.title_x = gutter + kIconSize + kIconGap;
    l.title_w = title_width(width, rows, links_row);
    l.title_lines = two_line_title ? 2 : 1;
    l.title_size = two_line_title ? 46.0f : 64.0f;
    // The title block (title, then the meta line) is centred on the cover's middle or a little above.
    l.title_baseline[0] = two_line_title ? 296.0f : 336.0f;
    l.title_baseline[1] = 350.0f;
    l.meta_baseline = two_line_title ? 402.0f : 388.0f;

    l.stack_x = gutter;
    const bool panel = rows > 0 || links_row;
    l.stack_w = (panel ? width - gutter - kPanelW - kPanelGap : width - gutter) - l.stack_x;

    // The left stack, anchored on the buttons and built upwards.
    l.buttons_top = kButtonsTop;
    float y = kButtonsTop - 28.0f;
    if (why)
    {
        l.why_baseline = y;
        y -= 42.0f;
    }
    if (offer)
    {
        l.offer_baseline = y;
        y -= 62.0f;
    }
    if (rating)
    {
        l.rating_baseline = y;
        y -= 62.0f;
    }
    if (chips_shown)
        l.chips_cy = y;

    // The panel: bottom on the buttons' bottom, as tall as its rows.
    if (panel)
    {
        float h = 2.0f * kPanelPad + static_cast<float>(rows) * kRowPitch;
        if (links_row)
            h += kDividerGap + kRowPitch;
        const float bottom = kButtonsTop + kButtonH;
        l.panel = {width - gutter - kPanelW, bottom - h, kPanelW, h};
        l.first_row_cy = l.panel.y + kPanelPad + kRowPitch * 0.5f;
        if (links_row)
            l.divider_y = l.panel.y + kPanelPad + static_cast<float>(rows) * kRowPitch + kDividerGap * 0.5f;
    }
    l.label_baseline = kLabelBaseline;
    l.list_top = kListTop;
    return l;
}

} // namespace orbit::hero
