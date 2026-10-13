// Wings TV app - Lets the Pegasus adapter (wings/core) talk through the app's own HTTP client.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// wings/core ships a SocketTransport of its own, but the app already has one
// that is proven on the console (orbit::http::SocketTransport: blocking BSD
// sockets, numeric IPv4, one connection per request). This small class
// presents it to the adapter as a wings::http::Transport, so the console build
// has exactly one socket implementation.
//
// Limits of the app's client, enforced here with a clear error rather than
// silently misbehaving: every body is sent as application/json, and replies
// larger than kMaxReply are refused (the app heap is a fixed 128 MiB, and the
// 7 MB catalogue needs far less). The file manager's raw PUT upload therefore
// is not available through this transport.

#pragma once

#include "orbit/http.hpp"
#include "wings/http.hpp"

#include <algorithm>
#include <utility>

namespace orbit
{

class CoreTransport final : public wings::http::Transport
{
  public:
    // The largest reply accepted, whatever the caller asks for.
    static constexpr std::size_t kMaxReply = 48u << 20;

    explicit CoreTransport(http::Transport &transport) : transport_(transport)
    {
    }

    wings::http::Response send(const wings::http::Request &request) override
    {
        wings::http::Response out;
        const bool json_body = request.content_type.empty() ||
                               request.content_type.rfind("application/json", 0) == 0;
        if (request.method == "PUT" || (!request.body.empty() && !json_body))
        {
            out.error = "request type not supported by this transport";
            return out;
        }
        http::Request mine;
        mine.method = request.method;
        mine.path = request.path;
        mine.body = request.body;
        mine.max_bytes = request.max_bytes == 0 ? http::kDefaultLimit
                                                 : std::min(request.max_bytes, kMaxReply);
        mine.timeout_ms = request.timeout_ms;
        http::Response reply = transport_.send(mine);
        out.status = reply.status;
        out.body = std::move(reply.body);
        out.content_type = std::move(reply.content_type);
        out.retry_after = reply.retry_after;
        out.error = std::move(reply.error);
        return out;
    }

  private:
    http::Transport &transport_;
};

} // namespace orbit
