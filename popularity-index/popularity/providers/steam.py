"""Steam: name search (undocumented storesearch, see research) + official
IUserReviewsService/GetAppReviews summary. Publishes only percent positive and count.
Anonymous use; ~1 req/s with backoff (net.py).Also derives the public hero art URL from the matched appid (no extra request).
"""
import json

from ..matcher import Candidate, rank
from ..models import Provider, ProviderError, Signal
from ..net import HttpError

SEARCH = "https://store.steampowered.com/api/storesearch/"
HERO = "https://cdn.cloudflare.steamstatic.com/steam/apps/%d/library_hero.jpg"
STEAM_HERO_RATIO = 1920 / 620  # library_hero.jpg is always 1920x620
REVIEWS = "https://api.steampowered.com/IUserReviewsService/GetAppReviews/v1/"


class Steam(Provider):
    name = "steam"

    def __init__(self, http):
        self.http = http

    def _candidates(self, title):
        res = self.http.get_json(SEARCH, params={"term": title, "cc": "us", "l": "en"})
        out = []
        for it in res.get("items", []):
            if it.get("type") != "app":
                continue
            win = (it.get("platforms") or {}).get("windows", True)  # PC build must exist
            out.append(Candidate(it["id"], it.get("name", ""), platform_ok=bool(win)))
        return out

    def _summary(self, appid):
        res = self.http.get_json(REVIEWS, params={"input_json": json.dumps(
            {"appid": int(appid), "num_per_page": 0, "language": "all", "purchase_type": "all"},
            separators=(",", ":"))})
        qs = (res.get("response") or res).get("query_summary") or {}
        pos, total = qs.get("total_positive"), qs.get("total_reviews")
        if not total:
            return None
        return round(100.0 * pos / total, 1), int(total)

    def lookup(self, entry, override=None):
        try:
            if override:
                appid, name, conf, amb = int(override), str(override), 1.0, False
            else:
                m = rank(entry.title, self._candidates(entry.title), entry.year)
                if not m:
                    return None
                appid, name, conf, amb = m.candidate.id, m.candidate.name, m.confidence, m.ambiguous
            s = self._summary(appid)
        except HttpError as e:
            raise ProviderError(str(e)) from None
        if s is None:
            return None
        pct, total = s
        return Signal(self.name, appid, name, conf, amb, rating=pct, rating_count=total,
                      attention=total, attention_kind="reviews", image_url=HERO % int(appid),
                      image_kind="steam", image_ratio=STEAM_HERO_RATIO)
