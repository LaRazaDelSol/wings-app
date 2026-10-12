"""IGDB (api-docs.igdb.com). DISABLED by default: only runs when explicitly listed in
--providers AND TWITCH_CLIENT_ID / TWITCH_CLIENT_SECRET are in the environment.

Secrets come from env only (never argv), travel only in the token POST body and the
Client-ID / Authorization headers, are never cached (cache=False) and never logged or put
in error text. One token per run. Limits: 4 req/s (net.py), backend only.
PS5 platform id 167, PS4 48 (from memory in the research doc; unverified)."""
import datetime
import os
import re
import urllib.parse

from ..matcher import Candidate, rank
from ..models import Provider, ProviderError, Signal
from ..net import HttpError

TOKEN_URL = "https://id.twitch.tv/oauth2/token"
GAMES_URL = "https://api.igdb.com/v4/games"
IMAGE = "https://images.igdb.com/igdb/image/upload/t_1080p/%s.jpg"
IMAGE_LOW = "https://images.igdb.com/igdb/image/upload/t_screenshot_big/%s.jpg"  # 889x500
TARGET = 16 / 9
MAX_DEVIATION = 0.15   # accepted ratio is within 15% of 16:9
MIN_WIDTH = 1280
MIN_WIDTH_LOW = 640
PLATFORMS = "(167,48)"
FIELDS = "name,first_release_date,platforms,total_rating,total_rating_count,rating,rating_count,artworks.image_id,artworks.width,artworks.height,screenshots.image_id,screenshots.width,screenshots.height"


class Igdb(Provider):
    name = "igdb"

    def __init__(self, http, environ=None):
        self.http = http
        env = os.environ if environ is None else environ
        self._id = env.get("TWITCH_CLIENT_ID", "")
        self._secret = env.get("TWITCH_CLIENT_SECRET", "")
        self._token = None

    def available(self):
        if not (self._id and self._secret):
            return False, "TWITCH_CLIENT_ID / TWITCH_CLIENT_SECRET not set"
        return True, ""

    def _auth(self):
        if self._token is None:
            body = urllib.parse.urlencode({"client_id": self._id, "client_secret": self._secret,
                                           "grant_type": "client_credentials"})
            try:
                res = self.http.post_json(
                    TOKEN_URL, body, headers={"Content-Type": "application/x-www-form-urlencoded"}, cache=False)
            except HttpError as e:
                raise ProviderError("IGDB token request failed (%s)" % e.status) from None
            self._token = res.get("access_token")
            if not self._token:
                raise ProviderError("IGDB token response had no access_token")
        return {"Client-ID": self._id, "Authorization": "Bearer " + self._token, "Content-Type": "text/plain"}

    def _query(self, apicalypse):
        return self.http.post_json(GAMES_URL, apicalypse, headers=self._auth())

    @staticmethod
    def _year(g):
        ts = g.get("first_release_date")
        if ts is None:
            return None
        return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).year

    @staticmethod
    def _best(items, min_width):
        """Closest to 16:9 among items with a plain image_id, real dims, width >= min_width and a ratio
        within MAX_DEVIATION of 16:9. Ties go to the wider image. Returns (image_id, ratio) or None."""
        best = None
        for it in items or []:
            if not isinstance(it, dict):
                continue
            iid, w, h = it.get("image_id"), it.get("width"), it.get("height")
            if not (isinstance(iid, str) and re.fullmatch(r"[a-z0-9]{1,40}", iid)):
                continue
            if not all(isinstance(v, int) and not isinstance(v, bool) and v > 0 for v in (w, h)):
                continue
            ratio = w / h
            dev = abs(ratio / TARGET - 1)
            if w < min_width or dev > MAX_DEVIATION:
                continue
            key = (round(dev, 4), -w)
            if best is None or key < best[0]:
                best = (key, iid, ratio)
        return (best[1], best[2]) if best else None

    @classmethod
    def _image_url(cls, g):
        """(hero_url, kind, ratio, low_url). hero: best 16:9 screenshot at t_1080p, else best 16:9
        artwork; banners and odd shapes are never returned. low: small screenshot (t_screenshot_big)."""
        shot = cls._best(g.get("screenshots"), MIN_WIDTH)
        art = None if shot else cls._best(g.get("artworks"), MIN_WIDTH)
        low = cls._best(g.get("screenshots"), MIN_WIDTH_LOW)
        low_url = IMAGE_LOW % low[0] if low else None
        if shot:
            return IMAGE % shot[0], "screenshot", shot[1], low_url
        if art:
            return IMAGE % art[0], "artwork", art[1], low_url
        return None, None, None, low_url

    def lookup(self, entry, override=None):
        try:
            if override:
                rows = self._query("fields %s; where id = %d; limit 1;" % (FIELDS, int(override)))
                if not rows:
                    return None
                g, conf, amb = rows[0], 1.0, False
            else:
                q = entry.title.replace('"', " ")
                rows = self._query('search "%s"; fields %s; where platforms = %s; limit 5;' % (q, FIELDS, PLATFORMS))
                m = rank(entry.title, [Candidate(r["id"], r.get("name", ""), self._year(r), extra=r) for r in rows],
                         entry.year)
                if not m:
                    return None
                g, conf, amb = m.candidate.extra, m.confidence, m.ambiguous
        except HttpError as e:
            raise ProviderError("IGDB request failed (%s)" % e.status) from None
        rating = g.get("total_rating", g.get("rating"))
        count = g.get("total_rating_count", g.get("rating_count"))
        if rating is None:
            return None
        url, kind, ratio, low = self._image_url(g)
        return Signal(self.name, g["id"], g.get("name", ""), conf, amb, rating=round(float(rating), 1),
                      rating_count=int(count or 0), image_url=url, image_kind=kind, image_ratio=ratio,
                      low_url=low)
