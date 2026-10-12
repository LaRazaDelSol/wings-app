// Wings core tests - UI-side logic ported from the Pegasus web UI JS.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>

#include "test_harness.hpp"
#include "test_support.hpp"
#include "wings/logic.hpp"

using namespace wings;
using namespace wings::logic;

TEST(url_safeHttpUrl)
{
    CHECK_EQ_STR(safeHttpUrl("https://Example.com"), "https://example.com/");
    CHECK_EQ_STR(safeHttpUrl("  http://a.com:80/x  "), "http://a.com/x");
    CHECK_EQ_STR(safeHttpUrl("https://a.com:443"), "https://a.com/");
    CHECK_EQ_STR(safeHttpUrl("http://a.com:8080/x"), "http://a.com:8080/x");
    CHECK_EQ_STR(safeHttpUrl("https://x.com/a b?q=1 2#f g"), "https://x.com/a%20b?q=1%202#f%20g");
    CHECK_EQ_STR(safeHttpUrl("http://x.com/a/../b/./c"), "http://x.com/b/c");
    CHECK_EQ_STR(safeHttpUrl("http://x.com\\a\\b"), "http://x.com/a/b");
    CHECK_EQ_STR(safeHttpUrl("ftp://x.com/a"), "");
    CHECK_EQ_STR(safeHttpUrl("javascript:alert(1)"), "");
    CHECK_EQ_STR(safeHttpUrl("//x.com/a"), "");
    CHECK_EQ_STR(safeHttpUrl(""), "");
    CHECK_EQ_STR(safeHttpUrl("https://"), "");
    CHECK_EQ_STR(safeHttpUrl("https://[::1]:6970/x"), "https://[::1]:6970/x");
    CHECK_EQ_STR(safeHttpUrl("https://user:pw@h.com/x"), "https://user:pw@h.com/x");
    CHECK_EQ_STR(safeHttpUrl("https://h.com/a\tb"), "https://h.com/ab");
}

TEST(url_safeAbsoluteHttpUrl)
{
    CHECK_EQ_STR(safeAbsoluteHttpUrl("  https://a.com/  "), "https://a.com");
    CHECK_EQ_STR(safeAbsoluteHttpUrl("https://a.com/x/"), "https://a.com/x");
    CHECK_EQ_STR(safeAbsoluteHttpUrl("https://a.com/x?y=1"), "https://a.com/x?y=1");
    CHECK_EQ_STR(safeAbsoluteHttpUrl("nope"), "");
}

TEST(url_pkg_extension_rule)
{
    CHECK(urlHasPkgExtension("https://h.com/game.pkg"));
    CHECK(urlHasPkgExtension("https://h.com/GAME.PKG"));
    CHECK(urlHasPkgExtension("https://h.com/game.pkg?token=1"));
    CHECK(urlHasPkgExtension("https://h.com/game.pkg#frag"));
    CHECK(urlHasPkgExtension("http://h.com/a/b/c.d.pkg"));
    CHECK(!urlHasPkgExtension("https://h.com/game.pkg/"));
    CHECK(!urlHasPkgExtension("https://h.com/game.pkgx"));
    CHECK(!urlHasPkgExtension("https://h.com/x?file=a.pkg"));
    CHECK(!urlHasPkgExtension("https://h.com/game.exfat"));
    CHECK(!urlHasPkgExtension("ftp://h.com/a.pkg"));
    CHECK(!urlHasPkgExtension("a.pkg"));
    CHECK(!urlHasPkgExtension(""));
}

