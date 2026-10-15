// Wings TV app - A minimal HTTP/1.1 client for the local Pegasus DL backend.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/http.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace orbit::http
{

namespace
{

bool parse_ipv4(const std::string &text, std::uint32_t *address)
{
    unsigned parts[4] = {};
    int part = 0;
    unsigned value = 0;
    bool digit = false;
    for (std::size_t i = 0; i <= text.size(); ++i)
    {
        const char c = i < text.size() ? text[i] : '.';
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + static_cast<unsigned>(c - '0');
            if (value > 255)
                return false;
            digit = true;
        }
        else if (c == '.' && digit && part < 4)
        {
            parts[part++] = value;
            value = 0;
            digit = false;
        }
        else
        {
            return false;
        }
    }
    if (part != 4)
        return false;
    *address = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

std::string_view trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

bool parse_unsigned(std::string_view text, std::size_t *out, int base = 10)
{
    text = trim(text);
    if (text.empty())
        return false;
    std::size_t value = 0;
    for (const char c : text)
    {
        int digit = -1;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else if (base == 16 && c == ';')
            break; // chunk extensions
        if (digit < 0)
            return false;
        if (value > (static_cast<std::size_t>(-1) - static_cast<std::size_t>(digit)) /
                        static_cast<std::size_t>(base))
            return false;
        value = value * static_cast<std::size_t>(base) + static_cast<std::size_t>(digit);
    }
    *out = value;
    return true;
}

bool dechunk(std::string_view body, std::string *out)
{
    out->clear();
    for (;;)
    {
        const std::size_t line_end = body.find("\r\n");
        if (line_end == std::string_view::npos)
            return false;
        std::size_t size = 0;
        if (!parse_unsigned(body.substr(0, line_end), &size, 16))
            return false;
        body.remove_prefix(line_end + 2);
        if (size == 0)
            return true;
        if (body.size() < size + 2)
            return false;
        out->append(body.substr(0, size));
        body.remove_prefix(size + 2);
    }
}

} // namespace

namespace
{

struct Head
{
    std::size_t status = 0;
    bool chunked = false;
    bool have_length = false;
    std::size_t length = 0;
};

// Reads the status line and the headers we care about. False: not an HTTP/1.x reply.
bool parse_head(std::string_view head, Head *out, Response *response)
{
    const std::size_t status_end = head.find("\r\n");
    const std::string_view status_line = head.substr(0, status_end);
    const std::size_t space = status_line.find(' ');
    if (head.substr(0, 7) != "HTTP/1." || space == std::string_view::npos ||
        !parse_unsigned(status_line.substr(space + 1, 3), &out->status) || out->status < 100 ||
        out->status > 599)
    {
        response->error = head.substr(0, 7) != "HTTP/1." ? "invalid reply" : "invalid status line";
        return false;
    }
    std::string_view rest =
        status_end == std::string_view::npos ? std::string_view() : head.substr(status_end + 2);
    while (!rest.empty())
    {
        const std::size_t end = rest.find("\r\n");
        const std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 2);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        const std::string_view name = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (iequals(name, "content-length"))
            out->have_length = parse_unsigned(value, &out->length);
        else if (iequals(name, "transfer-encoding"))
            out->chunked = iequals(value, "chunked");
        else if (iequals(name, "content-type"))
            response->content_type = std::string(value);
        else if (iequals(name, "retry-after"))
        {
            std::size_t seconds = 0;
            if (parse_unsigned(value, &seconds) && seconds < 86400)
                response->retry_after = static_cast<int>(seconds);
        }
    }
    return true;
}

void reset(Response *response)
{
    response->status = 0;
    response->body.clear();
    response->content_type.clear();
    response->retry_after = -1;
}

} // namespace

static bool take_response(std::string &&raw, Response *response)
{
    // A plain Content-Length body (the common case, and the 7 MB catalogue) is
    // moved out of the receive buffer instead of copied: halves the peak.
    const std::size_t head_end = raw.find("\r\n\r\n");
    if (head_end != std::string::npos)
    {
        Response probe;
        Head head;
        if (parse_head(std::string_view(raw).substr(0, head_end), &head, &probe) && !head.chunked &&
            head.have_length && raw.size() - (head_end + 4) >= head.length)
        {
            reset(response);
            response->content_type = std::move(probe.content_type);
            response->retry_after = probe.retry_after;
            raw.erase(0, head_end + 4);
            raw.resize(head.length);
            response->body = std::move(raw);
            response->status = static_cast<int>(head.status);
            response->error.clear();
            return true;
        }
    }
    return parse_response(raw, response);
}

