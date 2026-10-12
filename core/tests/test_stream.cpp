// Wings core tests - array-wise catalogue reading (PegasusV1::streamLibrary, json::for_each_array_element).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace wings;

namespace
{

struct Collect final : json::ElementSink
{
    std::vector<std::string> items;
    std::size_t stop_after = 0;
    bool element(std::string_view text) override
    {
        items.emplace_back(text);
        return stop_after == 0 || items.size() < stop_after;
    }
};

struct Packages final : PackageSink
{
    std::vector<Package> all;
    void package(Package &&p) override { all.push_back(std::move(p)); }
};

} // namespace

TEST(scan_elements_respect_strings_and_nesting)
{
    Collect c;
    const std::string doc =
        R"({"x":[9,{"packages":1}], "packages" : [ {"a":"}],{\"q\"","b":[1,[2,{"c":null}]]} , "s]\\", 12.5e3, true, [], {} ] , "z":{"packages":[1]}})";
    CHECK(json::for_each_array_element(doc, "packages", c) == json::ScanResult::ok);
    CHECK_EQ_INT(c.items.size(), 6);
    if (c.items.size() == 6)
    {
        CHECK_EQ_STR(c.items[0], R"({"a":"}],{\"q\"","b":[1,[2,{"c":null}]]})");
        CHECK_EQ_STR(c.items[1], R"("s]\\")");
        CHECK_EQ_STR(c.items[2], "12.5e3");
        CHECK_EQ_STR(c.items[3], "true");
        CHECK_EQ_STR(c.items[4], "[]");
        CHECK_EQ_STR(c.items[5], "{}");
    }
}

TEST(scan_missing_empty_and_malformed)
{
    Collect c;
    CHECK(json::for_each_array_element(R"({"packages":[]})", "packages", c) == json::ScanResult::ok);
    CHECK(c.items.empty());
    CHECK(json::for_each_array_element("{}", "packages", c) == json::ScanResult::missing);
    CHECK(json::for_each_array_element(R"({"other":1})", "packages", c) == json::ScanResult::missing);
    CHECK(json::for_each_array_element(R"({"packages":5})", "packages", c) == json::ScanResult::missing);
    CHECK(json::for_each_array_element("[1,2]", "packages", c) == json::ScanResult::malformed);
    CHECK(json::for_each_array_element("", "packages", c) == json::ScanResult::malformed);
    CHECK(json::for_each_array_element(R"({"packages":[{"a":1})", "packages", c) == json::ScanResult::malformed);
    CHECK(json::for_each_array_element(R"({"packages":[1,,2]})", "packages", c) == json::ScanResult::malformed);
    CHECK(json::for_each_array_element(R"({"packages":[{"a":"unterminated}]})", "packages", c) == json::ScanResult::malformed);
    std::string deep(200, '[');
    CHECK(json::for_each_array_element("{\"packages\":[" + deep + "]}", "packages", c) == json::ScanResult::malformed);
}

TEST(scan_sink_can_stop_early)
{
    Collect c;
    c.stop_after = 2;
    CHECK(json::for_each_array_element(R"({"packages":[1,2,3,4]})", "packages", c) == json::ScanResult::ok);
    CHECK_EQ_INT(c.items.size(), 2);
}

TEST(stream_library_matches_get_library_on_the_real_sample)
{
    Rig r;
    CHECK(r.t->add_file("GET", "/api/library", sample("library.json")));
    auto whole = r.api.getLibrary();
    CHECK(whole.ok());
    Packages streamed;
    const double t0 = testing::now_ms();
    auto result = r.api.streamLibrary(streamed);
    const double ms = testing::now_ms() - t0;
    CHECK(result.ok());
    std::printf("    streamLibrary: %zu packages in %.0f ms\n", streamed.all.size(), ms);
    CHECK_EQ_INT(streamed.all.size(), whole.value.size());
    CHECK_EQ_STR(r.t->last().path, "/api/library");
    std::size_t n = std::min(streamed.all.size(), whole.value.size());
    bool same = true;
    for (std::size_t i = 0; i < n && same; ++i)
    {
        const Package &a = streamed.all[i];
        const Package &b = whole.value[i];
        same = a.id == b.id && a.title == b.title && a.titleId == b.titleId && a.sourceName == b.sourceName &&
               a.sizeBytes == b.sizeBytes && a.description == b.description && a.posterUrl == b.posterUrl &&
               a.downloadLinks.size() == b.downloadLinks.size();
        for (std::size_t k = 0; same && k < a.downloadLinks.size(); ++k)
            same = a.downloadLinks[k].url == b.downloadLinks[k].url && a.downloadLinks[k].name == b.downloadLinks[k].name &&
                   a.downloadLinks[k].hasIndex == b.downloadLinks[k].hasIndex;
        if (!same)
            std::printf("    first difference at package %zu (id %lld)\n", i, static_cast<long long>(a.id));
    }
    CHECK(same);
}

TEST(stream_library_errors_and_edge_replies)
{
    {
        Rig r;
        r.t->add("GET", "/api/library", 200, "{}");
        Packages p;
        CHECK(r.api.streamLibrary(p).ok());
        CHECK(p.all.empty());
    }
    {
        Rig r;
        r.t->add("GET", "/api/library", 200, "");
        Packages p;
        CHECK(r.api.streamLibrary(p).ok());
    }
    {
        Rig r;
        r.t->add("GET", "/api/library", 503, R"({"error":"catalog busy"})");
        Packages p;
        auto e = r.api.streamLibrary(p);
        CHECK(!e.ok());
        CHECK_EQ_INT(e.error.status, 503);
        CHECK_EQ_STR(e.error.message, "catalog busy");
    }
    {
        Rig r;
        r.t->add("GET", "/api/library", 200, R"({"packages":[{"id":1,"title":"a"},{"id":)");
        Packages p;
        auto e = r.api.streamLibrary(p);
        CHECK(!e.ok());
        CHECK(e.error.kind == ApiError::Kind::parse);
    }
    {
        Rig r; // unmatched route: Pegasus' own 404
        Packages p;
        auto e = r.api.streamLibrary(p);
        CHECK(e.error.route_missing());
    }
    {
        Rig r;
        r.t->add("GET", "/api/library", 200, R"({"packages":[{"id":7,"sourceId":2,"title":"Zed","downloadLinks":[{"name":"One","url":"http://h/x.pkg"},{"index":5,"url":"http://h/y"}]}]})");
        Packages p;
        CHECK(r.api.streamLibrary(p).ok());
        CHECK_EQ_INT(p.all.size(), 1);
        if (p.all.size() == 1)
        {
            CHECK_EQ_INT(p.all[0].id, 7);
            CHECK_EQ_INT(p.all[0].downloadLinks.size(), 2);
            CHECK(p.all[0].downloadLinks[1].hasIndex && p.all[0].downloadLinks[1].index == 5);
        }
    }
}