TEST(url_base64_decoding)
{
    // Standard, padded and unpadded, and url-safe forms all decode.
    CHECK_EQ_STR(downloadLinkUrl("aHR0cHM6Ly9leGFtcGxlLmNvbS9maWxlLnBrZw=="), "https://example.com/file.pkg");
    CHECK_EQ_STR(downloadLinkUrl("aHR0cHM6Ly9leGFtcGxlLmNvbS9maWxlLnBrZw"), "https://example.com/file.pkg");
    CHECK_EQ_STR(downloadLinkUrl("aHR0cDovL3guY28vPz8/Pj4+"), "http://x.co/?" "?" "?>>>");
    CHECK_EQ_STR(downloadLinkUrl("aHR0cDovL3guY28vPz8_Pj4-"), "http://x.co/?" "?" "?>>>");
    CHECK_EQ_STR(downloadLinkUrl("aHR0cHM6Ly9ha2lyYWJveC5jb20vQm5rbVd2OVlZR1IwL2ZpbGU="), "https://akirabox.com/BnkmWv9YYGR0/file");
    // Already-http(s) values are returned untouched (even weird ones).
    CHECK_EQ_STR(downloadLinkUrl("https://akirabox.com/x"), "https://akirabox.com/x");
    CHECK_EQ_STR(downloadLinkUrl("HTTP://UPPER.com/x"), "HTTP://UPPER.com/x");
    // Not base64 / not an http URL when decoded => unchanged.
    CHECK_EQ_STR(downloadLinkUrl("hello"), "hello");         // length % 4 == 1
    CHECK_EQ_STR(downloadLinkUrl("abcd"), "abcd");           // decodes to non-URL bytes
    CHECK_EQ_STR(downloadLinkUrl("a b="), "a b=");           // invalid character
    CHECK_EQ_STR(downloadLinkUrl("YQ==="), "YQ===");         // more than two '='
    CHECK_EQ_STR(downloadLinkUrl("ZnRwOi8veC5jb20v"), "ZnRwOi8veC5jb20v"); // decodes to ftp://
    CHECK_EQ_STR(downloadLinkUrl(""), "");
    CHECK_EQ_STR(hostFromDownloadUrl("aHR0cHM6Ly93d3cubWVkaWFmaXJlLmNvbS9maWxlL2FiYy94LnJhcg=="), "mediafire.com");
    CHECK_EQ_STR(hostFromDownloadUrl("not a url"), "");
}

TEST(url_hosts)
{
    CHECK_EQ_STR(hostFromUrl("https://WWW.Example.com/x"), "example.com");
    CHECK_EQ_STR(hostFromUrl("https://www2.example.com/x"), "www2.example.com");
    CHECK_EQ_STR(hostFromUrl("ftp://example.com"), "");
    CHECK(providerHostMatches("sub.buzzheavier.com", "buzzheavier.com"));
    CHECK(providerHostMatches("www.buzzheavier.com", "buzzheavier.com"));
    CHECK(providerHostMatches("buzzheavier.com", "www.buzzheavier.com"));
    CHECK(!providerHostMatches("evilbuzzheavier.com", "buzzheavier.com"));
    CHECK(!providerHostMatches("buzzheavier.com.evil.io", "buzzheavier.com"));
    CHECK(!providerHostMatches("", "buzzheavier.com"));
}

TEST(provider_classification)
{
    auto kind = [](const char *url) { return downloadSupportForUrl(url).kind; };
    // automated table (app.js:38-40)
    CHECK(kind("https://buzzheavier.com/abc") == SupportKind::automated);
    CHECK(kind("https://www.bzzhr.co/x") == SupportKind::automated);
    CHECK(kind("https://bzzhr.to/x") == SupportKind::automated);
    CHECK(kind("https://sub.buzzheavier.com/x") == SupportKind::automated);
    CHECK(kind("https://datanodes.to/abc") == SupportKind::automated);
    CHECK(kind("https://mediafire.com/file/x") == SupportKind::automated);
    CHECK(kind("https://www.mediafire.com/file/x") == SupportKind::automated);
    // mediafire: matchSubdomains:false => subdomains are NOT matched
    CHECK(kind("https://download.mediafire.com/x") == SupportKind::unknown);
    // direct-browser table (app.js:42-46)
    CHECK(kind("https://rootz.so/d/1") == SupportKind::directBrowser);
    CHECK(kind("https://www.rootz.so/d/1UZHP6") == SupportKind::directBrowser);
    CHECK(kind("https://filekeeper.net/x") == SupportKind::directBrowser);
    CHECK(kind("https://vikingfile.com/f/Ni0uI6Ukkq") == SupportKind::directBrowser);
    CHECK(kind("https://vik1ngfile.site/f/x") == SupportKind::directBrowser);
    // unknown
    CHECK(kind("https://akirabox.com/x/file") == SupportKind::unknown);
    CHECK(kind("https://evilrootz.so/x") == SupportKind::unknown);
    CHECK(kind("not a url") == SupportKind::unknown);
    CHECK(kind("") == SupportKind::unknown);
    // base64-wrapped link is classified by its decoded host
    CHECK(kind("aHR0cHM6Ly9idXp6aGVhdmllci5jb20veA==") == SupportKind::automated);

    // Field-level semantics (app.js:458-473)
    DownloadSupport a = downloadSupportForUrl("https://buzzheavier.com/x");
    CHECK(a.automated && !a.guarded && !a.blocked && !a.browserOnly && a.state == "supported" && a.label == "Supported");
    CHECK(a.hasProvider && a.provider.id == "buzzheavier" && a.provider.requiresBrowser);
    DownloadSupport m = downloadSupportForUrl("https://mediafire.com/x");
    CHECK(m.automated && !m.provider.requiresBrowser && !m.provider.matchSubdomains);
    DownloadSupport r = downloadSupportForUrl("https://rootz.so/d/1");
    CHECK(!r.automated && r.guarded && r.state == "supported");
    DownloadSupport f = downloadSupportForUrl("https://filekeeper.net/x");
    CHECK(!f.automated && !f.guarded);
    DownloadSupport v = downloadSupportForUrl("https://vikingfile.com/f/x");
    CHECK(!v.guarded);
    DownloadSupport u = downloadSupportForUrl("https://akirabox.com/x");
    CHECK(!u.automated && u.guarded && u.state == "manual" && u.label == "Unknown" && !u.hasProvider);
    CHECK(unsupportedProviders().empty());
    CHECK_EQ_INT(automatedProviders().size(), 3);
    CHECK_EQ_INT(directBrowserProviders().size(), 3);
}

