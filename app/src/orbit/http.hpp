// Wings TV app - A minimal HTTP/1.1 client for the local Pegasus DL backend.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app only ever talks to Pegasus DL on the console's loopback
// address, in plain HTTP, one request per connection. Calls block, so they
// run on worker threads (orbit/worker.hpp), never on the frame.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace orbit::http
{

struct Request
{
    std::string method = "GET";
    std::string path;          // "/api/v1/system"
    std::string body;          // JSON for POST
    std::string bearer;        // pairing token, sent as Authorization
    std::size_t max_bytes = 0; // reply size limit; 0 = kDefaultLimit
    int timeout_ms = 0;        // socket time limit for this request; 0 = the transport's own
};

struct Response
{
    int status = 0; // HTTP status; 0 when the request never completed
    std::string body;
    std::string content_type;
    int retry_after = -1; // seconds from Retry-After, -1 when absent
    std::string error;    // transport failure ("connection refused", ...)

    bool transported() const
    {
        return error.empty() && status != 0;
    }
};

constexpr std::size_t kDefaultLimit = 8u << 20;

// What sends requests. The console and the PC preview use SocketTransport;
// tests and the fixture preview substitute their own.
class Transport
{
  public:
    virtual ~Transport() = default;
    virtual Response send(const Request &request) = 0;
};

class SocketTransport final : public Transport
{
  public:
    // host is a dotted IPv4 address; the app uses numeric hosts only.
    SocketTransport(std::string host, std::uint16_t port, int timeout_ms = 8000);
    Response send(const Request &request) override;

  private:
    std::string host_;
    std::uint16_t port_;
    int timeout_ms_;
};

// Builds the bytes of a request (exposed for tests).
std::string format_request(const Request &request, std::string_view host, std::uint16_t port);

// Parses a complete reply read until the server closed the connection.
// Handles Content-Length and chunked bodies. Returns false (with
// response->error set) when the bytes are not a valid HTTP/1.x reply.
bool parse_response(std::string_view raw, Response *response);

} // namespace orbit::http
