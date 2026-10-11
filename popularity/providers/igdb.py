"""IGDB (api-docs.igdb.com). DISABLED by default: only runs when explicitly listed in
--providers AND TWITCH_CLIENT_ID / TWITCH_CLIENT_SECRET are in the environment.

Secrets come from env only (never argv), travel only in the token POST body and the
Client-ID / Authorization headers, are never cached (cache=False) and never logged or put
in error text. One token per run. Limits: 4 req/s (net.py), backend only.
PS5 platform id 167, PS4 48 (from memory in the research doc; unverified)."""
import datetime
import os
import urllib.parse

from ..matcher import Candidate, rank
from ..models import Provider, ProviderError, Signal
from ..net import HttpError

TOKEN_URL = "https://id.twitch.tv/oauth2/token"
GAMES_URL = "https://api.igdb.com/v4/games"
PLATFORMS = "(167,48)"
FIELDS = "name,first_release_date,platforms,total_rating,total_rating_count,rating,rating_count"


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
        return Signal(self.name, g["id"], g.get("name", ""), conf, amb, rating=round(float(rating), 1),
                      rating_count=int(count or 0))