namespace
{
Settings settings_with_debrid(bool rdReady, bool tbReady)
{
    Settings s;
    s.realDebrid.present = s.torBox.present = true;
    s.realDebrid.configured = rdReady;
    s.realDebrid.valid = rdReady;
    if (rdReady)
        s.realDebrid.domains = {"1fichier.com", "rootz.so"};
    s.torBox.configured = tbReady;
    s.torBox.valid = tbReady;
    if (tbReady)
        s.torBox.domains = {"1fichier.com", "filer.net"};
    return s;
}
} // namespace

TEST(debrid_matching)
{
    const Settings both = settings_with_debrid(true, true);
    auto svc = debridServicesForUrl("https://1fichier.com/?abc", both);
    CHECK_EQ_INT(svc.size(), 2);
    CHECK(svc.size() == 2 && svc[0] == DebridService::realDebrid && svc[1] == DebridService::torBox);
    svc = debridServicesForUrl("https://sub.1fichier.com/x", both);
    CHECK_EQ_INT(svc.size(), 2);
    svc = debridServicesForUrl("https://filer.net/x", both);
    CHECK(svc.size() == 1 && svc[0] == DebridService::torBox);
    svc = debridServicesForUrl("https://rootz.so/d/1", both);
    CHECK(svc.size() == 1 && svc[0] == DebridService::realDebrid);
    CHECK(debridServicesForUrl("https://not1fichier.com/x", both).empty());
    CHECK(debridServicesForUrl("not a url", both).empty());
    CHECK(realDebridSupportsUrl("https://1fichier.com/x", both));
    CHECK(!realDebridSupportsUrl("https://filer.net/x", both));

    // A service must be configured AND valid AND have domains (app.js:429).
    Settings partial = both;
    partial.realDebrid.valid = false;
    svc = debridServicesForUrl("https://1fichier.com/x", partial);
    CHECK(svc.size() == 1 && svc[0] == DebridService::torBox);
    partial = both;
    partial.torBox.domains.clear();
    CHECK(!debridReady(partial.torBox));
    CHECK(debridServicesForUrl("https://filer.net/x", partial).empty());
    CHECK(debridServicesForUrl("https://1fichier.com/x", settings_with_debrid(false, false)).empty());

    // base64 link
    CHECK_EQ_INT(debridServicesForUrl("aHR0cHM6Ly8xZmljaGllci5jb20veA==", both).size(), 2); // base64-wrapped link
    CHECK(debridServiceById("torbox").service == DebridService::torBox);
    CHECK(debridServiceById("bogus").service == DebridService::realDebrid); // app.js:418 fallback
    CHECK_EQ_STR(debridServiceInfo(DebridService::torBox).tokenSetting, "torBoxToken");
    CHECK_EQ_STR(debridServiceInfo(DebridService::realDebrid).tokenSetting, "realDebridToken");
}

