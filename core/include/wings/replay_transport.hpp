// Wings core - Offline transport for tests: scripted responses + request log.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Responses are matched on "METHOD path" (path includes the query string).
// Several responses for the same key are served in order, the last one
// repeating. Unmatched requests get 404 {"error":"Route not found"}, the same
// thing Pegasus answers for an unknown route. Every request is recorded so
// tests can assert exact paths and bodies.

#pragma once

#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "wings/http.hpp"
#include "wings/json.hpp"
#include "wings/json_writer.hpp"

namespace wings::http
{

inline std::string read_text_file(const std::string &file, bool *ok)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        *ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *ok = true;
    return ss.str();
}

class ReplayTransport final : public Transport
{
  public:
    struct Recorded
    {
        std::string method;
        std::string path;
        std::string body;
        std::string content_type;
    };

    // Queue a response for "METHOD path".
    void add(const std::string &method, const std::string &path, int status, std::string body,
             std::string content_type = "application/json")
    {
        Response r;
        r.status = status;
        r.body = std::move(body);
        r.content_type = std::move(content_type);
        routes_[method + " " + path].push_back(std::move(r));
    }

    // Replace whatever is scripted for "METHOD path" (drops earlier queued responses).
    void set(const std::string &method, const std::string &path, int status, std::string body,
             std::string content_type = "application/json")
    {
        routes_.erase(method + " " + path);
        add(method, path, status, std::move(body), std::move(content_type));
    }

    // Queue a transport-level failure ("connection refused", ...).
    void add_failure(const std::string &method, const std::string &path, const std::string &error)
    {
        Response r;
        r.error = error;
        routes_[method + " " + path].push_back(std::move(r));
    }

    // Queue a response from a recorded sample. Handles both raw API bodies
    // (recon/samples/*.json) and the wrapped {"req":..,"status":..,"res":..}
    // form of recon/samples/mutating/*.json. Returns false if unreadable.
    bool add_file(const std::string &method, const std::string &path, const std::string &file, int status_if_raw = 200)
    {
        bool ok = false;
        const std::string text = read_text_file(file, &ok);
        if (!ok)
            return false;
        json::Value doc;
        if (json::parse(text, &doc) && doc.is_object() && doc.has("req") && doc.has("res") && doc.has("status"))
        {
            add(method, path, static_cast<int>(doc["status"].as_int(200)), json::dump(doc["res"]));
            return true;
        }
        add(method, path, status_if_raw, text);
        return true;
    }

    Response send(const Request &request) override
    {
        log_.push_back({request.method, request.path, request.body, request.content_type});
        auto it = routes_.find(request.method + " " + request.path);
        if (it == routes_.end() || it->second.empty())
        {
            Response r;
            r.status = 404;
            r.body = "{\"error\":\"Route not found\"}";
            r.content_type = "application/json";
            return r;
        }
        Response r = it->second.front();
        if (it->second.size() > 1)
            it->second.pop_front();
        return r;
    }

    const std::vector<Recorded> &log() const
    {
        return log_;
    }
    const Recorded &last() const
    {
        return log_.back();
    }
    void clear_log()
    {
        log_.clear();
    }
    // Number of recorded requests matching method + path.
    std::size_t count(const std::string &method, const std::string &path) const
    {
        std::size_t n = 0;
        for (const Recorded &r : log_)
            if (r.method == method && r.path == path)
                ++n;
        return n;
    }

  private:
    std::map<std::string, std::deque<Response>> routes_;
    std::vector<Recorded> log_;
};

} // namespace wings::http
