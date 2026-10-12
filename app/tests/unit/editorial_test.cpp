// Wings TV app - Editorial data: the notes and carousel files, their checks, the hub and the fetch service.
// No test uses the network.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/editorial.hpp"
#include "orbit/prefs.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>

namespace orbit
{
namespace
{

std::string tmp_dir()
{
    char path[] = "/tmp/wings-edit-XXXXXX";
    return mkdtemp(path) != nullptr ? std::string(path) : std::string("/tmp");
}

std::string notes_doc(const std::string &entries, const std::string &version = "1")
{
    return "{\"schemaVersion\":" + version + ",\"notes\":{" + entries + "}}";
}

std::string curated_doc(const std::string &items, const std::string &version = "1")
{
    return "{\"schemaVersion\":" + version + ",\"items\":[" + items + "]}";
}

struct FakeFetcher final : DocFetcher
{
    int calls = 0;
    std::map<std::string, DocResult> results; // by url
    DocResult get(const std::string &url, std::size_t) override
    {
        ++calls;
        const auto it = results.find(url);
        return it == results.end() ? DocResult{} : it->second;
    }
    void serve(const std::string &url, int status, const std::string &body = "", const std::string &error = "")
    {
        DocResult r;
        r.status = status;
        r.body = body;
        r.error = error;
        results[url] = r;
    }
};

TEST(Editorial, NotesParseAndLookUpByAnyTitleId)
{
    EditorialData data;
    std::string why;
    ASSERT_TRUE(EditorialData::parse_notes(
        notes_doc("\"PPSA00001\":\"Great co-op.\",\"CUSA00002\":\"Needs the day-one patch.\""), &data, &why))
        << why;
    EXPECT_EQ(data.note_count(), 2u);
    EXPECT_EQ(data.note({"PPSA09999", "CUSA00002"}), "Needs the day-one patch.");
    EXPECT_EQ(data.note({"PPSA00001", "CUSA00002"}), "Great co-op."); // the first id with a note wins
    EXPECT_EQ(data.note({"PPSA09999"}), "");
    EXPECT_EQ(data.note({}), "");
}

TEST(Editorial, BadNotesAreSkippedNotFatal)
{
    const std::string long_text(161, 'x');
    EditorialData data;
    std::string why;
    ASSERT_TRUE(EditorialData::parse_notes(notes_doc("\"PPSA00001\":\"ok\","
                                                     "\"PPSA1\":\"short id\","
                                                     "\"ABCD00003\":\"wrong prefix\","
                                                     "\"PPSA00004\":\"\","
                                                     "\"PPSA00005\":\"" +
                                                     long_text +
                                                     "\","
                                                     "\"PPSA00006\":\"tab\\there\","
                                                     "\"PPSA00007\":42,"
                                                     "\"PPSA00008\":\"also ok\""),
                                           &data, &why))
        << why;
    EXPECT_EQ(data.note_count(), 3u); // the long one is kept: no length limit
    EXPECT_EQ(data.note({"PPSA00001"}), "ok");
    EXPECT_EQ(data.note({"PPSA00008"}), "also ok");
    std::string accents;
    for (int i = 0; i < 160; ++i)
        accents += "\xC3\xA9";
    ASSERT_TRUE(EditorialData::parse_notes(notes_doc("\"PPSA00001\":\"" + accents + "\""), &data, &why));
    EXPECT_EQ(data.note_count(), 1u);
    // No length limit: the page scales the type.
    ASSERT_TRUE(EditorialData::parse_notes(notes_doc("\"PPSA00001\":\"" + std::string(900, 'x') + "\""), &data, &why)) << why;
    EXPECT_EQ(data.note_count(), 1u);
}

TEST(Editorial, SchemaAndSizeRejects)
{
    EditorialData data;
    std::string why;
    EXPECT_FALSE(EditorialData::parse_notes("not json", &data, &why));
    EXPECT_FALSE(EditorialData::parse_notes("{\"notes\":{}}", &data, &why));
    EXPECT_FALSE(EditorialData::parse_notes(notes_doc("", "0"), &data, &why));
    EXPECT_FALSE(EditorialData::parse_notes("{\"schemaVersion\":1}", &data, &why));
    EXPECT_FALSE(EditorialData::parse_notes(notes_doc("", "2"), &data, &why));
    EXPECT_NE(why.find("newer Wings"), std::string::npos);
    EXPECT_FALSE(EditorialData::parse_curated(curated_doc("", "2"), &data, &why));
    EXPECT_NE(why.find("newer Wings"), std::string::npos);
    EXPECT_FALSE(EditorialData::parse_curated("{\"schemaVersion\":1,\"items\":5}", &data, &why));
    // Oversize files are refused whole.
    EXPECT_FALSE(EditorialData::parse_notes(notes_doc("\"PPSA00001\":\"" + std::string(kEditorialNotesMaxBytes, 'x') + "\""),
                                            &data, &why));
    EXPECT_NE(why.find("too large"), std::string::npos);
    EXPECT_FALSE(EditorialData::parse_curated(curated_doc(std::string(kEditorialCuratedMaxBytes, ' ')), &data, &why));
    // A failed parse leaves what was there.
    ASSERT_TRUE(EditorialData::parse_notes(notes_doc("\"PPSA00001\":\"keep\""), &data, &why));
    EXPECT_FALSE(EditorialData::parse_notes("nope", &data, &why));
    EXPECT_EQ(data.note({"PPSA00001"}), "keep");
}

TEST(Editorial, NoteCountIsCapped)
{
    std::string entries;
    for (int i = 0; i < 700; ++i)
    {
        char id[16];
        std::snprintf(id, sizeof id, "PPSA%05d", i);
        entries += std::string(entries.empty() ? "" : ",") + "\"" + id + "\":\"n\"";
    }
    EditorialData data;
    std::string why;
    ASSERT_TRUE(EditorialData::parse_notes(notes_doc(entries), &data, &why)) << why;
    EXPECT_EQ(data.note_count(), kEditorialMaxNotes);
}

TEST(Editorial, CuratedKeepsOrderDedupesAndCaps)
{
    EditorialData data;
    std::string why;
    ASSERT_TRUE(EditorialData::parse_curated(
        curated_doc("\"PPSA00003\",\"CUSA00001\",\"PPSA00003\",\"bogus\",7,\"PPSA00002\""), &data, &why))
        << why;
    const std::vector<std::string> want = {"PPSA00003", "CUSA00001", "PPSA00002"};
    EXPECT_EQ(data.curated_ids(), want);
    std::string items;
    for (int i = 0; i < 40; ++i)
    {
        char id[16];
        std::snprintf(id, sizeof id, "PPSA%05d", i);
        items += std::string(items.empty() ? "" : ",") + "\"" + id + "\"";
    }
    ASSERT_TRUE(EditorialData::parse_curated(curated_doc(items), &data, &why)) << why;
    ASSERT_EQ(data.curated_ids().size(), kEditorialMaxCurated);
    EXPECT_EQ(data.curated_ids().front(), "PPSA00000");
    EXPECT_EQ(data.curated_ids().back(), "PPSA00023");
    // Notes are untouched by a curated parse.
    ASSERT_TRUE(EditorialData::parse_notes(notes_doc("\"PPSA00001\":\"hi\""), &data, &why));
    EXPECT_EQ(data.curated_ids().size(), kEditorialMaxCurated);
}

TEST(Editorial, HubHonoursTheSwitches)
{
    auto data = std::make_shared<EditorialData>();
    std::string why;
    ASSERT_TRUE(EditorialData::parse_notes(notes_doc("\"PPSA00001\":\"hi\""), data.get(), &why));
    ASSERT_TRUE(EditorialData::parse_curated(curated_doc("\"PPSA00001\""), data.get(), &why));
    EditorialHub hub;
    hub.set_data(data);
    EXPECT_EQ(hub.note({"PPSA00001"}), ""); // both off until the prefs say otherwise
    EXPECT_TRUE(hub.curated_ids().empty());
    const std::uint64_t before = hub.serial();
    hub.apply_prefs(true, false);
    EXPECT_NE(hub.serial(), before);
    EXPECT_EQ(hub.note({"PPSA00001"}), "hi");
    EXPECT_TRUE(hub.curated_ids().empty());
    hub.apply_prefs(false, true);
    EXPECT_EQ(hub.note({"PPSA00001"}), "");
    EXPECT_EQ(hub.curated_ids().size(), 1u);
}

struct ServiceFixture
{
    std::string dir = tmp_dir();
    EditorialServiceConfig config;
    std::int64_t now = 1000000;
    FakeFetcher fetcher;
    ServiceFixture()
    {
        config.cache_paths = {dir + "/ed.v1"};
        config.app_tag = "build-1";
        config.now = [this] { return now; };
        fetcher.serve(kEditorialNotesUrl, 200, notes_doc("\"PPSA00001\":\"first\""));
        fetcher.serve(kEditorialCuratedUrl, 200, curated_doc("\"PPSA00002\",\"PPSA00001\""));
    }
    // One app start: apply, wait, return what the hub holds.
    std::shared_ptr<const EditorialData> start(bool notes = true, bool carousel = true)
    {
        EditorialHub hub;
        EditorialService service(hub, fetcher, config);
        service.apply(notes, carousel);
        service.wait_idle();
        last_status = hub.status();
        return hub.data();
    }
    EditorialStatus last_status;
};

TEST(Editorial, OffMeansNoFetch)
{
    ServiceFixture f;
    EXPECT_EQ(f.start(false, false), nullptr);
    EXPECT_EQ(f.fetcher.calls, 0);
}

TEST(Editorial, FetchesBothFilesOnceAndUsesTheFreshCopyNextTime)
{
    ServiceFixture f;
    auto data = f.start();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(f.fetcher.calls, 2);
    EXPECT_EQ(data->note({"PPSA00001"}), "first");
    EXPECT_EQ(data->curated_ids().size(), 2u);
    EXPECT_EQ(f.last_status.updated, f.now);
    EXPECT_FALSE(f.last_status.busy);
    f.now += 3600;
    data = f.start();
    EXPECT_EQ(f.fetcher.calls, 2); // fresh: no fetch
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->curated_ids().front(), "PPSA00002");
}

TEST(Editorial, StaleCopyOrAnotherBuildIsFetchedAgain)
{
    ServiceFixture f;
    f.start();
    f.now += 25 * 3600;
    f.fetcher.serve(kEditorialNotesUrl, 200, notes_doc("\"PPSA00001\":\"second\""));
    auto data = f.start();
    EXPECT_EQ(f.fetcher.calls, 4);
    EXPECT_EQ(data->note({"PPSA00001"}), "second");
    f.now += 60;
    f.config.app_tag = "build-2";
    f.start();
    EXPECT_EQ(f.fetcher.calls, 6);
}

TEST(Editorial, FailureKeepsTheSavedCopy)
{
    ServiceFixture f;
    f.start();
    f.now += 3 * 24 * 3600;
    f.fetcher.serve(kEditorialNotesUrl, 0, "", "connection refused");
    f.fetcher.serve(kEditorialCuratedUrl, 500);
    auto data = f.start();
    EXPECT_EQ(f.fetcher.calls, 4);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->note({"PPSA00001"}), "first");
    EXPECT_EQ(data->curated_ids().size(), 2u);
    EXPECT_NE(f.last_status.message.find("Could not refresh"), std::string::npos);
    // The failure did not make the old copy look new: the next start tries again.
    f.start();
    EXPECT_EQ(f.fetcher.calls, 6);
}