TEST(link_action_decision)
{
    Package pkg;
    pkg.id = 42;
    auto add = [&](const char *name, const char *url, bool hasIndex = false, int64_t idx = 0) {
        DownloadLink l;
        l.name = name;
        l.url = url;
        l.hasIndex = hasIndex;
        l.index = idx;
        pkg.downloadLinks.push_back(l);
    };
    add("pkg", "https://h.com/game.pkg");              // 0
    add("", "");                                        // dropped (no url)
    add("bz", "https://buzzheavier.com/x");             // index 1 (position after filtering)
    add("rz", "https://rootz.so/d/1");                  // 2
    add("fi", "https://1fichier.com/x");                // 3
    add("ak", "https://akirabox.com/x");                // 4
    add("mf", "https://mediafire.com/f");               // 5
    add("vk", "https://vikingfile.com/f/x");            // 6
    const Settings none = settings_with_debrid(false, false);
    const Settings one = settings_with_debrid(false, true);
    const Settings two = settings_with_debrid(true, true);

    CHECK(decideLinkAction(pkg, 0, none).kind == LinkActionKind::nativePkg);
    LinkAction n = decideLinkAction(pkg, 0, none);
    CHECK_EQ_INT(n.packageId, 42);
    CHECK_EQ_INT(n.linkIndex, 0);
    CHECK(decideLinkAction(pkg, 1, none).kind == LinkActionKind::automatedCapture);
    CHECK(decideLinkAction(pkg, 2, none).kind == LinkActionKind::manualCapture);
    LinkAction rz = decideLinkAction(pkg, 2, none);
    CHECK(rz.guarded && rz.requiresBrowser);
    LinkAction vk = decideLinkAction(pkg, 6, none);
    CHECK(vk.kind == LinkActionKind::manualCapture && !vk.guarded);
    LinkAction mf = decideLinkAction(pkg, 5, none);
    CHECK(mf.kind == LinkActionKind::automatedCapture && !mf.requiresBrowser);
    CHECK(decideLinkAction(pkg, 4, none).kind == LinkActionKind::manualCapture);
    CHECK(decideLinkAction(pkg, 3, none).kind == LinkActionKind::manualCapture);
    CHECK(decideLinkAction(pkg, 3, one).kind == LinkActionKind::debrid);
    CHECK(decideLinkAction(pkg, 3, one).services.size() == 1);
    CHECK(decideLinkAction(pkg, 3, two).kind == LinkActionKind::chooseDebrid);
    // .pkg wins over debrid, as in app.js:4863 coming before :4881
    pkg.downloadLinks[0].url = "https://1fichier.com/game.pkg";
    CHECK(decideLinkAction(pkg, 0, two).kind == LinkActionKind::nativePkg);
    CHECK(decideLinkAction(pkg, 99, none).kind == LinkActionKind::linkNotFound);

    // packageLinks(): drops empty-url links, positional index after filtering, "Link N" names.
    auto links = packageLinks(pkg);
    CHECK_EQ_INT(links.size(), 7);
    CHECK_EQ_INT(links[1].index, 1);
    CHECK_EQ_STR(links[0].name, "pkg");
    Package p2;
    DownloadLink a;
    a.url = "https://a/x";
    DownloadLink b;
    b.url = "https://b/x";
    b.hasIndex = true;
    b.index = 9;
    p2.downloadLinks = {a, b};
    links = packageLinks(p2);
    CHECK_EQ_STR(links[0].name, "Link 1");
    CHECK_EQ_INT(links[0].index, 0);
    CHECK_EQ_INT(links[1].index, 9);
    CHECK(packageLinkByIndex(links, 9) != nullptr);
    CHECK(packageLinkByIndex(links, 1) == nullptr);
}

