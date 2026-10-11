"""Title normalisation. ``clean_title`` keeps case (for humans / catalogue-titles.json);
``normalize`` produces the comparison key plus a set of edition words."""
import re
import unicodedata

EDITION_WORDS = {
    "remastered", "remaster", "remake", "deluxe", "ultimate", "definitive", "complete", "goty", "edition",
    "collection", "directors", "cut", "final", "enhanced", "anniversary", "premium", "standard", "gold",
    "legendary", "special", "digital", "bundle", "upgrade",
}
STOP_FORMAT = {
    "usa", "eur", "jpn", "asia", "backport", "fpkg", "pkg", "update", "dlc", "demo", "exfat", "ps5", "ps4",
    "ppsa", "cusa", "patch", "repack", "multi", "region", "free",
}
ROMAN = {"ii": "2", "iii": "3", "iv": "4", "v": "5", "vi": "6", "vii": "7", "viii": "8", "ix": "9"}
_TM = re.compile("[™®©]")
_BRACKETS = re.compile(r"\[[^\]]*\]|\([^)]*\)")
_VERSION = re.compile(r"\bv?\d{1,2}\.\d{2,3}(?:\.\d+)*\b", re.I)
_PACKAGER = re.compile(r"@\w+")


def clean_title(title):
    t = _TM.sub("", title or "")
    t = unicodedata.normalize("NFKC", t)
    t = _BRACKETS.sub(" ", t)
    t = _PACKAGER.sub(" ", t)
    t = _VERSION.sub(" ", t)
    return re.sub(r"\s+", " ", t).strip(" -_.,")


class Norm:
    __slots__ = ("key", "tokens", "editions")

    def __init__(self, key, tokens, editions):
        self.key, self.tokens, self.editions = key, tokens, frozenset(editions)


def normalize(title):
    t = clean_title(title)
    t = unicodedata.normalize("NFKD", t)
    t = "".join(c for c in t if not unicodedata.combining(c)).lower()
    t = t.replace("&", " and ")
    t = re.sub(r"['’`]", "", t)
    t = re.sub(r"[^a-z0-9]+", " ", t)
    toks = [w for w in t.split() if w not in STOP_FORMAT]
    if toks and toks[0] == "the":
        toks = toks[1:]
    toks = [ROMAN.get(w, w) if i > 0 else w for i, w in enumerate(toks)]
    editions = {w for w in toks if w in EDITION_WORDS}
    core = [w for w in toks if w not in EDITION_WORDS]
    core = core or toks
    return Norm(" ".join(core), core, editions)
