import datetime
import json
import os
import sys
import tempfile
import types
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from popularity import blend, build, matcher, report  # noqa: E402
from popularity.matcher import Candidate  # noqa: E402
from popularity.models import Entry, ProviderError, Signal  # noqa: E402
from popularity.net import CachedHttp, HttpError, RateLimiter  # noqa: E402
from popularity.normalize import clean_title, normalize  # noqa: E402
from popularity.providers import pic0  # noqa: E402
from popularity.providers.igdb import Igdb  # noqa: E402
from popularity.providers.steam import Steam  # noqa: E402
from popularity.providers.wikipedia import Wikipedia  # noqa: E402

FIX = os.path.join(ROOT, "tests", "fixtures")


def fx(name):
    with open(os.path.join(FIX, name), encoding="utf-8") as f:
        return json.load(f)


class FakeHttp:
    """Recorded-fixture HTTP: routes by URL substring. Records calls; never touches the network."""

    def __init__(self, routes):
        self.routes, self.calls, self.headers = routes, [], None

    def _route(self, url, params, body):
        self.calls.append((url, params, body))
        for key, val in self.routes.items():
            if key in url:
                if isinstance(val, Exception):
                    raise val
                return val
        raise AssertionError("unexpected URL " + url)

    def get_json(self, url, params=None, headers=None, cache=True):
        return self._route(url, params, None)

    def post_json(self, url, body, headers=None, params=None, cache=True):
        self.headers = headers
        return self._route(url, params, body)