TEST(dispatch_results)
{
    QueueResponse r;
    auto add = [&](bool ok, DispatchKind k, bool accepted, const char *title = "", const char *msg = "", const char *cid = "") {
        DispatchResult d;
        d.ok = ok;
        d.kind = k;
        d.accepted = accepted;
        d.title = title;
        d.message = msg;
        d.contentId = cid;
        r.dispatchResults.push_back(d);
    };
    add(true, DispatchKind::nativePkg, true, "Game", "Started!", "UP0001-X");
    add(true, DispatchKind::nativePkg, false);           // not accepted
    add(false, DispatchKind::nativePkg, true);           // not ok
    add(true, DispatchKind::queued, false);
    add(true, DispatchKind::other, true);
    CHECK_EQ_INT(nativeDispatchResults(r).size(), 1);
    CHECK_EQ_INT(queuedDispatchResults(r).size(), 1);

    DispatchOutcome o = finishDispatchResponse(r, "Download queued.");
    CHECK(o.showNativeConfirmation);
    CHECK_EQ_STR(o.toast, "Package download started in PS5 Downloads.");
    CHECK_EQ_STR(o.confirmation.title, "Game");
    CHECK_EQ_STR(o.confirmation.message, "Started!");
    CHECK(o.confirmation.contentIds.size() == 1 && o.confirmation.contentIds[0] == "UP0001-X");
    CHECK(!o.queueIncluded && o.needsQueueReload && o.schedulePoll); // queued result, reply had no jobs[]

    r.hasQueue = true;
    o = finishDispatchResponse(r, "Download queued.");
    CHECK(o.queueIncluded && !o.needsQueueReload && o.schedulePoll);

    QueueResponse only_queued;
    DispatchResult q;
    q.ok = true;
    q.kind = DispatchKind::queued;
    only_queued.dispatchResults.push_back(q);
    o = finishDispatchResponse(only_queued, "Direct link queued.");
    CHECK(!o.showNativeConfirmation);
    CHECK_EQ_STR(o.toast, "Direct link queued.");

    // Several native results => aggregate texts (app.js:3486-3490, 3513-3515).
    QueueResponse many;
    for (int i = 0; i < 3; ++i)
    {
        DispatchResult d;
        d.ok = d.accepted = true;
        d.kind = DispatchKind::nativePkg;
        d.contentId = i == 1 ? "" : "C" + std::to_string(i);
        many.dispatchResults.push_back(d);
    }
    o = finishDispatchResponse(many, "");
    CHECK_EQ_STR(o.toast, "3 package downloads started in PS5 Downloads.");
    CHECK_EQ_STR(o.confirmation.title, "3 package downloads started");
    CHECK_EQ_INT(o.confirmation.contentIds.size(), 2);
    // Defaults when the single result has no title/message (app.js:3486-3488).
    QueueResponse bare;
    DispatchResult d;
    d.ok = d.accepted = true;
    d.kind = DispatchKind::nativePkg;
    bare.dispatchResults.push_back(d);
    o = finishDispatchResponse(bare, "");
    CHECK_EQ_STR(o.confirmation.title, "Package download started");
    CHECK(o.confirmation.message.find("PS5 Downloads") != std::string::npos);
    // Nothing at all.
    o = finishDispatchResponse(QueueResponse(), "");
    CHECK(o.toast.empty() && !o.showNativeConfirmation && !o.schedulePoll);
}

TEST(probe_rejection_and_capture_text)
{
    ResolveProbe p;
    p.status = 200;
    p.isHtml = true;
    CHECK_EQ_STR(probeRejection(p), "Link points to a web page, not a direct download.");
    p.isDownload = true;
    CHECK_EQ_STR(probeRejection(p), "");
    p.status = 404;
    CHECK_EQ_STR(probeRejection(p), "Link returned HTTP 404.");
    p.status = 0; // status 0 is not checked (app.js:3552 `status && ...`)
    p.isHtml = false;
    CHECK_EQ_STR(probeRejection(p), "");
    p.status = 299;
    CHECK_EQ_STR(probeRejection(p), "");
    p.status = 300;
    CHECK_EQ_STR(probeRejection(p), "Link returned HTTP 300.");

    CHECK_EQ_STR(captureStatusText(CaptureState::watching, true, true), "Browser opened. Waiting for Cloudflare verification...");
    CHECK_EQ_STR(captureStatusText(CaptureState::watching, true, false), "Resolving download link...");
    CHECK_EQ_STR(captureStatusText(CaptureState::watching, false, true), "Browser opened. Waiting for the download URL...");
    CHECK_EQ_STR(captureStatusText(CaptureState::queueing, false, true), "Download URL found. Adding it to the queue...");
    CHECK_EQ_STR(captureStatusText(CaptureState::completed, true, true), "Supported download queued.");
    CHECK_EQ_STR(captureStatusText(CaptureState::failed, false, true), "Browser capture failed.");
    CHECK_EQ_STR(captureStatusText(CaptureState::idle, true, true), "Starting supported download...");
    CHECK(captureIsTerminal(CaptureState::completed) && captureIsTerminal(CaptureState::failed) &&
          captureIsTerminal(CaptureState::cancelled) && !captureIsTerminal(CaptureState::watching));
}