TEST(Editorial, AMissingFileMeansNoneAndTheOtherStillWorks)
{
    ServiceFixture f;
    f.fetcher.serve(kEditorialCuratedUrl, 404);
    auto data = f.start();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->note({"PPSA00001"}), "first");
    EXPECT_TRUE(data->curated_ids().empty());
    EXPECT_EQ(f.last_status.message, "Updated");
    // A bad body keeps the saved half and does not stop the other file.
    f.now += 2 * 24 * 3600;
    f.fetcher.serve(kEditorialNotesUrl, 200, "garbage");
    f.fetcher.serve(kEditorialCuratedUrl, 200, curated_doc("\"PPSA00009\""));
    data = f.start();
    EXPECT_EQ(data->note({"PPSA00001"}), "first");
    EXPECT_EQ(data->curated_ids().size(), 1u);
}

TEST(Editorial, DamagedCacheAndNoNetworkGiveNoDataAndNoCrash)
{
    ServiceFixture f;
    f.start();
    f.now += 3 * 24 * 3600;
    f.fetcher.results.clear();
    FILE *fp = std::fopen((f.dir + "/ed.v1").c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fputs("garbage", fp);
    std::fclose(fp);
    auto data = f.start();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->note_count(), 0u);
    EXPECT_TRUE(data->curated_ids().empty());
}

