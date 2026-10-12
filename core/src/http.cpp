// Wings core - Minimal HTTP/1.1 client for Pegasus DL (adapted from the Orbit Store TV app).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wings/http.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
// Host-side builds on Windows (smoke tool). The PS5 / POSIX path is below.
#include <winsock2.h>
#include <ws2tcpip.h>
namespace
{
inline void close_socket(SOCKET fd) { closesocket(fd); }
inline bool would_block() { return WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK; }
inline bool refused() { return WSAGetLastError() == WSAECONNREFUSED; }
inline bool interrupted() { return false; }
struct WsaInit
{
    WsaInit() { WSADATA data; WSAStartup(MAKEWORD(2, 2), &data); }
};
} // namespace
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
namespace
{
using SOCKET = int;
inline void close_socket(SOCKET fd) { ::close(fd); }
inline bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline bool refused() { return errno == ECONNREFUSED; }
inline bool interrupted() { return errno == EINTR; }
} // namespace
#endif

namespace wings::http
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
    out += "\r\nConnection: close\r\nAccept: */*\r\nUser-Agent: WingsCore\r\n";
    // Mirrors fetch(): Content-Type is only sent with a body (JSON by default).
    // A bodiless POST still declares Content-Length: 0 so the server does not wait.
    if (!request.body.empty())
    {
        out += "Content-Type: ";
        out += request.content_type.empty() ? "application/json" : request.content_type;
        out += "\r\n";
    }
    if (!request.body.empty() || request.method != "GET")
    {
        out += "Content-Length: ";
        out += std::to_string(request.body.size());
        out += "\r\n";
    }
    out += "\r\n";
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
    #ifdef _WIN32
    static WsaInit wsa_init;
#endif
    const SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == static_cast<SOCKET>(-1))
    {
        response.error = "socket failed";
        return response;
    }
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(timeout_ms_);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = timeout_ms_ / 1000;
    timeout.tv_usec = (timeout_ms_ % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
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
        response.error = refused() ? "connection refused" : "connect failed";
        close_socket(fd);
        return response;
    }

    const std::string bytes = format_request(request, host_, port_);
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
#endif
        if (n <= 0)
        {
            if (n < 0 && interrupted())
                continue;
            response.error = "send failed";
            close_socket(fd);
            return response;
        }
        sent += static_cast<std::size_t>(n);
    }

    const std::size_t limit = (request.max_bytes != 0 ? request.max_bytes : kDefaultLimit) + 16384;
    std::string raw;
    char buffer[16384];
    for (;;)
    {
        const ssize_t n = recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (n == 0)
            break;
        if (n < 0)
        {
            if (interrupted())
                continue;
            response.error =
                would_block() ? "timed out" : "receive failed";
            close_socket(fd);
            return response;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        if (raw.size() > limit)
        {
            response.error = "reply too large";
            close_socket(fd);
            return response;
        }
    }
    close_socket(fd);
    parse_response(raw, &response);
    return response;
}

bool parse_response(std::string_view raw, Response *response)
{
    response->status = 0;
    response->body.clear();
    response->content_type.clear();
    response->retry_after = -1;
    const std::size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos || raw.substr(0, 7) != "HTTP/1.")
    {
        response->error = "invalid reply";
        return false;
    }
    const std::string_view head = raw.substr(0, head_end);
    std::string_view body = raw.substr(head_end + 4);

    const std::size_t status_end = head.find("\r\n");
    const std::string_view status_line = head.substr(0, status_end);
    const std::size_t space = status_line.find(' ');
    std::size_t status = 0;
    if (space == std::string_view::npos ||
        !parse_unsigned(status_line.substr(space + 1, 3), &status) || status < 100 || status > 599)
    {
        response->error = "invalid status line";
        return false;
    }

    bool chunked = false;
    bool have_length = false;
    std::size_t length = 0;
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
            have_length = parse_unsigned(value, &length);
        else if (iequals(name, "transfer-encoding"))
            chunked = iequals(value, "chunked");
        else if (iequals(name, "content-type"))
            response->content_type = std::string(value);
        else if (iequals(name, "retry-after"))
        {
            std::size_t seconds = 0;
            if (parse_unsigned(value, &seconds) && seconds < 86400)
                response->retry_after = static_cast<int>(seconds);
        }
    }

    if (chunked)
    {
        if (!dechunk(body, &response->body))
        {
            response->error = "truncated chunked reply";
            return false;
        }
    }
    else if (have_length)
    {
        if (body.size() < length)
        {
            response->error = "truncated reply";
            return false;
        }
        response->body.assign(body.substr(0, length));
    }
    else
    {
        response->body.assign(body);
    }
    response->status = static_cast<int>(status);
    response->error.clear();
    return true;
}

} // namespace wings::http