TEST(queue_status_helpers)
{
    CHECK(jobStatusFromString("waiting_password") == JobStatus::waitingPassword);
    CHECK(jobStatusFromString("waiting_choice") == JobStatus::waitingChoice);
    CHECK(jobStatusFromString("nonsense") == JobStatus::unknown);
    CHECK_EQ_STR(toString(JobStatus::waitingChoice), "waiting_choice");
    CHECK(isActiveJobStatus(JobStatus::paused) && !isActiveJobStatus(JobStatus::completed));
    CHECK(isRunningJobStatus(JobStatus::pausing) && !isRunningJobStatus(JobStatus::queued));
    CHECK(isPausableJobStatus(JobStatus::queued) && !isPausableJobStatus(JobStatus::finalizing));
    CHECK(isTerminalJobStatus(JobStatus::failed));
    CHECK(jobPhaseFromString("") == JobPhase::none && jobPhaseFromString("merge") == JobPhase::merge &&
          jobPhaseFromString("weird") == JobPhase::unknown);
    std::vector<QueueJob> jobs(1);
    jobs[0].status = JobStatus::completed;
    CHECK_EQ_INT(queuePollDelayMs(jobs), 2500);
    jobs[0].status = JobStatus::queued;
    CHECK_EQ_INT(queuePollDelayMs(jobs), 1000);
    QueueJob j;
    j.archiveMode = "stream";
    CHECK(queueJobStreamMode(j));
    j.hasStreamMode = true;
    j.streamMode = false; // explicit false wins (app.js:1796)
    CHECK(!queueJobStreamMode(j));
}

TEST(apr_logic)
{
    AprEmu a;
    CHECK(aprEmuState(a) == AprState::missing);
    a.reason = "Unsupported title";
    CHECK(aprEmuState(a) == AprState::unsupported);
    a.present = true;
    a.updateAvailable = true;
    a.canUpdate = true;
    CHECK(aprEmuState(a) == AprState::update);
    a.canUpdate = false;
    CHECK(aprEmuState(a) == AprState::unsupported); // present, update available but can't: falls to reason
    a.updateAvailable = false;
    CHECK(aprEmuState(a) == AprState::current);
    a.remoteAvailable = false;
    CHECK(aprEmuState(a) == AprState::unavailable);
    CHECK_EQ_STR(aprEmuVersionText(a), "unknown");
    a.version = "1.4.2";
    a.build = "release";
    CHECK_EQ_STR(aprEmuVersionText(a), "v1.4.2 release");
    AprEmu none;
    CHECK_EQ_STR(aprEmuVersionText(none), "");

    std::vector<AprVersion> v = {{"a", "1.9.0", "release", 0},
                                 {"b", "1.10.0", "release", 0},
                                 {"c", "1.10.0", "debug", 0},
                                 {"d", "1.10.0-beta", "release", 0}};
    auto sorted = sortedAprVersions(v, "a");
    CHECK(sorted.size() == 4 && sorted[0].id == "a" && sorted[1].id == "b" && sorted[2].id == "c" && sorted[3].id == "d");
    sorted = sortedAprVersions(v, "");
    CHECK(sorted[0].id == "b" && sorted[1].id == "c" && sorted[2].id == "d" && sorted[3].id == "a");
    CHECK_EQ_STR(aprVersionLabel(v[2]), "v1.10.0 debug");
    CHECK_EQ_STR(aprVersionLabel(v[0]), "v1.9.0 release");
}

TEST(installed_game_helpers)
{
    CHECK_EQ_STR(psFirmwareVersion("0x0900000000000000"), "9.00");
    CHECK_EQ_STR(psFirmwareVersion("0x1400000000000000"), "14.00");
    CHECK_EQ_STR(psFirmwareVersion("0x0960000000000000"), "9.60");
    CHECK_EQ_STR(psFirmwareVersion("9.60"), "9.60");
    CHECK_EQ_STR(psFirmwareVersion("FW 05.50 beta"), "5.50");
    CHECK_EQ_STR(psFirmwareVersion("150000000"), "15.00"); // decimal-looking, falls back to the digits
    CHECK_EQ_STR(psFirmwareVersion("0x3000000000000000"), ""); // major 30 > 25
    CHECK_EQ_STR(psFirmwareVersion("0x0000000000000000"), ""); // major 0
    CHECK_EQ_STR(psFirmwareVersion("garbage"), "");
    CHECK_EQ_STR(psFirmwareVersion(""), "");
    CHECK_EQ_STR(installedFirmwareText("0x1400000000000000"), "FW 14.00");
    CHECK_EQ_STR(installedFirmwareText(""), "");

    InstalledGame g;
    g.titleId = "PPSA 1";
    CHECK_EQ_STR(installedArtUrl(g, "icon"), "/appdb/icon?id=PPSA%201");
    CHECK_EQ_STR(installedArtUrl(g, "pic0"), "/appdb/pic0?id=PPSA%201");
    g.iconUrl = "/appdb/icon?id=X";
    CHECK_EQ_STR(installedArtUrl(g, "icon"), "/appdb/icon?id=X");
    CHECK_EQ_STR(installedVersionText(g), "");
    g.masterVersion = "01.00";
    CHECK_EQ_STR(installedVersionText(g), "01.00");
    g.version = "01.02";
    CHECK_EQ_STR(installedVersionText(g), "01.02");
    g.contentVersion = "01.000.034";
    CHECK_EQ_STR(installedVersionText(g), "01.000.034");
    g.storageKind = "folder";
    CHECK_EQ_STR(installedFormatText(g), "folder");
    g.format = "exfat";
    CHECK_EQ_STR(installedFormatText(g), "exfat");
    g.installPath = "/user/app/X";
    CHECK_EQ_STR(installedLocationText(g), "/user/app/X");
    g.backingPath = "/mnt/ext0/x.exfat";
    CHECK_EQ_STR(installedLocationText(g), "/mnt/ext0/x.exfat");
}

