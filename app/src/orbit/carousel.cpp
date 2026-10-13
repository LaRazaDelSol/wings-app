// Wings TV app - The Store's curated carousel: the pure rules (carousel.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/carousel.hpp"

#include <algorithm>

namespace orbit
{

std::vector<int> resolve_carousel(const std::vector<std::string> &ids, const std::vector<GameGroup> &games)
{
    std::vector<int> out;
    for (const std::string &id : ids)
    {
        if (out.size() >= kCarouselMaxItems)
            break;
        for (std::size_t g = 0; g < games.size(); ++g)
        {
            const std::vector<std::string> &have = games[g].title_ids;
            if (std::find(have.begin(), have.end(), id) == have.end())
                continue;
            if (std::find(out.begin(), out.end(), static_cast<int>(g)) == out.end())
                out.push_back(static_cast<int>(g));
            break;
        }
    }
    return out;
}

bool carousel_shown(bool pref_on, std::size_t resolved, bool query_empty, std::size_t tiles)
{
    return pref_on && resolved > 0 && query_empty && tiles > 0;
}

std::vector<int> pick_most_popular(const std::vector<long> &keys, std::size_t count)
{
    std::vector<int> order;
    bool any = false;
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        order.push_back(static_cast<int>(i));
        any = any || keys[i] >= 0;
    }
    if (any)
        std::stable_sort(order.begin(), order.end(), [&keys](int a, int b)
                         { return keys[static_cast<std::size_t>(a)] > keys[static_cast<std::size_t>(b)]; });
    if (order.size() > count)
        order.resize(count);
    return order;
}

int carousel_wrap(int slide, int delta, int count)
{
    if (count <= 0)
        return 0;
    const int at = (slide + delta) % count;
    return at < 0 ? at + count : at;
}

CarouselStep carousel_step(int slide, int button, int count, hui::Direction nav)
{
    CarouselStep step;
    step.slide = slide;
    step.button = button;
    if (nav == hui::Direction::right)
    {
        if (button == kCarouselView)
        {
            step.button = kCarouselFavourite;
            step.moved = true;
        }
        else if (count > 1)
        {
            step.slide = carousel_wrap(slide, 1, count);
            step.moved = step.slide_changed = true;
        }
    }
    else if (nav == hui::Direction::left)
    {
        if (button == kCarouselFavourite)
        {
            step.button = kCarouselView;
            step.moved = true;
        }
        else if (count > 1)
        {
            step.slide = carousel_wrap(slide, -1, count);
            step.moved = step.slide_changed = true;
        }
    }
    return step;
}

bool CarouselClock::update(float dt, bool paused, int count)
{
    if (paused || count < 2)
    {
        elapsed_ = 0.0f;
        return false;
    }
    elapsed_ += dt;
    if (elapsed_ < kCarouselAdvanceS)
        return false;
    elapsed_ = 0.0f;
    return true;
}

float grid_scroll_target(int row, float pitch, float extra)
{
    return extra + std::max(0.0f, static_cast<float>(row - 1) * pitch);
}

float carousel_fade(float scroll)
{
    return std::clamp(1.0f - scroll / (kCarouselExtra * 0.8f), 0.0f, 1.0f);
}

} // namespace orbit
