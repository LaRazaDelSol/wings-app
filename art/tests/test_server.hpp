// Wings cover-art tests: a small TLS HTTP/1.1 fixture server (host only).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fixture
{

struct Reply
{
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool chunked = false;
    bool omit_length = false;      // body runs to connection close
    bool close = false;            // "Connection: close" and close afterwards
    bool close_silently = false;   // keep-alive advertised, then close anyway (stale pooled connection)
    int delay_ms = 0;              // before answering
    int stall_ms = 0;              // between the headers and the body
    std::string raw;               // when set: send exactly this instead of a formatted reply
};

class Server
{
  public:
    using Handler = std::function<Reply(const std::string &path)>;

    Server(std::string cert, std::string key, Handler h)
        : cert_(std::move(cert)), key_(std::move(key)), handler_(std::move(h))
    {
    }
    ~Server() { stop(); }

    bool start()
    {
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_ || SSL_CTX_use_certificate_chain_file(ctx_, cert_.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx_, key_.c_str(), SSL_FILETYPE_PEM) != 1)
            return false;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(listen_fd_, 64) != 0)
            return false;
        socklen_t len = sizeof a;
        getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&a), &len);
        port_ = ntohs(a.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop()
    {
        if (stopping_.exchange(true))
            return;
        if (acceptor_.joinable())
            acceptor_.join();
        {
            std::lock_guard<std::mutex> l(m_);
            for (int fd : live_)
                ::shutdown(fd, SHUT_RDWR);
        }
        for (auto &t : conns_)
            t.join();
        if (listen_fd_ >= 0)
            ::close(listen_fd_);
        if (ctx_)
            SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        listen_fd_ = -1;
    }

    int port() const { return port_; }
    int accepted() const { return accepted_.load(); }
    int requests() const { return requests_.load(); }
    std::vector<std::string> log() const
    {
        std::lock_guard<std::mutex> l(m_);
        return log_;
    }
    int count(const std::string &path) const
    {
        int n = 0;
        for (const std::string &p : log())
            n += p == path;
        return n;
    }

  private:
    void accept_loop()
    {
        while (!stopping_.load())
        {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0)
                continue;
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0)
                continue;
            ++accepted_;
            std::lock_guard<std::mutex> l(m_);
            live_.insert(fd);
            conns_.emplace_back([this, fd] { serve(fd); });
        }
    }

    static bool send_all(SSL *ssl, const std::string &s)
    {
        std::size_t off = 0;
        while (off < s.size())
        {
            const int n = SSL_write(ssl, s.data() + off, static_cast<int>(s.size() - off));
            if (n <= 0)
                return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    void sleep_interruptible(int ms)
    {
        for (int i = 0; i < ms / 10 && !stopping_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void serve(int fd)
    {
        SSL *ssl = SSL_new(ctx_);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) == 1)
        {
            std::string buf;
            bool alive = true;
            while (alive && !stopping_.load())
            {
                std::size_t end;
                while ((end = buf.find("\r\n\r\n")) == std::string::npos)
                {
                    char tmp[4096];
                    const int n = SSL_read(ssl, tmp, sizeof tmp);
                    if (n <= 0)
                    {
                        alive = false;
                        break;
                    }
                    buf.append(tmp, static_cast<std::size_t>(n));
                }
                if (!alive)
                    break;
                const std::string head = buf.substr(0, end);
                buf.erase(0, end + 4);
                const std::size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
                const std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);
                {
                    std::lock_guard<std::mutex> l(m_);
                    log_.push_back(path);
                    last_head_ = head;
                }
                ++requests_;
                Reply r = handler_(path);
                sleep_interruptible(r.delay_ms);
                if (!r.raw.empty())
                {
                    send_all(ssl, r.raw);
                    break;
                }
                std::string out = "HTTP/1.1 " + std::to_string(r.status) + " X\r\n";
                bool has_len = false;
                for (auto &h : r.headers)
                {
                    out += h.first + ": " + h.second + "\r\n";
                    if (h.first == "Content-Length")
                        has_len = true;
                }
                if (r.chunked)
                    out += "Transfer-Encoding: chunked\r\n";
                else if (!r.omit_length && !has_len)
                    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
                if (r.close || r.omit_length)
                    out += "Connection: close\r\n";
                out += "\r\n";
                if (!send_all(ssl, out))
                    break;
                sleep_interruptible(r.stall_ms);
                if (r.chunked)
                {
                    for (std::size_t i = 0; i < r.body.size(); i += 1000)
                    {
                        const std::string part = r.body.substr(i, 1000);
                        char hex[16];
                        std::snprintf(hex, sizeof hex, "%zx\r\n", part.size());
                        if (!send_all(ssl, hex + part + "\r\n"))
                            break;
                    }
                    send_all(ssl, "0\r\n\r\n");
                }
                else
                    send_all(ssl, r.body);
                if (r.close || r.omit_length || r.close_silently)
                    break;
            }
        }
        SSL_set_quiet_shutdown(ssl, 1);
        SSL_free(ssl);
        ::close(fd);
        std::lock_guard<std::mutex> l(m_);
        live_.erase(fd);
    }

    std::string cert_, key_;
    Handler handler_;
    SSL_CTX *ctx_ = nullptr;
    int listen_fd_ = -1, port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0}, requests_{0};
    std::thread acceptor_;
    std::vector<std::thread> conns_;
    mutable std::mutex m_;
    std::set<int> live_;
    std::vector<std::string> log_;
    std::string last_head_;
};

} // namespace fixture
