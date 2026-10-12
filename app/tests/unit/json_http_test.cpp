// Wings TV app - JSON reading and HTTP framing.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/http.hpp"
#include "orbit/json.hpp"

#include <gtest/gtest.h>

#include <string>

namespace orbit
{
namespace
{

TEST(Json, ReadsTheShapesTheBackendSends)
{
    json::Value value;
    ASSERT_TRUE(
        json::parse(R"({"a":1,"b":[true,false,null],"c":{"d":"x\"y"},"e":-2.5e2})", &value));
    EXPECT_EQ(value["a"].as_int(), 1);
    ASSERT_EQ(value["b"].items().size(), 3u);
    EXPECT_TRUE(value["b"].items()[0].as_bool());
    EXPECT_TRUE(value["b"].items()[2].is_null());
    EXPECT_EQ(value["c"].text("d"), "x\"y");
    EXPECT_DOUBLE_EQ(value.number("e"), -250.0);
    EXPECT_TRUE(value["missing"].is_null());
    EXPECT_EQ(value.text("a"), "");
    EXPECT_EQ(value["b"]["nested"].as_string(), "");
}

TEST(Json, DecodesEscapesAndSurrogatePairsToUtf8)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"(["caf\u00e9","\u2019","\ud83d\ude80","a\/b\n"])", &value));
    EXPECT_EQ(value.items()[0].as_string(), "caf\xC3\xA9");
    EXPECT_EQ(value.items()[1].as_string(), "\xE2\x80\x99");
    EXPECT_EQ(value.items()[2].as_string(), "\xF0\x9F\x9A\x80");
    EXPECT_EQ(value.items()[3].as_string(), "a/b\n");
}

TEST(Json, KeepsLargeByteCountsExact)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"({"sizeBytes":61700000000,"received":4294967297})", &value));
    EXPECT_EQ(value["sizeBytes"].as_int(), 61700000000LL);
    EXPECT_EQ(value["received"].as_int(), 4294967297LL);
}

TEST(Json, RejectsMalformedAndHostileDocuments)
{
    json::Value value;
    std::string error;
    for (const char *bad : {"", "{", "[1,]", "{\"a\" 1}", "tru", "\"\\ud800\"", "01x", "[1] 2",
                            "\"a\nb\"", "{\"a\":\"\\q\"}"})
    {
        EXPECT_FALSE(json::parse(bad, &value, &error)) << bad;
        EXPECT_FALSE(error.empty()) << bad;
    }
    const std::string deep(200, '[');
    EXPECT_FALSE(json::parse(deep + std::string(200, ']'), &value));
}

TEST(Json, QuotesStringsForRequestBodies)
{
    EXPECT_EQ(json::quote("plain"), "\"plain\"");
    EXPECT_EQ(json::quote("a\"b\\c\n\x01"), "\"a\\\"b\\\\c\\n\\u0001\"");
    json::Value value;
    ASSERT_TRUE(json::parse(json::quote("round \"trip\"\t"), &value));
    EXPECT_EQ(value.as_string(), "round \"trip\"\t");
}

TEST(Http, FormatsGetsAndJsonPostsWithoutAnOrigin)
{
    http::Request get;
    get.path = "/api/health";
    const std::string g = http::format_request(get, "127.0.0.1", 6970);
    EXPECT_EQ(g.rfind("GET /api/health HTTP/1.1\r\nHost: 127.0.0.1:6970\r\n", 0), 0u);
    EXPECT_EQ(g.find("Authorization"), std::string::npos);
    EXPECT_EQ(g.find("Content-Length"), std::string::npos);

    http::Request post;
    post.method = "POST";
    post.path = "/api/v1/downloads";
    post.body = "{\"a\":1}";
    post.bearer = "abc";
    const std::string p = http::format_request(post, "127.0.0.1", 6970);
    EXPECT_NE(p.find("Authorization: Bearer abc\r\n"), std::string::npos);
    EXPECT_NE(p.find("Content-Type: application/json\r\nContent-Length: 7\r\n"), std::string::npos);
    EXPECT_EQ(p.find("Origin"), std::string::npos);
    EXPECT_EQ(p.substr(p.size() - 7), "{\"a\":1}");
}

TEST(Http, ParsesLengthChunkedAndRetryAfterReplies)
{
    http::Response response;
    ASSERT_TRUE(http::parse_response(
        "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\nRetry-After: 3\r\n"
        "Content-Length: 4\r\n\r\nbodyEXTRA",
        &response));
    EXPECT_EQ(response.status, 202);
    EXPECT_EQ(response.body, "body");
    EXPECT_EQ(response.retry_after, 3);
    EXPECT_EQ(response.content_type, "application/json");

    ASSERT_TRUE(http::parse_response("HTTP/1.1 200 OK\r\ntransfer-encoding: "
                                     "chunked\r\n\r\n4\r\nWiki\r\n5;x=y\r\npedia\r\n0\r\n\r\n",
                                     &response));
    EXPECT_EQ(response.body, "Wikipedia");

    ASSERT_TRUE(http::parse_response("HTTP/1.0 404 Not Found\r\n\r\nuntil close", &response));
    EXPECT_EQ(response.status, 404);
    EXPECT_EQ(response.body, "until close");
}

TEST(Http, RejectsTruncatedOrForeignReplies)
{
    http::Response response;
    EXPECT_FALSE(http::parse_response("", &response));
    EXPECT_FALSE(http::parse_response("SSH-2.0-OpenSSH\r\n\r\n", &response));
    EXPECT_FALSE(
        http::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", &response));
    EXPECT_FALSE(http::parse_response(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWi", &response));
    EXPECT_FALSE(http::parse_response("HTTP/1.1 999 Odd\r\n\r\n", &response));
    EXPECT_FALSE(response.error.empty());
}

TEST(Http, ReportsARefusedConnectionAsUnreachable)
{
    // Port 1 on loopback is closed in the test environment.
    http::SocketTransport transport("127.0.0.1", 1, 500);
    http::Request request;
    request.path = "/api/health";
    const http::Response response = transport.send(request);
    EXPECT_EQ(response.status, 0);
    EXPECT_FALSE(response.error.empty());
    http::SocketTransport bad("not-an-address", 6970, 500);
    EXPECT_EQ(bad.send(request).error, "invalid backend address");
}

} // namespace
} // namespace orbit
