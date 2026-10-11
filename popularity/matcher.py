"""Candidate ranking with confidence.

confidence = best similarity (token Jaccard + Jaro-Winkler on normalised keys, exact key = 1.0)
with bonuses/penalties for edition words and release year. If the runner-up (a *different*
external id) is within AMBIGUITY_GAP the result is flagged ambiguous and confidence is capped
at AMBIGUOUS_CAP: a low-confidence answer, never a silent guess.
"""
import re

from .normalize import normalize

ACCEPT = 0.85       # auto-accept at or above
MIN_KEEP = 0.60     # below this: no match (dropped)
AMBIGUITY_GAP = 0.04
AMBIGUOUS_CAP = 0.50


class Candidate:
    def __init__(self, id, name, year=None, platform_ok=True, extra=None):
        self.id, self.name, self.year, self.platform_ok, self.extra = id, name, year, platform_ok, extra or {}


class Match:
    def __init__(self, candidate, confidence, ambiguous, second=None):
        self.candidate, self.confidence, self.ambiguous, self.second = candidate, confidence, ambiguous, second


def jaro_winkler(a, b):
    if a == b:
        return 1.0
    la, lb = len(a), len(b)
    if not la or not lb:
        return 0.0
    rng = max(max(la, lb) // 2 - 1, 0)
    ma, mb = [False] * la, [False] * lb
    m = 0
    for i, ca in enumerate(a):
        for j in range(max(0, i - rng), min(lb, i + rng + 1)):
            if not mb[j] and b[j] == ca:
                ma[i] = mb[j] = True
                m += 1
                break
    if not m:
        return 0.0
    t, k = 0, 0
    for i in range(la):
        if ma[i]:
            while not mb[k]:
                k += 1
            if a[i] != b[k]:
                t += 1
            k += 1
    jaro = (m / la + m / lb + (m - t / 2) / m) / 3
    p = 0
    while p < min(4, la, lb) and a[p] == b[p]:
        p += 1
    return jaro + p * 0.1 * (1 - jaro)


def similarity(qa, qb):
    if qa.key == qb.key:
        return 1.0
    sa, sb = set(qa.tokens), set(qb.tokens)
    jac = len(sa & sb) / len(sa | sb) if sa | sb else 0.0
    return 0.5 * jac + 0.5 * jaro_winkler(qa.key, qb.key)


def score(query, cand, year=None):
    n = normalize(cand.name)
    s = similarity(query, n)
    if query.editions != n.editions:
        s -= 0.08
    if year and cand.year:
        d = abs(int(year) - int(cand.year))
        s += 0.05 if d <= 1 else (-0.15 if d > 2 else 0.0)
    return max(0.0, min(1.0, s))


NON_BASE = {"bundle", "soundtrack", "artbook", "dlc", "pack", "demo", "tools", "ost", "playtest",
            "expansion", "wallpaper", "skin", "skins", "cosmetic", "supporter"}


def _is_base_content(title, cand):
    """Reject DLC/bundle/soundtrack listings unless the catalogue title itself names them."""
    words = lambda t: set(re.findall(r"[a-z0-9]+", t.lower()))  # noqa: E731
    return not ((words(cand.name) & NON_BASE) - words(title))


def rank(title, candidates, year=None):
    """Return a Match, or None when nothing reaches MIN_KEEP."""
    q = normalize(title)
    scored = sorted(((score(q, c, year), str(c.id), c) for c in candidates
                     if c.platform_ok and _is_base_content(title, c)),
                    key=lambda x: (-x[0], x[1]))
    if not scored or scored[0][0] < MIN_KEEP:
        return None
    best = scored[0]
    second = next((x for x in scored[1:] if x[2].id != best[2].id), None)
    conf, amb = best[0], False
    if second and best[0] - second[0] < AMBIGUITY_GAP and second[0] >= MIN_KEEP:
        conf, amb = min(conf, AMBIGUOUS_CAP), True
    return Match(best[2], round(conf, 3), amb, second[2] if second else None)
