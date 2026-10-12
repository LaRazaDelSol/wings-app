"""Injectable HTTP layer: rate limiting, retries, on-disk response cache.

Providers only call ``http.get_json`` / ``http.post_json``; tests pass a fake
object with the same two methods, so no test ever touches the network.
Error text never includes query strings, bodies or headers (secrets).
"""
import gzip
import hashlib
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


class HttpError(Exception):
    def __init__(self, status, where):
        super().__init__("HTTP %s from %s" % (status, where))
        self.status = status


def _where(url):
    p = urllib.parse.urlsplit(url)
    return p.netloc + p.path


class RateLimiter:
    """Spaces requests per host. Thread safe; sleeping happens outside the lock."""

    def __init__(self, intervals=None, default=0.25, clock=time.monotonic, sleep=time.sleep):
        self.intervals = dict(intervals or {})
        self.default = default
        self._next = {}
        self._lock = threading.Lock()
        self._clock, self._sleep = clock, sleep

    def wait(self, host):
        gap = self.intervals.get(host, self.default)
        with self._lock:
            now = self._clock()
            slot = max(now, self._next.get(host, 0.0))
            self._next[host] = slot + gap
        if slot > now:
            self._sleep(slot - now)


class UrlLibHttp:
    def __init__(self, user_agent, limiter=None, timeout=30, retries=3, sleep=time.sleep):
        self.user_agent = user_agent
        self.limiter = limiter or RateLimiter()
        self.timeout, self.retries, self._sleep = timeout, retries, sleep

    def request(self, method, url, params=None, headers=None, body=None):
        if params:
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(params)
        hdr = {"User-Agent": self.user_agent, "Accept-Encoding": "gzip", "Accept": "application/json"}
        hdr.update(headers or {})
        data = body.encode("utf-8") if isinstance(body, str) else body
        host = urllib.parse.urlsplit(url).netloc
        for attempt in range(self.retries + 1):
            self.limiter.wait(host)
            req = urllib.request.Request(url, data=data, headers=hdr, method=method)
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    raw = r.read()
                    if r.headers.get("Content-Encoding") == "gzip":
                        raw = gzip.decompress(raw)
                    return json.loads(raw.decode("utf-8"))
            except urllib.error.HTTPError as e:
                if e.code in (429, 500, 502, 503, 504) and attempt < self.retries:
                    try:
                        delay = float(e.headers.get("Retry-After", ""))
                    except ValueError:
                        delay = 2.0 * (2 ** attempt)
                    self._sleep(min(delay, 60.0))
                    continue
                raise HttpError(e.code, _where(url)) from None
            except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError):
                if attempt < self.retries:
                    self._sleep(2.0 * (2 ** attempt))
                    continue
                raise HttpError("network", _where(url)) from None

    def get_json(self, url, params=None, headers=None, cache=True):
        return self.request("GET", url, params, headers)

    def post_json(self, url, body, headers=None, params=None, cache=True):
        return self.request("POST", url, params, headers, body)


class CachedHttp:
    """Disk cache around another client. Key ignores headers (so tokens are never stored).

    Pass ``cache=False`` for anything secret-bearing (token requests)."""

    def __init__(self, inner, directory, ttl_seconds=6 * 86400, clock=time.time):
        self.inner, self.dir, self.ttl, self._clock = inner, directory, ttl_seconds, clock

    def _path(self, method, url, params, body):
        raw = json.dumps([method, url, sorted((params or {}).items()), body if isinstance(body, str) else None])
        h = hashlib.sha256(raw.encode("utf-8")).hexdigest()
        return os.path.join(self.dir, h[:2], h + ".json")

    def _go(self, method, url, params, headers, body, cache):
        path = self._path(method, url, params, body) if cache else None
        if path and os.path.exists(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    e = json.load(f)
                if self._clock() - e["t"] < self.ttl:
                    if "err" in e:
                        raise HttpError(e["err"], _where(url))
                    return e["body"]
            except (ValueError, KeyError, OSError):
                pass
        try:
            res = self.inner.request(method, url, params, headers, body) if hasattr(self.inner, "request") \
                else (self.inner.get_json(url, params, headers) if method == "GET"
                      else self.inner.post_json(url, body, headers, params))
            entry = {"t": self._clock(), "body": res}
        except HttpError as e:
            if e.status != 404:
                raise
            entry, res = {"t": self._clock(), "err": 404}, None
        if path:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(entry, f)
            os.replace(tmp, path)
        if res is None:
            raise HttpError(404, _where(url))
        return res

    def get_json(self, url, params=None, headers=None, cache=True):
        return self._go("GET", url, params, headers, None, cache)

    def post_json(self, url, body, headers=None, params=None, cache=True):
        return self._go("POST", url, params, headers, body, cache)