SocketTransport::SocketTransport(std::string host, std::uint16_t port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms)
{
}

std::string format_request(const Request &request, std::string_view host, std::uint16_t port)
{
    std::string out;
    out.reserve(256 + request.body.size());
    out += request.method;
    out += ' ';
    out += request.path;
    out += " HTTP/1.1\r\nHost: ";
    out += host;
    out += ':';
    out += std::to_string(port);
    out += "\r\nConnection: close\r\nAccept: */*\r\nUser-Agent: WingsTV\r\n";
    if (!request.bearer.empty())
    {
        out += "Authorization: Bearer ";
        out += request.bearer;
        out += "\r\n";
    }
    out += request.headers; // extra "Name: value\r\n" lines, as given
    if (request.method != "GET")
    {
        // The backend accepts mutations only as JSON. No Origin header is sent:
        // it is checked only when present, and this client is not a browser.
        out += "Content-Type: application/json\r\nContent-Length: ";
        out += std::to_string(request.body.size());
        out += "\r\n";
    }
    out += "\r\n";
    if (request.method != "GET")
        out += request.body;
    return out;
}

Response SocketTransport::send(const Request &request)
{
    Response response;
    std::uint32_t address = 0;
    if (!parse_ipv4(host_, &address))
    {
        response.error = "invalid backend address";
        return response;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        response.error = "socket failed";
        return response;
    }
    // Per request (no shared state): the worker and the catalogue thread share this transport.
    const int limit_ms = request.timeout_ms > 0 ? request.timeout_ms : timeout_ms_;
    timeval timeout{};
    timeout.tv_sec = limit_ms / 1000;
    timeout.tv_usec = (limit_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port_);
    peer.sin_addr.s_addr = htonl(address);
    if (connect(fd, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer)) != 0)
    {
        response.error = errno == ECONNREFUSED ? "connection refused" : "connect failed";
        close(fd);
        return response;
    }

    const std::string bytes = format_request(request, host_, port_);
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            response.error = "send failed";
            close(fd);
            return response;
        }
        sent += static_cast<std::size_t>(n);
    }

    const std::size_t limit = (request.max_bytes != 0 ? request.max_bytes : kDefaultLimit) + 16384;
    std::string raw;
    bool sized = false; // the buffer was sized from Content-Length
    char buffer[16384];
    for (;;)
    {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0)
            break;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            response.error =
                errno == EAGAIN || errno == EWOULDBLOCK ? "timed out" : "receive failed";
            close(fd);
            return response;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        if (!sized)
        {
            // Allocate the whole reply once instead of doubling up to it.
            const std::size_t head_end = raw.find("\r\n\r\n");
            if (head_end != std::string::npos)
            {
                sized = true;
                Response scratch;
                Head head;
                if (parse_head(std::string_view(raw).substr(0, head_end), &head, &scratch) &&
                    head.have_length && head.length + head_end + 4 <= limit)
                    raw.reserve(head_end + 4 + head.length);
            }
        }
        if (raw.size() > limit)
        {
            response.error = "reply too large";
            close(fd);
            return response;
        }
    }
    close(fd);
    take_response(std::move(raw), &response);
    return response;
}


bool parse_response(std::string_view raw, Response *response)
{
    reset(response);
    const std::size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos || raw.substr(0, 7) != "HTTP/1.")
    {
        response->error = "invalid reply";
        return false;
    }
    Head head;
    if (!parse_head(raw.substr(0, head_end), &head, response))
        return false;
    const std::string_view body = raw.substr(head_end + 4);
    if (head.chunked)
    {
        if (!dechunk(body, &response->body))
        {
            response->error = "truncated chunked reply";
            return false;
        }
    }
    else if (head.have_length)
    {
        if (body.size() < head.length)
        {
            response->error = "truncated reply";
            return false;
        }
        response->body.assign(body.substr(0, head.length));
    }
    else
    {
        response->body.assign(body);
    }
    response->status = static_cast<int>(head.status);
    response->error.clear();
    return true;
}

} // namespace orbit::http
