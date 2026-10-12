// Wings core - Transport abstraction and a minimal HTTP/1.1 client for Pegasus DL.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from the Orbit Store TV app's http.hpp (GPL-3.0-or-later). The
// adapter only depends on the abstract Transport, so it can be driven by the
// SocketTransport (console loopback / PC), by the ReplayTransport in tests,
// or by any platform HTTP stack.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wings::http
{

struct Request
{
    std::string method = "GET";
    std::string path;                 // path + query, e.g. "/api/files/list?path=%2Fdata"
    std::string body;                 // raw body (JSON text, or raw file bytes for upload)
    std::string content_type;         // sent only when a body is present; "" => application/json
    std::size_t max_bytes = 0;        // reply size limit; 0 = kDefaultLimit
};

struct Response
{
    int status = 0; // HTTP status; 0 when the request never completed
    std::string body;
    std::string content_type;
    int retry_after = -1;
    std::string error; // transport failure ("connection refused", ...)

    bool transported() const
    {
        return error.empty() && status != 0;
    }
};

constexpr std::size_t kDefaultLimit = 8u << 20;
constexpr std::size_t kLibraryLimit = 128u << 20; // /api/library is ~7 MB today

class Transport
{
  public:
    virtual ~Transport() = default;
    virtual Response send(const Request &request) = 0;
};

// Plain-HTTP, one request per connection, numeric IPv4 host only.
class SocketTransport final : public Transport
{
  public:
    SocketTransport(std::string host, std::uint16_t port, int timeout_ms = 15000);
    Response send(const Request &request) override;

  private:
    std::string host_;
    std::uint16_t port_;
    int timeout_ms_;
};

// Exposed for tests.
std::string format_request(const Request &request, std::string_view host, std::uint16_t port);
bool parse_response(std::string_view raw, Response *response);

} // namespace wings::http
