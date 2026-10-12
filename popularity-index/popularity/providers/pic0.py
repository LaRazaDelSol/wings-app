"""pic0: the PS5 background image (1920x1080) of a title, via its Prosperopatches page. Art only: it
never contributes to the score. Optional, enabled by listing "pic0" in --providers.

For a PPSA id the page https://prosperopatches.com/<PPSAxxxxx> carries a twitter:image meta tag that
points at .../titles/<PPSAxxxxx>_<64 hex>/icon0.webp for real titles (unknown ids get a generic site
banner, which is rejected). pic0.webp sits next to icon0.webp. The result is accepted only when:
  * the meta URL matches ICON_RE exactly and its id equals the requested id, and
  * a 64-byte Range request of pic0.webp is a WebP at least 1280 px wide with ratio 1.5-2.0.
Politeness: one shared 1 req/s budget for both hosts (see build.py), honest User-Agent, cached for weeks
(the hashed paths are stable) so reruns only fetch new titles. A failure never fails the run."""
import re
from html.parser import HTMLParser

from ..models import Provider, ProviderError, Signal
from ..net import HttpError

PAGE = "https://prosperopatches.com/%s"
ICON_RE = re.compile(r"https://cdn\.prosperopatches\.com/titles/(PPSA\d{5})_[0-9a-f]{64}/icon0\.webp")
ID_RE = re.compile(r"PPSA\d{5}")
MIN_WIDTH = 1280
MIN_RATIO, MAX_RATIO = 1.5, 2.0
PAGE_TTL = 30 * 86400
IMAGE_TTL = 180 * 86400


class _Meta(HTMLParser):
    def __init__(self):
        super().__init__()
        self.found = None

    def handle_starttag(self, tag, attrs):
        if tag != "meta" or self.found is not None:
            return
        a = dict(attrs)
        if (a.get("name") or a.get("property") or "").lower() == "twitter:image" and a.get("content"):
            self.found = a["content"].strip()


def twitter_image(html):
    """Raw content of the first twitter:image meta tag, or ''. Small, so it is what gets cached."""
    p = _Meta()
    try:
        p.feed(html)
    except Exception:
        pass
    return p.found or ""


def icon_url_for(content, ppsa):
    """The icon0.webp URL if content passes the strict pattern and belongs to ppsa, else None."""
    m = ICON_RE.fullmatch(content or "")
    return content if m and m.group(1) == ppsa else None


def pic0_url(icon_url):
    return icon_url[:-len("icon0.webp")] + "pic0.webp"


def webp_size(data):
    """(width, height) from the first 30 bytes of a WebP (VP8, VP8L or VP8X), else None."""
    if len(data) < 30 or not (data.startswith(b"RIFF") and data[8:12] == b"WEBP"):
        return None
    chunk = data[12:16]
    if chunk == b"VP8X":
        return 1 + int.from_bytes(data[24:27], "little"), 1 + int.from_bytes(data[27:30], "little")
    if chunk == b"VP8 " and data[23:26] == b"\x9d\x01\x2a":
        return int.from_bytes(data[26:28], "little") & 0x3FFF, int.from_bytes(data[28:30], "little") & 0x3FFF
    if chunk == b"VP8L" and data[20] == 0x2F:
        bits = int.from_bytes(data[21:25], "little")
        return (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1
    return None


def good_size(size):
    return bool(size) and size[0] >= MIN_WIDTH and size[1] > 0 and MIN_RATIO <= size[0] / size[1] <= MAX_RATIO


class Pic0(Provider):
    name = "pic0"

    def __init__(self, http):
        self.http = http

    def lookup(self, entry, override=None):
        ppsa = override if isinstance(override, str) and ID_RE.fullmatch(override) else entry.title_id
        if not ID_RE.fullmatch(ppsa):
            return None  # PS4 (CUSA) ids have no pic0
        try:
            content = self.http.get_text(PAGE % ppsa, limit=65536, extract=twitter_image, ttl=PAGE_TTL)
            icon = icon_url_for(content, ppsa)
            if not icon:
                return None
            url = pic0_url(icon)
            head = self.http.get_bytes(url, 0, 64, ttl=IMAGE_TTL)
        except HttpError as e:
            if e.status in (404, 403, 410, 416):
                return None
            raise ProviderError("pic0 request failed (%s)" % e.status) from None
        except Exception as e:  # never let this optional provider fail the run
            raise ProviderError("pic0 failed (%s)" % type(e).__name__) from None
        size = webp_size(head)
        if not good_size(size):
            return None
        return Signal(self.name, ppsa, "", 1.0, image_url=url, image_kind="pic0", image_ratio=size[0] / size[1])