class NormalizeTests(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(clean_title("Hades II [exFAT] v01.005 @someone"), "Hades II")

    def test_key(self):
        self.assertEqual(normalize("Marvel's Spider-Man 2™").key, normalize("Marvels Spider Man 2").key)
        self.assertEqual(normalize("The Witcher III").key, "witcher 3")
        self.assertEqual(normalize("Tom & Jerry").key, "tom and jerry")

    def test_editions_separate(self):
        n = normalize("Control Ultimate Edition")
        self.assertEqual(n.key, "control")
        self.assertIn("ultimate", n.editions)


class MatcherTests(unittest.TestCase):
    def test_exact(self):
        m = matcher.rank("Hades II", [Candidate(1, "Hades II"), Candidate(2, "Hades")])
        self.assertEqual(m.candidate.id, 1)
        self.assertGreaterEqual(m.confidence, matcher.ACCEPT)
        self.assertFalse(m.ambiguous)

    def test_ambiguity_is_low_confidence(self):
        m = matcher.rank("Madden NFL 25", [Candidate("a", "Madden NFL 25", 2013), Candidate("b", "Madden NFL 25", 2024)])
        self.assertTrue(m.ambiguous)
        self.assertLessEqual(m.confidence, matcher.AMBIGUOUS_CAP)

    def test_year_disambiguates(self):
        c = [Candidate("a", "Madden NFL 25", 2013), Candidate("b", "Madden NFL 25", 2024)]
        m = matcher.rank("Madden NFL 25", c, year=2024)
        self.assertEqual(m.candidate.id, "b")
        self.assertFalse(m.ambiguous)

    def test_platform_filter(self):
        m = matcher.rank("Hades II", [Candidate(1, "Hades II", platform_ok=False)])
        self.assertIsNone(m)

    def test_dlc_ignored_unless_asked(self):
        c = [Candidate(1, "Foo Bundle"), Candidate(2, "Foo Soundtrack")]
        self.assertIsNone(matcher.rank("Foo", c))
        self.assertEqual(matcher.rank("Foo Bundle", c).candidate.id, 1)

    def test_no_match(self):
        self.assertIsNone(matcher.rank("Hades II", [Candidate(1, "Completely Different")]))

    def test_jaro_winkler_bounds(self):
        self.assertEqual(matcher.jaro_winkler("abc", "abc"), 1.0)
        self.assertEqual(matcher.jaro_winkler("abc", "xyz"), 0.0)


class ProviderTests(unittest.TestCase):
    def test_steam(self):
        http = FakeHttp({"storesearch": fx("steam_search_hades.json"), "GetAppReviews": fx("steam_reviews_hades.json")})
        s = Steam(http).lookup(Entry("PPSA1", "Hades II"))
        self.assertEqual(s.external_id, 1145350)
        self.assertEqual(s.rating_count, 65870)
        self.assertAlmostEqual(s.rating, 96.7, places=1)
        self.assertGreaterEqual(s.confidence, 0.85)
        self.assertTrue(s.image_url.endswith("/apps/1145350/library_hero.jpg"))

    def test_steam_override_and_error(self):
        http = FakeHttp({"GetAppReviews": fx("steam_reviews_hades.json")})
        self.assertEqual(Steam(http).lookup(Entry("PPSA1", "x"), 1145350).confidence, 1.0)
        bad = FakeHttp({"storesearch": HttpError(429, "x")})
        with self.assertRaises(ProviderError):
            Steam(bad).lookup(Entry("PPSA1", "Hades II"))

    def test_wikipedia(self):
        http = FakeHttp({"api.php": fx("wiki_search_hades.json"), "pageviews": fx("wiki_views_hades.json")})
        w = Wikipedia(http, today=datetime.date(2026, 10, 11)).lookup(Entry("PPSA1", "Hades II"))
        self.assertEqual(w.external_id, "Hades II")
        self.assertAlmostEqual(w.attention, 200.0)  # 6000 views / 30 days
        url = [c[0] for c in http.calls if "pageviews" in c[0]][0]
        self.assertTrue(url.endswith("/20260911/20261010"))

    def test_wikipedia_ambiguous_and_404(self):
        http = FakeHttp({"api.php": fx("wiki_search_madden.json"), "pageviews": HttpError(404, "x")})
        self.assertIsNone(Wikipedia(http).lookup(Entry("PPSA1", "Madden NFL 25")))
        http = FakeHttp({"api.php": fx("wiki_search_madden.json"), "pageviews": fx("wiki_views_hades.json")})
        w = Wikipedia(http).lookup(Entry("PPSA1", "Madden NFL 25"))
        self.assertTrue(w.ambiguous)
        self.assertLessEqual(w.confidence, 0.5)

    def test_igdb_skipped_without_env(self):
        ok, why = Igdb(FakeHttp({}), environ={}).available()
        self.assertFalse(ok)
        self.assertIn("TWITCH_CLIENT_ID", why)

    def test_igdb_lookup_and_secret_hygiene(self):
        env = {"TWITCH_CLIENT_ID": "cid", "TWITCH_CLIENT_SECRET": "s3cr3t-value"}
        http = FakeHttp({"oauth2/token": {"access_token": "tok", "expires_in": 100},
                         "v4/games": fx("igdb_games.json")})
        p = Igdb(http, environ=env)
        self.assertTrue(p.available()[0])
        s = p.lookup(Entry("PPSA1", "Elden Ring"))
        self.assertEqual(s.external_id, 119133)
        self.assertEqual(s.rating, 95.4)
        self.assertEqual(s.rating_count, 3000)
        self.assertEqual(s.image_url, "https://images.igdb.com/igdb/image/upload/t_1080p/sc2def.jpg")  # not the banner
        self.assertEqual(s.image_kind, "screenshot")
        self.assertEqual(s.low_url, "https://images.igdb.com/igdb/image/upload/t_screenshot_big/sc2def.jpg")
        self.assertIn("artworks.width", http.calls[-1][2])
        self.assertIn("screenshots.height", http.calls[-1][2])
        self.assertEqual(http.headers["Client-ID"], "cid")
        self.assertIn("platforms = (167,48)", http.calls[-1][2])
        p.lookup(Entry("PPSA2", "Elden Ring"))
        self.assertEqual(sum(1 for c in http.calls if "oauth2" in c[0]), 1)  # one token per run
        self.assertTrue(all("s3cr3t" not in c[0] for c in http.calls))      # never in a URL

    def test_igdb_error_has_no_secret(self):
        env = {"TWITCH_CLIENT_ID": "cid", "TWITCH_CLIENT_SECRET": "s3cr3t-value"}
        p = Igdb(FakeHttp({"oauth2/token": HttpError(403, "id.twitch.tv/oauth2/token")}), environ=env)
        with self.assertRaises(ProviderError) as cm:
            p.lookup(Entry("PPSA1", "Elden Ring"))
        self.assertNotIn("s3cr3t", str(cm.exception))


def sig(source, conf=1.0, rating=None, count=None, attention=None):
    return Signal(source, 1, "x", conf, rating=rating, rating_count=count, attention=attention)


class BlendTests(unittest.TestCase):
    def test_weights_documented_sum(self):
        self.assertAlmostEqual(blend.W_POPULARITY + blend.W_QUALITY, 1.0)

    def test_missing_sources_renormalise(self):
        data = {
            "A": [sig("wikipedia", attention=10)],
            "B": [sig("wikipedia", attention=100), sig("steam", rating=90, count=1000, attention=1000)],
            "C": [sig("steam", rating=50, count=1000, attention=10)],
            "D": [],
        }
        out = blend.blend(data, "2026-10-11")
        self.assertNotIn("D", out)                # no signal -> no record
        self.assertNotIn("rating", out["A"])      # wikipedia gives no rating
        self.assertEqual(out["B"]["rating"], 90)
        self.assertEqual(out["B"]["ratingCount"], 1000)
        self.assertGreater(out["B"]["score"], out["C"]["score"])
        self.assertTrue(all(0 <= r["score"] <= 100 for r in out.values()))
        self.assertEqual(out["A"]["score"], 25)   # wikipedia only: its percentile alone (lowest of 2)

    def test_shrinkage(self):
        data = {"few": [sig("steam", rating=100, count=3, attention=3)],
                "many": [sig("steam", rating=95, count=40000, attention=40000)],
                "mid": [sig("steam", rating=60, count=500, attention=500)]}
        out = blend.blend(data, "d")
        self.assertGreater(out["many"]["score"], out["few"]["score"])

    def test_confidence_is_min(self):
        out = blend.blend({"A": [sig("wikipedia", 0.9, attention=1), sig("steam", 0.5, 80, 100, 100)]}, "d")
        self.assertEqual(out["A"]["confidence"], 0.5)
        self.assertEqual(out["A"]["sources"], ["steam", "wikipedia"])


class NetTests(unittest.TestCase):
    def test_rate_limiter_spaces_requests(self):
        t = [0.0]
        slept = []

        def sleep(d):
            slept.append(d)
            t[0] += d
        rl = RateLimiter({"h": 0.25}, clock=lambda: t[0], sleep=sleep)
        for _ in range(4):
            rl.wait("h")
        self.assertAlmostEqual(sum(slept), 0.75)  # 4 req/s, under the 5 req/s policy

    def test_cache_hits_and_skips_secret_calls(self):
        class Inner:
            n = 0

            def request(self, method, url, params, headers, body):
                Inner.n += 1
                return {"n": Inner.n}
        with tempfile.TemporaryDirectory() as d:
            c = CachedHttp(Inner(), d)
            self.assertEqual(c.get_json("http://x/a", {"q": 1}), c.get_json("http://x/a", {"q": 1}))
            self.assertEqual(Inner.n, 1)
            c.post_json("http://x/token", "secret=1", cache=False)
            c.post_json("http://x/token", "secret=1", cache=False)
            self.assertEqual(Inner.n, 3)
            for root, _, files in os.walk(d):
                for f in files:
                    with open(os.path.join(root, f)) as fh:
                        self.assertNotIn("secret=1", fh.read())


class BuildTests(unittest.TestCase):
    def args(self, tmp, **kw):
        a = types.SimpleNamespace(catalogue=os.path.join(FIX, "library_mini.json"),
                                  out=os.path.join(tmp, "o", "p.json"), report=os.path.join(tmp, "o", "report.md"),
                                  overrides=None, providers="wikipedia,steam", cache_dir=None, cache_ttl_days=6,
                                  limit=None, max_bytes=400000, now="2026-10-11")
        a.__dict__.update(kw)
        return a

    def http(self):
        return FakeHttp({"storesearch": fx("steam_search_hades.json"), "GetAppReviews": fx("steam_reviews_hades.json"),
                         "api.php": fx("wiki_search_hades.json"), "pageviews": fx("wiki_views_hades.json")})

    def test_catalogue_dedupe_and_filter(self):
        es = build.load_catalogue(os.path.join(FIX, "library_mini.json"))
        self.assertEqual([e.title_id for e in es], ["CUSA00002", "PPSA00001"])
        self.assertEqual(es[1].platform, "PS5")

    def test_end_to_end_deterministic_and_schema(self):
        with tempfile.TemporaryDirectory() as tmp:
            outs = []
            for _ in range(2):
                build.run(self.args(tmp), environ={}, http=self.http(), log=lambda m: None)
                with open(os.path.join(tmp, "o", "p.json"), "rb") as f:
                    outs.append(f.read())
            self.assertEqual(outs[0], outs[1])
            doc = json.loads(outs[0])
            self.assertEqual(doc["schemaVersion"], 1)
            self.assertEqual(doc["sources"], ["steam", "wikipedia"])
            self.assertIn("steam", doc["attribution"])
            self.assertEqual(list(doc["titles"]), sorted(doc["titles"]))
            rec = doc["titles"]["PPSA00001"]
            self.assertEqual(set(rec), {"score", "rating", "ratingCount", "sources", "confidence", "hero"})
            self.assertEqual(rec["hero"], "https://cdn.cloudflare.steamstatic.com/steam/apps/1145350/library_hero.jpg")
            self.assertTrue(os.path.exists(os.path.join(tmp, "o", "report.md")))
            self.assertTrue(os.path.exists(os.path.join(tmp, "o", "p.json.sha256")))

    def test_report_in_a_missing_separate_folder(self):
        # CI writes site/popularity.v1.json and out/report.md: two different, not yet existing folders.
        with tempfile.TemporaryDirectory() as tmp:
            a = self.args(tmp, out=os.path.join(tmp, "site", "p.json"), report=os.path.join(tmp, "out", "report.md"))
            build.run(a, environ={}, http=self.http(), log=lambda m: None)
            self.assertTrue(os.path.exists(os.path.join(tmp, "out", "report.md")))

    def test_overrides_ignore_and_force(self):
        entries = [Entry("PPSA00001", "Hades II"), Entry("PPSA00002", "Whatever")]
        provs, _ = build.make_providers(["steam"], self.http(), datetime.date(2026, 10, 11), {})
        ov = {"PPSA00001": "ignore", "PPSA00002": {"steam": 1145350}}
        res = build.collect(entries, provs, ov)
        self.assertEqual(res["PPSA00001"]["steam"], "ignored")
        self.assertEqual(res["PPSA00002"]["steam"].confidence, 1.0)

    def test_igdb_skipped_when_secrets_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc, _ = build.run(self.args(tmp, providers="wikipedia,igdb"), environ={}, http=self.http(),
                               log=lambda m: None)
            self.assertEqual(doc["sources"], ["wikipedia"])
            with open(os.path.join(tmp, "o", "report.md"), encoding="utf-8") as f:
                self.assertIn("`igdb` skipped", f.read())

    def test_size_budget(self):
        entries = [Entry("PPSA%05d" % i, "T%d" % i) for i in range(1318)]
        results = {e.title_id: {"steam": Signal("steam", 1, "x", 1.0, rating=87.3, rating_count=123456,
                                                attention=123456 + i),
                                "wikipedia": Signal("wikipedia", "p", "p", 1.0, attention=i + 0.5)}
                   for i, e in enumerate(entries)}
        doc, _ = build.build_document(entries, results, ["steam", "wikipedia"],
                                      datetime.datetime(2026, 10, 11, tzinfo=datetime.timezone.utc))
        self.assertLess(len(build.dump(doc).encode()), 200 * 1024)

    def test_report_sections(self):
        entries = [Entry("PPSA1", "A"), Entry("PPSA2", "B"), Entry("PPSA3", "C")]
        results = {"PPSA1": {"steam": Signal("steam", 1, "A", 1.0, rating=1, attention=1)},
                   "PPSA2": {"steam": Signal("steam", 2, "Bee", 0.5, ambiguous=True, rating=1, attention=1)},
                   "PPSA3": {"steam": None}}
        md = report.render(entries, results, {}, ["steam"], "now")
        self.assertIn("| steam | 2 | 1 | 1 | 0 |", md)
        self.assertIn("Low confidence (1)", md)
        self.assertIn("PPSA3  C", md)
        # A public CI report carries IDs only: no game title and no matched name.
        hidden = report.render(entries, results, {}, ["steam"], "now", ids_only=True)
        self.assertIn("PPSA3", hidden)
        self.assertIn("PPSA2", hidden)
        for text in ("Bee", "  C", "| B |"):
            self.assertNotIn(text, hidden)


STEAM = "https://cdn.cloudflare.steamstatic.com/steam/apps/1/library_hero.jpg"
IGDB = "https://images.igdb.com/igdb/image/upload/t_1080p/a.jpg"
IGDB_LOW = "https://images.igdb.com/igdb/image/upload/t_screenshot_big/a.jpg"
PIC0 = "https://cdn.prosperopatches.com/titles/PPSA01234_" + "a1" * 32 + "/pic0.webp"
ICON = "https://cdn.prosperopatches.com/titles/PPSA01234_" + "a1" * 32 + "/icon0.webp"


class HeroTests(unittest.TestCase):
    def s(self, src, conf=1.0, amb=False, url=None, ratio=16 / 9, low=None):
        return Signal(src, 1, "x", conf, ambiguous=amb, rating=90, rating_count=100, attention=5, image_url=url,
                      image_ratio=ratio if url else None, low_url=low)

    def rec(self, *sigs):
        return blend.blend({"PPSA1": list(sigs)}, "2026-10-11")["PPSA1"]

    def test_priority_pic0_then_igdb_then_steam(self):
        p = Signal("pic0", "PPSA1", "", 1.0, image_url=PIC0, image_ratio=16 / 9)
        st, ig = self.s("steam", url=STEAM, ratio=1920 / 620), self.s("igdb", url=IGDB, low=IGDB_LOW)
        r = self.rec(st, ig, p)
        self.assertEqual((r["hero"], r["heroLow"]), (PIC0, IGDB_LOW))
        self.assertEqual(self.rec(st, ig)["hero"], IGDB)
        self.assertEqual(self.rec(st, self.s("igdb"))["hero"], STEAM)

    def test_pic0_is_art_only(self):
        p = Signal("pic0", "PPSA1", "", 1.0, image_url=PIC0, image_ratio=16 / 9)
        r = self.rec(self.s("steam"), p)
        self.assertEqual(r["sources"], ["steam"])
        self.assertEqual(r["confidence"], 1.0)
        self.assertEqual(blend.blend({"PPSA1": [p]}, None), {})  # art alone never creates a record

    def test_low_confidence_or_ambiguous_or_http_gets_none(self):
        self.assertNotIn("hero", self.rec(self.s("steam", conf=0.7, url="https://x/y.jpg")))
        self.assertNotIn("hero", self.rec(self.s("steam", conf=0.5, amb=True, url="https://x/y.jpg")))
        self.assertNotIn("hero", self.rec(self.s("steam", url="http://x/y.jpg")))
        self.assertNotIn("hero", self.rec(self.s("steam", url="https://u@x/y.jpg")))
        self.assertNotIn("hero", self.rec(self.s("steam", url="https://x/y.jpg?" + "a" * 300)))

    def test_ratio_bounds(self):
        for ratio, ok in ((1.4, False), (1.5, True), (3.2, True), (3.3, False), (1920 / 399, False)):
            r = self.rec(self.s("steam", url=STEAM, ratio=ratio))
            self.assertEqual("hero" in r, ok, ratio)
        self.assertNotIn("hero", self.rec(self.s("steam", url=STEAM, ratio=None)))

    def test_heroLow_omitted_when_missing_same_or_no_hero(self):
        self.assertNotIn("heroLow", self.rec(self.s("igdb", url=IGDB)))
        self.assertNotIn("heroLow", self.rec(self.s("igdb", url=IGDB, low=IGDB)))
        r = self.rec(self.s("igdb", url=None, low=IGDB_LOW))
        self.assertNotIn("hero", r)
        self.assertNotIn("heroLow", r)
        self.assertNotIn("heroLow", self.rec(self.s("igdb", conf=0.7, url=IGDB, low=IGDB_LOW)))

    def test_no_fetchedAt(self):
        self.assertNotIn("fetchedAt", self.rec(self.s("steam")))

    def test_igdb_selection(self):
        def sc(i, w, h):
            return {"image_id": i, "width": w, "height": h}
        g = {"artworks": [sc("ban", 1920, 399), sc("art", 1920, 1080)],
             "screenshots": [sc("shot43", 1600, 1200), sc("shot169", 1920, 1080), sc("small", 960, 540)]}
        url, kind, ratio, low = Igdb._image_url(g)
        self.assertTrue(url.endswith("/t_1080p/shot169.jpg"))
        self.assertEqual(kind, "screenshot")
        self.assertTrue(low.endswith("/t_screenshot_big/shot169.jpg"))
        # banner only -> nothing; artwork 16:9 used when no screenshot qualifies
        self.assertEqual(Igdb._image_url({"artworks": [sc("ban", 1920, 399)]}), (None, None, None, None))
        url, kind, _, low = Igdb._image_url({"artworks": [sc("art", 2560, 1440)], "screenshots": [sc("tiny", 800, 450)]})
        self.assertEqual((kind, url.split("/")[-1]), ("artwork", "art.jpg"))
        self.assertTrue(low.endswith("/tiny.jpg"))
        # too narrow for the hero, missing dims, odd ids are skipped
        self.assertIsNone(Igdb._best([sc("narrow", 1000, 562)], 1280))
        self.assertIsNone(Igdb._best([{"image_id": "nodims"}, sc("../x", 1920, 1080), sc("a b", 1920, 1080)], 1280))
        self.assertEqual(Igdb._best([sc("w", 1920, 1080), sc("v", 1280, 720)], 1280)[0], "w")  # tie -> wider

    def test_verify_hero_falls_back_to_header(self):
        import verify_hero
        base = "https://cdn.cloudflare.steamstatic.com/steam/apps/%d/"
        doc = {"titles": {"PPSA1": {"hero": base % 1 + "library_hero.jpg"},
                          "PPSA2": {"hero": base % 2 + "library_hero.jpg"},
                          "PPSA3": {"hero": base % 3 + "library_hero.jpg"}, "PPSA4": {}}}
        ok = {base % 1 + "library_hero.jpg", base % 2 + "header.jpg"}
        n, bad = verify_hero.verify(doc, lambda u: u in ok, fix=True)
        self.assertEqual((n, bad), (3, ["PPSA2", "PPSA3"]))
        self.assertTrue(doc["titles"]["PPSA2"]["hero"].endswith("header.jpg"))
        self.assertNotIn("hero", doc["titles"]["PPSA3"])

    def test_verify_hero_handles_heroLow(self):
        import verify_hero
        doc = {"titles": {"A": {"hero": "https://h/a", "heroLow": "https://l/a"},
                          "B": {"hero": "https://h/b", "heroLow": "https://l/b"}}}
        ok = {"https://h/a", "https://l/b"}
        n, bad = verify_hero.verify(doc, lambda u: u in ok, fix=True)
        self.assertEqual((n, bad), (2, ["A", "B"]))
        self.assertEqual(doc["titles"]["A"], {"hero": "https://h/a"})
        self.assertEqual(doc["titles"]["B"], {})


def webp(kind, w, h):
    head = b"RIFF" + (1000).to_bytes(4, "little") + b"WEBP"
    if kind == "lossy":
        return head + b"VP8 " + (500).to_bytes(4, "little") + b"\x00\x00\x00" + b"\x9d\x01\x2a" \
            + w.to_bytes(2, "little") + h.to_bytes(2, "little") + b"\x00" * 40
    if kind == "lossless":
        bits = (w - 1) | ((h - 1) << 14)
        return head + b"VP8L" + (500).to_bytes(4, "little") + b"\x2f" + bits.to_bytes(4, "little") + b"\x00" * 40
    return head + b"VP8X" + (10).to_bytes(4, "little") + b"\x00" * 4 + (w - 1).to_bytes(3, "little") \
        + (h - 1).to_bytes(3, "little") + b"\x00" * 40


def page(content, attr="name"):
    return '<html><head><title>x</title><meta %s="twitter:image" content="%s"><meta name="a" content="b"></head></html>' \
        % (attr, content)


class FakeWeb:
    """get_text / get_bytes fake with the real extract contract."""

    def __init__(self, pages, images):
        self.pages, self.images, self.calls = pages, images, []

    def get_text(self, url, limit=65536, extract=None, ttl=None, cache=True):
        self.calls.append(url)
        if url not in self.pages:
            raise HttpError(404, url)
        return extract(self.pages[url]) if extract else self.pages[url]

    def get_bytes(self, url, start=0, length=64, ttl=None, cache=True):
        self.calls.append(url)
        if url not in self.images:
            raise HttpError(404, url)
        return self.images[url][:length]


class Pic0Tests(unittest.TestCase):
    def look(self, html, img=None, ppsa="PPSA01234"):
        web = FakeWeb({"https://prosperopatches.com/" + ppsa: html}, {PIC0: img} if img else {})
        return pic0.Pic0(web).lookup(Entry(ppsa, "T")), web

    def test_webp_dimensions(self):
        self.assertEqual(pic0.webp_size(webp("lossy", 1920, 1080)), (1920, 1080))
        self.assertEqual(pic0.webp_size(webp("lossless", 1920, 1080)), (1920, 1080))
        self.assertEqual(pic0.webp_size(webp("extended", 1920, 1080)), (1920, 1080))
        self.assertIsNone(pic0.webp_size(b"RIFF" + b"\x00" * 60))
        self.assertIsNone(pic0.webp_size(b"\x89PNG" + b"\x00" * 60))
        self.assertIsNone(pic0.webp_size(b"RIFF"))

    def test_hit_for_each_encoding(self):
        for kind in ("lossy", "lossless", "extended"):
            s, web = self.look(page(ICON), webp(kind, 1920, 1080))
            self.assertEqual((s.image_url, s.image_kind, s.confidence), (PIC0, "pic0", 1.0))
            self.assertAlmostEqual(s.image_ratio, 16 / 9, places=3)

    def test_meta_property_attribute_also_works(self):
        s, _ = self.look(page(ICON, "property"), webp("lossy", 1920, 1080))
        self.assertIsNotNone(s)

    def test_banner_and_unknown_page_rejected(self):
        s, web = self.look(page("https://prosperopatches.com/assets/images/banner.webp"), webp("lossy", 1920, 1080))
        self.assertIsNone(s)
        self.assertEqual(len(web.calls), 1)  # never probed an image
        self.assertIsNone(self.look("<html>no meta</html>")[0])

    def test_wrong_id_and_malformed_urls_rejected(self):
        other = ICON.replace("PPSA01234", "PPSA99999")
        bad = [other, ICON.replace("https://", "http://"), ICON + "?x=1", ICON.replace("a1" * 32, "A1" * 32),
               ICON.replace("cdn.prosperopatches.com", "cdn.prosperopatches.com.evil.test"),
               ICON.replace("a1" * 32, "a1" * 31), ICON.replace("icon0", "pic0"), "https://u@" + ICON[8:]]
        for c in bad:
            self.assertIsNone(pic0.icon_url_for(c, "PPSA01234"), c)
            self.assertIsNone(self.look(page(c), webp("lossy", 1920, 1080))[0])
        self.assertEqual(pic0.icon_url_for(ICON, "PPSA01234"), ICON)

    def test_image_shape_checks(self):
        for img in (webp("lossy", 1920, 400), webp("lossy", 1000, 562), webp("lossy", 1280, 1280), b"nope" * 20):
            self.assertIsNone(self.look(page(ICON), img)[0])
        self.assertIsNotNone(self.look(page(ICON), webp("lossy", 1280, 720))[0])
        self.assertIsNone(self.look(page(ICON))[0])  # pic0.webp missing -> no pic0

    def test_cusa_skipped_and_errors_are_provider_errors(self):
        web = FakeWeb({}, {})
        self.assertIsNone(pic0.Pic0(web).lookup(Entry("CUSA00002", "T")))
        self.assertEqual(web.calls, [])

        class Boom(FakeWeb):
            def get_text(self, *a, **k):
                raise HttpError(503, "x")
        with self.assertRaises(ProviderError):
            pic0.Pic0(Boom({}, {})).lookup(Entry("PPSA01234", "T"))

    def test_cache_stores_small_extract_and_bytes(self):
        class Inner:
            n = 0

            def request(self, method, url, params=None, headers=None, body=None, parse="json", limit=None):
                Inner.n += 1
                return page(ICON) if parse == "text" else webp("lossy", 1920, 1080)
        with tempfile.TemporaryDirectory() as d:
            for _ in range(2):
                s = pic0.Pic0(CachedHttp(Inner(), d)).lookup(Entry("PPSA01234", "T"))
                self.assertEqual(s.image_url, PIC0)
            self.assertEqual(Inner.n, 2)  # second run fully served from disk
            for root, _, files in os.walk(d):
                for f in files:
                    with open(os.path.join(root, f)) as fh:
                        self.assertNotIn("<html>", fh.read())

    def test_shared_rate_budget_for_both_hosts(self):
        t = [0.0]
        slept = []

        def sleep(d):
            slept.append(d)
            t[0] += d
        rl = RateLimiter({"prosperopatches.com": 1.0}, clock=lambda: t[0], sleep=sleep,
                         groups={"cdn.prosperopatches.com": "prosperopatches.com"})
        for h in ("prosperopatches.com", "cdn.prosperopatches.com", "prosperopatches.com"):
            rl.wait(h)
        self.assertAlmostEqual(sum(slept), 2.0)

    def test_build_registers_pic0_and_hides_it_from_sources(self):
        provs, _ = build.make_providers(["pic0"], FakeWeb({}, {}), datetime.date(2026, 10, 11), {})
        self.assertEqual([p.name for p in provs], ["pic0"])
        doc, _ = build.build_document([], {}, ["steam", "pic0"],
                                      datetime.datetime(2026, 10, 11, tzinfo=datetime.timezone.utc))
        self.assertEqual(doc["sources"], ["steam"])
        self.assertIn("pic0", doc["attribution"])


class CatalogueExportTests(unittest.TestCase):
    def test_titles_json_from_in_app_export(self):
        rows = [{"titleId": "PPSA00009", "title": "B"}, {"titleId": "CUSA00001", "title": "A"},
                {"titleId": "PPSA00009", "title": "dup"}, {"titleId": "NOPE", "title": "x"}]
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "titles.json")
            with open(p, "w", encoding="utf-8") as f:
                json.dump(rows, f)
            es = build.load_catalogue(p)
        self.assertEqual([(e.title_id, e.title) for e in es], [("CUSA00001", "A"), ("PPSA00009", "B")])


class ExtractTests(unittest.TestCase):
    def test_extract_only_ids_and_titles(self):
        import extract_titles
        rows = extract_titles.extract(os.path.join(FIX, "library_mini.json"))
        self.assertEqual(rows, [{"titleId": "CUSA00002", "title": "Elden Ring"},
                                {"titleId": "PPSA00001", "title": "Hades II"}])


if __name__ == "__main__":
    unittest.main()