TEST(file_manager_helpers)
{
    CHECK_EQ_STR(normalizeFilePath("//a//b/"), "/a/b");
    CHECK_EQ_STR(normalizeFilePath("/"), "/");
    CHECK_EQ_STR(normalizeFilePath("///"), "/");
    CHECK_EQ_STR(normalizeFilePath("a/b"), "");
    CHECK_EQ_STR(normalizeFilePath(""), "");
    CHECK_EQ_STR(normalizeFilePath("/data/x/"), "/data/x");
    auto u = uniquePaths({"/a", "/a/", "bad", "/b", "//b", "/c"});
    CHECK(u.size() == 3 && u[0] == "/a" && u[1] == "/b" && u[2] == "/c");

    FileEntry e;
    e.type = "file";
    e.name = "Game.PKG";
    CHECK(isPkgEntry(e));
    e.type = "dir";
    CHECK(!isPkgEntry(e));
    e.type = "file";
    e.name = "x.rar";
    CHECK(isExtractableRarEntry(e));
    e.name = "x.part1.rar";
    CHECK(!isExtractableRarEntry(e));
    e.name = "X.PART12.RAR";
    CHECK(!isExtractableRarEntry(e));
    e.name = "part1.rar";
    CHECK(isExtractableRarEntry(e));
    e.name = "x.zip";
    CHECK(!isExtractableRarEntry(e));

    CHECK_EQ_STR(validateName("  ok "), "ok");
    CHECK_EQ_STR(validateName("a/b"), "");
    CHECK_EQ_STR(validateName(".."), "");
    CHECK_EQ_STR(validateName("."), "");
    CHECK_EQ_STR(validateName("   "), "");
    CHECK_EQ_STR(pathParent("/a/b"), "/a");
    CHECK_EQ_STR(pathParent("/a"), "/");
    CHECK_EQ_STR(pathParent("/"), "/");
    CHECK_EQ_STR(pathParent("/a/"), "/a/");
    CHECK_EQ_STR(pathJoin("/", "x"), "/x");
    CHECK_EQ_STR(pathJoin("/data", "x"), "/data/x");

    CHECK(fileOpStatusFromString("waiting_password") == FileOpStatus::waitingPassword);
    CHECK(isActiveFileOp(FileOpStatus::copying) && !isActiveFileOp(FileOpStatus::completed));
    CHECK(isTerminalFileOp(FileOpStatus::accepted) && isTerminalFileOp(FileOpStatus::failed) && !isTerminalFileOp(FileOpStatus::running));
    FileOperation op;
    op.status = FileOpStatus::running;
    CHECK(canCancelFileOp(op));
    op.cancelable = false;
    CHECK(!canCancelFileOp(op));
    op.cancelable = true;
    op.status = FileOpStatus::queued;
    CHECK(!canCancelFileOp(op));
    CHECK_EQ_STR(toString(Collision::keepBoth), "keep-both");
    CHECK_EQ_STR(toString(Collision::replace), "replace");
    CHECK_EQ_STR(toString(Collision::skip), "skip");
    CHECK(collisionFromString("skip") == Collision::skip && collisionFromString("x") == Collision::keepBoth);

    std::vector<Mount> mounts(3);
    mounts[0].path = "/data";
    mounts[1].path = "/mnt/ext0";
    mounts[2].path = "/data/sub";
    mounts[2].writable = false;
    CHECK(mountForPath(mounts, "/data/x") == &mounts[0]);
    CHECK(mountForPath(mounts, "/data/sub/x") == &mounts[2]);
    CHECK(mountForPath(mounts, "/data") == &mounts[0]);
    CHECK(mountForPath(mounts, "/database") == nullptr);
    CHECK(canSelectBrowsePath(mounts, "/mnt/ext0/games"));
    CHECK(!canSelectBrowsePath(mounts, "/data/sub"));
    CHECK(!canSelectBrowsePath(mounts, "/nowhere"));
    CHECK(validDownloadConnections(1) && validDownloadConnections(64) && !validDownloadConnections(0) && !validDownloadConnections(65));
}