TEST(Editorial, PrefsRoundTripAndOldFilesKeepTheDefaults)
{
    WingsLocalPrefs prefs;
    EXPECT_TRUE(prefs.notes_enabled);
    EXPECT_TRUE(prefs.carousel_enabled);
    EXPECT_FALSE(prefs.folder_confirmed);
    prefs.notes_enabled = false;
    prefs.carousel_enabled = false;
    prefs.folder_confirmed = true;
    WingsLocalPrefs back;
    ASSERT_TRUE(decode_local_prefs(encode_local_prefs(prefs), &back));
    EXPECT_FALSE(back.notes_enabled);
    EXPECT_FALSE(back.carousel_enabled);
    EXPECT_TRUE(back.folder_confirmed);
    EXPECT_TRUE(back == prefs);
    WingsLocalPrefs other = prefs;
    other.folder_confirmed = false;
    EXPECT_FALSE(other == prefs);
    other = prefs;
    other.notes_enabled = true;
    EXPECT_FALSE(other == prefs);
    WingsLocalPrefs old;
    ASSERT_TRUE(decode_local_prefs("region=USA\n", &old));
    EXPECT_TRUE(old.notes_enabled);
    EXPECT_TRUE(old.carousel_enabled);
    EXPECT_FALSE(old.folder_confirmed);
}

} // namespace
} // namespace orbit
