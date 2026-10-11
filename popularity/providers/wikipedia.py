"""Wikipedia: article search + 30-day mean daily pageviews (attention only, no rating).

Policy (wikitech Robot_policy): descriptive User-Agent with contact, concurrency <= 3,
< 5 req/s (the RateLimiter in net.py is set to 4/s for these hosts). Pageview data is CC0.
"""
import datetime
import html
import re
import urllib.parse

from ..matcher import Candidate, rank
from ..models import Provider, ProviderError, Signal
from ..net import HttpError

API = "https://en.wikipedia.org/w/api.php"
VIEWS = ("https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/"
         "en.wikipedia/all-access/user/%s/daily/%s/%s")
GAMEISH = re.compile(r"video game|\bgame\b|developed by|published by|role-playing|roguelike|platform game", re.I)
SUFFIX = re.compile(r"\s*\((?:(\d{4})\s+)?(?:[\w\s-]*)?video game\)\s*$", re.I)
DAYS = 30


class Wikipedia(Provider):
    name = "wikipedia"

    def __init__(self, http, today=None, days=DAYS):
        self.http, self.days = http, days
        self.today = today or datetime.date.today()

    def _candidates(self, title):
        res = self.http.get_json(API, params={
            "action": "query", "list": "search", "srsearch": '"%s" video game' % title,
            "srlimit": "6", "srprop": "snippet", "format": "json", "formatversion": "2"})
        out = []
        for hit in res.get("query", {}).get("search", []):
            t = hit.get("title", "")
            snippet = html.unescape(re.sub(r"<[^>]+>", "", hit.get("snippet", "")))
            m = SUFFIX.search(t)
            base = SUFFIX.sub("", t)
            year = int(m.group(1)) if m and m.group(1) else None
            if year is None:
                ym = re.search(r"\b(?:is|was) an? (\d{4}) ", snippet)
                year = int(ym.group(1)) if ym else None
            ok = bool(m) or bool(GAMEISH.search(snippet))
            out.append(Candidate(t, base, year=year, platform_ok=ok))
        return out

    def _mean_views(self, page):
        end = self.today - datetime.timedelta(days=1)
        start = end - datetime.timedelta(days=self.days - 1)
        url = VIEWS % (urllib.parse.quote(page.replace(" ", "_"), safe=""), start.strftime("%Y%m%d"),
                       end.strftime("%Y%m%d"))
        try:
            items = self.http.get_json(url).get("items", [])
        except HttpError as e:
            if e.status == 404:
                return None
            raise
        if not items:
            return None
        return sum(i.get("views", 0) for i in items) / float(self.days)

    def lookup(self, entry, override=None):
        try:
            if override:
                page, conf, amb = str(override), 1.0, False
            else:
                m = rank(entry.title, self._candidates(entry.title), entry.year)
                if not m:
                    return None
                page, conf, amb = m.candidate.id, m.confidence, m.ambiguous
            views = self._mean_views(page)
        except HttpError as e:
            raise ProviderError(str(e)) from None
        if views is None:
            return None
        return Signal(self.name, page, page, conf, amb, attention=round(views, 2), attention_kind="views")
