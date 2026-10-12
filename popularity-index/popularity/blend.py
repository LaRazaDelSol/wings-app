"""Score blend. Documented weights, missing sources handled by renormalisation.

score (0-100) = weighted mean of two groups, over the groups that have data:
  popularity group  (W_POPULARITY = 0.6): percentile (0-100, mid-rank) of each source's attention
                    across the catalogue: wikipedia 30-day mean views (0.5), steam review count (0.3),
                    igdb rating count (0.2; stand-in until PopScore primitives are wired in).
  quality group     (W_QUALITY = 0.4): shrunk rating (n*r + K*m)/(n+K), K = 20, m = catalogue mean of
                    that source, so a 100% rating from 3 reviews does not beat 95% from 40,000:
                    igdb total_rating (0.6), steam percent positive (0.4).
Inside a group the weights of the *available* sources are renormalised to sum to 1; across groups the
same happens. A title with only one group therefore scores on that group alone (no penalty, no
imputation). A title with no signal gets no record at all.

rating      = mean of raw (unshrunk) ratings of the available rating sources, weighted by QUAL_WEIGHTS.
ratingCount = sum of rating counts of those sources.   (both absent when no rating source matched)
confidence  = min match confidence over the signals used.
hero        = optional https artwork URL, best first: pic0 (1920x1080 PS5 background), IGDB screenshot, IGDB
              artwork (both ~16:9 only), Steam library_hero (3.1:1). Only from a signal with confidence
              >= 0.85 (HERO_MIN_CONFIDENCE) and not ambiguous; never an image whose ratio is outside 1.5-3.2.
heroLow     = optional small ~16:9 https URL (IGDB t_screenshot_big) for a fast first paint; only present
              next to a hero and when different from it. Neither affects the score.
Art-only sources (pic0) are left out of "sources" and "confidence" and never create a record by themselves.
"""
import bisect

W_POPULARITY = 0.6
W_QUALITY = 0.4
POP_WEIGHTS = {"wikipedia": 0.5, "steam": 0.3, "igdb": 0.2}
QUAL_WEIGHTS = {"igdb": 0.6, "steam": 0.4}
K = 20
DEFAULT_MEAN = 70.0


def _wmean(pairs):
    tw = sum(w for w, _ in pairs)
    return sum(w * v for w, v in pairs) / tw if tw else None


def percentile_tables(signals_by_title):
    vals = {}
    for sigs in signals_by_title.values():
        for s in sigs:
            if s.attention is not None and s.source in POP_WEIGHTS:
                vals.setdefault(s.source, []).append(s.attention)
    return {k: sorted(v) for k, v in vals.items()}


def _percentile(table, x):
    lo, hi = bisect.bisect_left(table, x), bisect.bisect_right(table, x)
    return 100.0 * (lo + 0.5 * (hi - lo)) / len(table)


def source_means(signals_by_title):
    acc = {}
    for sigs in signals_by_title.values():
        for s in sigs:
            if s.rating is not None:
                acc.setdefault(s.source, []).append(s.rating)
    return {k: sum(v) / len(v) for k, v in acc.items()}


HERO_MIN_CONFIDENCE = 0.85  # matcher.ACCEPT: art is only attached to confidently matched, unambiguous titles
HERO_SOURCES = ("pic0", "igdb", "steam")  # preference order (igdb already prefers screenshot over artwork)
ART_ONLY = ("pic0",)
MIN_RATIO, MAX_RATIO = 1.5, 3.2
URL_MAX = 300


def _url_ok(url):
    if not isinstance(url, str) or not url.startswith("https://") or len(url) > URL_MAX:
        return False
    host = url[len("https://"):].split("/", 1)[0]
    return bool(host) and " " not in url and "@" not in host


def _usable(s):
    return bool(s) and not s.ambiguous and s.confidence >= HERO_MIN_CONFIDENCE


def pick_art(sigs):
    """(hero, hero_low), each a URL or None. hero_low is None when there is no hero or it equals hero."""
    by = {s.source: s for s in sigs}
    hero = None
    for src in HERO_SOURCES:
        s = by.get(src)
        ratio = getattr(s, "image_ratio", None)
        if (_usable(s) and _url_ok(getattr(s, "image_url", None))
                and isinstance(ratio, (int, float)) and MIN_RATIO <= ratio <= MAX_RATIO):
            hero = s.image_url
            break
    low = None
    s = by.get("igdb")
    if hero and _usable(s) and _url_ok(getattr(s, "low_url", None)) and s.low_url != hero:
        low = s.low_url
    return hero, low


def pick_hero(sigs):
    return pick_art(sigs)[0]


def blend_one(sigs, tables, means):
    pop, qual, raw = [], [], []
    scored = [s for s in sigs if s.source not in ART_ONLY]
    for s in scored:
        if s.attention is not None and s.source in tables:
            pop.append((POP_WEIGHTS[s.source], _percentile(tables[s.source], s.attention)))
        if s.rating is not None and s.source in QUAL_WEIGHTS:
            n = s.rating_count or 0
            m = means.get(s.source, DEFAULT_MEAN)
            qual.append((QUAL_WEIGHTS[s.source], (n * s.rating + K * m) / (n + K)))
            raw.append((QUAL_WEIGHTS[s.source], s.rating, n))
    groups = []
    if pop:
        groups.append((W_POPULARITY, _wmean(pop)))
    if qual:
        groups.append((W_QUALITY, _wmean(qual)))
    if not groups:
        return None
    rec = {"score": int(round(_wmean(groups))), "sources": sorted(s.source for s in scored),
           "confidence": round(min(s.confidence for s in scored), 2)}
    hero, low = pick_art(sigs)
    if hero:
        rec["hero"] = hero
        if low:
            rec["heroLow"] = low
    if raw:
        rec["rating"] = int(round(_wmean([(w, r) for w, r, _ in raw])))
        rec["ratingCount"] = sum(n for _, _, n in raw)
    return rec


def blend(signals_by_title, fetched_at=None):
    """signals_by_title: {titleId: [Signal]} -> {titleId: record} (sorted by titleId)."""
    tables, means = percentile_tables(signals_by_title), source_means(signals_by_title)
    out = {}
    for tid in sorted(signals_by_title):
        sigs = signals_by_title[tid]
        rec = blend_one(sigs, tables, means) if sigs else None
        if rec:
            out[tid] = rec
    return out
