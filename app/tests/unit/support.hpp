// Wings TV app - Shared test doubles: a scripted transport, a texture
// sink and the app's fonts loaded from assets/.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/font.hpp"
#include "orbit/art.hpp"
#include "orbit/http.hpp"
#include "orbit/thread.hpp"
#include "orbit/ui.hpp"
#include "ui/fonts.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace orbit::test
{

// Answers each request with a function of it, and records every request.
class ScriptedTransport final : public http::Transport
{
  public:
    std::function<http::Response(const http::Request &)> reply;
    std::vector<http::Request> seen;

    http::Response send(const http::Request &request) override
    {
        {
            Lock lock(mutex_);
            seen.push_back(request);
        }
        return reply ? reply(request) : http::Response{};
    }
    int count(const std::string &path)
    {
        Lock lock(mutex_);
        int n = 0;
        for (const http::Request &request : seen)
            n += request.path == path ? 1 : 0;
        return n;
    }

  private:
    Mutex mutex_;
};

inline http::Response json_reply(int status, std::string body)
{
    http::Response response;
    response.status = status;
    response.body = std::move(body);
    response.content_type = "application/json";
    return response;
}

class FakeSink final : public TextureSink
{
  public:
    std::uint32_t next = 1;
    std::vector<std::uint32_t> live;
    std::uint32_t create(const image::Pixels &pixels) override
    {
        if (pixels.empty())
            return 0;
        live.push_back(next);
        return next++;
    }
    void destroy(std::uint32_t texture) override
    {
        for (auto it = live.begin(); it != live.end(); ++it)
        {
            if (*it == texture)
            {
                live.erase(it);
                return;
            }
        }
        ADD_FAILURE() << "destroyed a texture that was not alive: " << texture;
    }
};

// The four Inter faces from assets/fonts, as the app loads them.
struct TestFonts
{
    hui::gfx::Font light;
    hui::gfx::Font regular;
    hui::gfx::Font medium;
    hui::gfx::Font semibold;
    ui::Type type;
    hui::ui::Fonts fonts;

    TestFonts()
    {
        load("inter-display-light.huifont", &light, &type.light);
        load("inter-regular.huifont", &regular, &type.regular);
        load("inter-medium.huifont", &medium, &type.medium);
        load("inter-semibold.huifont", &semibold, &type.semibold);
        fonts.regular = type.regular;
        fonts.semibold = type.semibold;
        fonts.display = type.light;
        fonts.mono = type.medium;
        fonts.pixel = type.regular;
        fonts.hand = type.regular;
    }

  private:
    static void load(const char *name, hui::gfx::Font *font, hui::ui::FontRef *ref)
    {
        std::ifstream file(std::string(HUI_SOURCE_DIR) + "/assets/fonts/" + name, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        if (!font->load(data))
            ADD_FAILURE() << "cannot load font " << name;
        ref->font = font;
        ref->texture = 1;
    }
};

} // namespace orbit::test
