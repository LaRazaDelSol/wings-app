// Wings core tests - HTTP/1.1 wire format matches what fetch()/XHR send.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_harness.hpp"
#include "wings/http.hpp"

using namespace wings;

TEST(wire_format_matches_fetch)
{
    http::Request post;
    post.method = "POST";
    post.path = "/api/queue";
    post.body = R"({"packageId":1,"linkIndex":0})";
    const std::string w = http::format_request(post, "10.0.0.113", 6970);
    CHECK(w.rfind("POST /api/queue HTTP/1.1\r\nHost: 10.0.0.113:6970\r\n", 0) == 0);
    CHECK(w.find("Content-Type: application/json\r\n") != std::string::npos);
    CHECK(w.find("Content-Length: " + std::to_string(post.body.size()) + "\r\n") != std::string::npos);
    CHECK(w.size() >= post.body.size() && w.substr(w.size() - post.body.size()) == post.body);
    CHECK(w.find("\r\n\r\n") == w.size() - post.body.size() - 4);

    http::Request bodiless;
    bodiless.method = "POST";
    bodiless.path = "/api/queue/clear-finished";
    const std::string b = http::format_request(bodiless, "h", 1);
    CHECK(b.find("Content-Type") == std::string::npos); // fetch() sends none without a body (app.js:604)
    CHECK(b.find("Content-Length: 0\r\n") != std::string::npos);

    http::Request get;
    get.path = "/api/health";
    const std::string g = http::format_request(get, "h", 1);
    CHECK(g.find("Content-Length") == std::string::npos && g.find("Content-Type") == std::string::npos);

    http::Request up;
    up.method = "PUT";
    up.path = "/api/files/upload?directory=%2Fa&name=b";
    up.body = "xyz";
    up.content_type = "application/octet-stream";
    CHECK(http::format_request(up, "h", 1).find("Content-Type: application/octet-stream\r\n") != std::string::npos);
}

TEST(link_start_posts_get_the_long_timeout)
{
    for (const char *path : {"/api/queue", "/api/provider/resolve", "/api/browser/open", "/api/debrid/queue",
                             "/api/queue/direct", "/api/resolve/probe", "/api/queue?x=1"})
        CHECK(http::timeout_for("POST", path) == http::kLinkStartTimeoutMs);
    CHECK(http::kLinkStartTimeoutMs == 30000);
    // Polls and every other call keep the transport's own limit.
    CHECK(http::timeout_for("GET", "/api/queue") == 0);
    CHECK(http::timeout_for("GET", "/api/provider/resolve") == 0);
    CHECK(http::timeout_for("POST", "/api/queue/pause") == 0);
    CHECK(http::timeout_for("POST", "/api/queue/clear-finished") == 0);
    CHECK(http::timeout_for("POST", "/api/settings") == 0);
    CHECK(http::timeout_for("GET", "/api/capture/status?sessionId=7") == 0);
    CHECK(http::timeout_for("POST", "/api/queuex") == 0);
    http::Request plain;
    CHECK(plain.timeout_ms == 0);
}

TEST(wire_response_parsing)
{
    http::Response res;
    const std::string raw =
        "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: 17\r\n\r\n{\"error\":\"x y z\"}";
    CHECK(http::parse_response(raw, &res));
    CHECK(res.status == 503 && res.body == "{\"error\":\"x y z\"}" && res.content_type == "application/json");
    const std::string chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    CHECK(http::parse_response(chunked, &res) && res.body == "hello world" && res.status == 200);
    CHECK(!http::parse_response("garbage", &res));
    CHECK(!res.error.empty());
    const std::string truncated = "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort";
    CHECK(!http::parse_response(truncated, &res));
}
