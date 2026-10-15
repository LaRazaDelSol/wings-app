// Wings TV app - a stand-in for the Wings update helper over a fake HTTP transport (no sockets, no files).
// Each status read moves a job one step. Download: `step` bytes, verifying, downloaded. Install (only of a checked
// download, else 409 "not_downloaded"): verifying, placing, placed.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "orbit/http.hpp"
#include "orbit/json.hpp"
#include "orbit/wings_helper_client.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orbit
{

class FakeWingsHelper final : public http::Transport
{
  public:
    http::Response send(const http::Request &r) override
    {
        http::Response out;
        requests.push_back(r.method + " " + r.path);
        if (!running)
        {
            out.error = "connection refused";
            return out;
        }
        if (r.headers.find("X-Wings-Helper: 1\r\n") == std::string::npos)
        {
            out.status = 403;
            return out;
        }
        out.status = 200;
        const std::string route = r.path.substr(r.path.rfind('/') + 1);
        if (route == "status")
        {
            ++status_reads;
            if (silent_after >= 0 && status_reads > silent_after)
            {
                out.status = 0;
                out.error = "timed out";
                return out;
            }
            if (!frozen)
                advance();
            out.body = body();
        }
        else if (route == "download" || route == "install")
        {
            const bool install = route == "install";
            (install ? installs : downloads).push_back(r.body);
            const int answer = install ? answer_install : answer_download;
            if (answer != 0)
            {
                out.status = answer;
                out.body = answer == 409 ? "{\"error\":\"an earlier update is waiting\",\"reason\":\"old_waiting\"}" : "{\"error\":\"url is not the official one\",\"reason\":\"invalid\"}";
                return out;
            }
            if (phase == "downloading" || phase == "verifying" || phase == "placing")
            {
                out.status = 409;
                out.body = "{\"error\":\"An update is already being installed. Wait for it.\",\"reason\":\"busy\"}";
                return out;
            }
            json::Value v;
            (void)json::parse(r.body, &v);
            std::string asked = v.text("version");
            if (!asked.empty() && asked[0] == 'v')
                asked.erase(0, 1); // like the real helper: 0.1.2 for v0.1.2
            if (install && (downloaded_version.empty() || downloaded_version != asked))
            {
                // Like the real helper: install only places a checked download, it never downloads.
                out.status = 409;
                out.body = "{\"error\":\"Download the update first.\",\"reason\":\"not_downloaded\"}";
                return out;
            }
            version = asked;
            size = static_cast<std::uint64_t>(v.number("size"));
            error.clear();
            installing = install;
            received = install ? size : 0;
            phase = install ? "verifying" : "downloading"; // install hashes the downloaded file again
            if (!install)
                downloaded_version.clear();
            out.status = 202;
            out.body = body();
        }
        else if (route == "cancel")
        {
            ++cancels;
            if (phase == "downloading" || phase == "verifying")
            {
                phase = "idle";
                received = 0;
            }
        }
        else if (route == "stop")
        {
            if (phase == "placing")
                out.status = 409; // like the real helper: never stopped while renaming files
            else
                running = false;
        }
        else if (route == "tidy")
        {
            tidies.push_back(r.body);
            if (tidy_ok)
                old_kept = false;
            else
            {
                out.status = 409;
                out.body = "{\"error\":\"The new version isn't running yet.\",\"reason\":\"not_running\"}";
            }
        }
        return out;
    }

    // What the payload loader does with the ELF: the shipped build starts answering.
    WingsHelperSender sender()
    {
        return [this](const std::string &path, std::string *why)
        {
            sent.push_back(path);
            if (!loader_up)
            {
                *why = "Connection refused";
                return false;
            }
            running = true;
            build = shipped_build;
            frozen = false; // a fresh helper works its jobs
            // Like the real helper: a checked download (downloaded.json) survives a restart.
            phase = downloaded_version.empty() ? "idle" : "downloaded";
            if (!downloaded_version.empty())
                version = downloaded_version;
            return true;
        };
    }

    std::string body() const
    {
        return "{\"helper\":\"" + build + "\",\"api\":" + std::to_string(api) + ",\"phase\":\"" + phase + "\",\"version\":\"" + version +
               "\",\"received\":" + std::to_string(received) + ",\"size\":" + std::to_string(size) +
               ",\"error\":\"" + error + "\",\"target\":\"" + target + "\",\"installed_version\":\"" +
               installed_version + "\",\"old_kept\":" +
               (old_kept ? "true" : "false") + "}";
    }

    void advance()
    {
        if (phase == "downloading")
        {
            received = received + step >= size ? size : received + step;
            if (received >= size)
                phase = "verifying";
        }
        else if (phase == "verifying")
        {
            if (!fail_with.empty())
            {
                phase = "error";
                error = fail_with;
            }
            else if (installing)
                phase = "placing";
            else
            {
                phase = "downloaded";
                downloaded_version = version;
            }
        }
        else if (phase == "placing")
        {
            phase = "placed";
            old_kept = true;            // the file that was there is kept as .old
            target = "wings";           // like the real helper: its record now names what it placed
            installed_version = version;
            downloaded_version.clear(); // the download was moved into place
        }
    }

    bool frozen = false; // true: status reads do not move the phase on
    bool running = false;
    bool loader_up = true;
    int api = kWingsHelperApi; // 2, from app/helper/version.h
    std::string build = kWingsHelperBuild;
    std::string shipped_build = kWingsHelperBuild;
    std::string phase = "idle";
    std::string version;
    std::uint64_t received = 0;
    std::uint64_t size = 0;
    std::uint64_t step = 30000;
    std::string error;
    std::string fail_with;   // verifying ends in this error
    std::string target = "none";
    bool old_kept = false;
    std::string installed_version; // what the helper's record says is in /data/homebrew
    bool tidy_ok = true;           // false: /tidy refuses (the placed version is not running yet)
    std::string downloaded_version; // the checked download the helper holds ("" none), kept across restarts
    bool installing = false;        // the running job is an install (verifying leads to placing, not downloaded)
    int answer_install = 0;  // non-zero: /install answers this status
    int answer_download = 0; // non-zero: /download answers this status
    int silent_after = -1;   // >= 0: status reads after this many time out
    int status_reads = 0;
    int cancels = 0;
    std::vector<std::string> requests;
    std::vector<std::string> downloads;
    std::vector<std::string> installs;
    std::vector<std::string> tidies;
    std::vector<std::string> sent;
};

} // namespace orbit