TEST(url_encoding)
{
    CHECK_EQ_STR(encodeURIComponent("/data/a b+c"), "%2Fdata%2Fa%20b%2Bc");
    CHECK_EQ_STR(encodeURIComponent("a-b_c.d!e~f*g'h(i)"), "a-b_c.d!e~f*g'h(i)");
    CHECK_EQ_STR(encodeURIComponent("\xC3\xA9"), "%C3%A9");
    CHECK_EQ_STR(formUrlEncode("a b*c~"), "a+b*c%7E");
    CHECK_EQ_STR(formUrlEncode("/data/x y"), "%2Fdata%2Fx+y");
}

// Every real link in the 2962-package sample classifies without surprises.
TEST(provider_classification_over_real_library)
{
    const std::string path = sample("library.json");
    auto t = std::make_shared<http::ReplayTransport>();
    CHECK(t->add_file("GET", "/api/library", path));
    PegasusV1 api(t);
    auto lib = api.getLibrary();
    CHECK(lib.ok());
    std::map<int, int> counts;
    int pkgs = 0, undecodable = 0;
    for (const Package &p : lib.value)
    {
        for (const DownloadLink &l : packageLinks(p))
        {
            const std::string url = downloadLinkUrl(l.url);
            if (url.rfind("http", 0) != 0)
                ++undecodable;
            if (urlHasPkgExtension(url))
                ++pkgs;
            ++counts[static_cast<int>(downloadSupportForUrl(url).kind)];
        }
    }
    std::printf("    links: automated=%d direct=%d unsupported=%d unknown=%d, .pkg=%d, non-http=%d\n",
                counts[0], counts[1], counts[2], counts[3], pkgs, undecodable);
    CHECK_EQ_INT(undecodable, 0);
    CHECK(counts[static_cast<int>(SupportKind::automated)] > 0);
    CHECK(counts[static_cast<int>(SupportKind::directBrowser)] > 0);
    CHECK_EQ_INT(counts[static_cast<int>(SupportKind::unsupported)], 0);
}

// Hostile / garbage input must never throw or crash.
TEST(robustness_on_garbage_input)
{
    const std::string huge(500, '9');
    std::vector<AprVersion> v = {{"a", huge + ".1", "release", 0}, {"b", "1." + huge, "debug", 0}, {"c", "", "", 0}};
    CHECK_EQ_INT(sortedAprVersions(v, "").size(), 3);
    const char *junk[] = {"", " ", "http://", "https://[", "https://[::1", "http://a:99999999999/", "http://a:x/", "://", "http:",
                          "https://a b/", "\xFF\xFE", "%%%%", "====", "YQ==YQ==", "http://\x01\x02/"};
    for (const char *j : junk)
    {
        (void)safeHttpUrl(j);
        (void)downloadLinkUrl(j);
        (void)downloadSupportForUrl(j);
        (void)urlHasPkgExtension(j);
        (void)hostFromDownloadUrl(j);
        (void)psFirmwareVersion(j);
        (void)normalizeFilePath(j);
        (void)pathParent(j);
    }
    CHECK(psFirmwareVersion(std::string(300, '9')).empty() || true);
    CHECK_EQ_STR(psFirmwareVersion("99999999999999999999999"), "");
    Rig r;
    // Deeply nested / truncated / wrong-typed documents parse to errors or defaults, never crash.
    r.t->add("GET", "/api/queue", 200, std::string(200, '[') + std::string(200, ']'));
    CHECK(!r.api.getQueue().ok());
    r.t->add("GET", "/api/mounts", 200, "[1,2,3]");
    CHECK(r.api.getMounts().ok());
    r.t->add("GET", "/api/sources", 200, R"({"sources":"nope"})");
    CHECK(r.api.listSources().ok());
    r.t->add("GET", "/api/health", 200, "null");
    CHECK(r.api.health().ok());
}