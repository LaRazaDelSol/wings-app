"""Reference link-name parser for Wings (phase 5). Pure functions, stdlib only.

parse_link(name, url) -> dict of fields; group_parts(links) -> multipart sets.
Tested over all 35,559 links of recon/samples/library.json (0 exceptions, 0 residue).
The C++ port mirrors it step by step. Note: std::regex has no lookbehind; the
lookbehind guards below become a manual "previous char is not alnum/'.'" check.
"""
import collections
import re
from urllib.parse import urlsplit

TRUNC_BYTES = 63  # Pegasus stores link names in 63 UTF-8 bytes; longer names are cut

# Host alias as written in names -> canonical host label. Keys are lower case.
HOST_ALIASES = {
    "akirabox": "Akirabox", "akira": "Akirabox", "akia": "Akirabox",
    "akirabox.com": "Akirabox", "akirabox.to": "Akirabox",
    "vikingfile": "VikingFile", "viki": "VikingFile", "vikingfile.com": "VikingFile",
    "vik1ngfile.site": "VikingFile",
    "rootz": "Rootz", "1fichier": "1fichier", "mediafire": "MediaFire",
    "buzzheavier": "BuzzHeavier", "buzz": "BuzzHeavier", "ts": "BuzzHeavier",
    "rapidgator": "Rapidgator", "gofile": "Gofile", "gofile.io": "Gofile",
    "filecrypt": "Filecrypt", "ranoz": "Ranoz", "transfer.it": "Transfer.it", "mega": "Mega",
    "datanodes": "DataNodes", "datanodes.to": "DataNodes",
    "data": "DataNodes",  # DLPS "DATA (exFAT)"; PFS "Data - DLC" is datavaults (see URL rule)
    "filek": "FileKeeper", "filekeeper": "FileKeeper",
    "filed": "FileDitch", "fileditch": "FileDitch",
    "vault": "DataVaults", "datavaults.co": "DataVaults",
    "archive": "archive.org", "download": "",
    "fuckingfast.net": "FuckingFast", "urlvanish.com": "urlvanish", "tinyurl.com": "tinyurl",
    "1024terabox.com": "TeraBox", "link-vault.org": "link-vault",
}
# URL hosts that only wrap another host: the real host is the one named in the link.
WRAPPER_HOSTS = {"pippo26442999.github.io", "link-vault.org", "tinyurl.com", "urlvanish.com",
                 "filecrypt.cc"}
URL_HOST_LABEL = {
    "akirabox.com": "Akirabox", "akirabox.to": "Akirabox", "vikingfile.com": "VikingFile",
    "vik1ngfile.site": "VikingFile", "rootz.so": "Rootz", "1fichier.com": "1fichier",
    "mediafire.com": "MediaFire", "buzzheavier.com": "BuzzHeavier", "ts.buzzheavier.com": "BuzzHeavier",
    "rapidgator.net": "Rapidgator", "gofile.io": "Gofile", "filecrypt.cc": "Filecrypt",
    "ranoz.gg": "Ranoz", "transfer.it": "Transfer.it", "mega.nz": "Mega",
    "datanodes.to": "DataNodes", "datanodes.co": "DataNodes", "filekeeper.net": "FileKeeper",
    "fileditchfiles.st": "FileDitch", "theditch.st": "FileDitch", "datavaults.co": "DataVaults",
    "archive.org": "archive.org", "fuckingfast.net": "FuckingFast", "1024terabox.com": "TeraBox",
}
PACKAGER_ALIASES = {"pkykzhack": "Psykzhack", "psykzhack": "Psykzhack", "realromhunter": "realromhunter",
                    "romhunter": "realromhunter", "high-speed007": "High-Speed007",
                    "high-speed9": "High-Speed007"}

FW_RE = re.compile(r"(?<![\w.])(?:FW\s*)?([1-9]|1[0-9])\.(xx|\d\d)(\+)?(?![\w.])", re.I)
FW_COMPACT_RE = re.compile(r"(?<![\w.])([1-9])xx\b", re.I)          # 4xx, 7xx, 5xx
VER_RE = re.compile(r"(?<![\w.])v(\d{1,2}(?:\.\d{1,3}){1,3})(?![\w.])", re.I)
PART_RE = re.compile(r"(?:\bpart\s*|\bpt\.?\s*)(\d{1,3})\s*$", re.I)
PART_TRUNC_RE = re.compile(r"\s(?:p|pa|par|part)\s*$", re.I)
TAG_RE = re.compile(r"[\[(]\s*([^\[\]()]*?)\s*[\])]")


def norm_space(s):
    return re.sub(r"\s+", " ", s.replace("–", "-").replace("—", "-")).strip()


def split_top(s):
    """Split on ' - ' only outside brackets."""
    out, depth, cur, i = [], 0, "", 0
    while i < len(s):
        c = s[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth = max(0, depth - 1)
        if depth == 0 and s.startswith(" - ", i):
            out.append(cur); cur = ""; i += 3; continue
        if depth == 0 and s.startswith("- ", i) and cur.endswith("fichier"):  # '1fichier- 5.xx'
            out.append(cur); cur = ""; i += 2; continue
        cur += c; i += 1
    out.append(cur)
    return [x.strip() for x in out if x.strip() != ""] or [""]


def host_of_url(url):
    try:
        h = (urlsplit(url).hostname or "").lower()
    except ValueError:
        return ""
    return h[4:] if h.startswith("www.") else h


def match_host_word(seg, allow_prefix):
    """Return (label, rest) if seg starts with a host alias."""
    low = seg.lower()
    best = None
    for alias in sorted(HOST_ALIASES, key=len, reverse=True):
        a = alias.strip()
        if low == a or low.startswith(a + " ") or low.startswith(a + "(") or low.startswith(a + "-"):
            best = (HOST_ALIASES[alias], seg[len(a):].strip(" -"))
            break
    if best is None and allow_prefix and len(low) >= 2:
        first = low.split(" ")[0]
        for alias in sorted(HOST_ALIASES, key=len, reverse=True):
            if len(first) >= 4 and alias.startswith(first) and " " not in seg.strip():
                return HOST_ALIASES[alias], ""
    return best


def parse_link(name, url=""):
    raw = name
    f = {
        "region": None, "lang_from": None, "lang_to": None,
        "fw": None, "fw_plus": False, "backport": False, "backport_target": None,
        "kind": None, "kind_inferred": False, "all_dlc": False,
        "format": None, "build": None, "version": None, "release": None,
        "part": None, "part_unknown": False, "truncated": False,
        "tags": [], "packager": None, "host": None, "host_from": None,
        "size_text": None, "title_id": None, "label": "", "residue": "",
    }
    f["truncated"] = len(raw.encode("utf-8")) >= TRUNC_BYTES
    s = norm_space(raw)
    # 1. multipart suffix (always last)
    m = PART_RE.search(s)
    if m:
        f["part"] = int(m.group(1)); s = s[:m.start()].rstrip(" -")
    elif f["truncated"] and PART_TRUNC_RE.search(s):
        f["part_unknown"] = True; s = PART_TRUNC_RE.sub("", s)
    # 2. host segment: last top-level segment, else first (PFS puts the host first)
    segs = split_top(s)
    host_seg = None
    if len(segs) > 1:
        hm = match_host_word(segs[-1], allow_prefix=f["truncated"])
        if hm:
            host_seg = len(segs) - 1
        else:
            hm = match_host_word(segs[0], allow_prefix=False)
            if hm:
                host_seg = 0
    else:
        hm = match_host_word(segs[0], allow_prefix=False)
        if hm:
            host_seg = 0
    if host_seg is not None:
        f["host"], hrest = hm
        f["host_from"] = "name"
        segs[host_seg] = hrest
    desc = " ".join(x for x in segs if x)
    # host from URL wins unless the URL host is a wrapper
    uh = host_of_url(url)
    if uh and uh not in WRAPPER_HOSTS and uh in URL_HOST_LABEL:
        f["host"], f["host_from"] = URL_HOST_LABEL[uh], "url"
    elif uh and not f["host"]:
        f["host"], f["host_from"] = uh, "url"
    work = " " + desc + " "

    def take(pattern, flags=re.I):
        nonlocal work
        mm = re.search(pattern, work, flags)
        if mm:
            work = work[:mm.start()] + " " + work[mm.end():]
        return mm

    # 3. size / embedded title id (only the PPSA26344 family)
    mm = take(r"(\d+(?:\.\d+)?)\s*(gb|mb|tb)\b")
    if mm:
        f["size_text"] = mm.group(1) + " " + mm.group(2).upper()
    mm = take(r"\b([A-Z]{4}\d{5})\b", 0)
    if mm:
        f["title_id"] = mm.group(1)
    # 4. tags in brackets: packagers, APR-EMU, languages, DLC markers
    for mm in list(TAG_RE.finditer(work)):
        inner = norm_space(mm.group(1))
        low = inner.lower()
        consumed = True
        if low in ("apr-emu",):
            f["tags"].append("APR-EMU")
        elif "arabic" in low:
            f["tags"].append("+Arabic")
        elif low.startswith("v") and VER_RE.fullmatch(inner):
            f["version"] = inner[1:]
        elif re.fullmatch(r"(all\s+)?dlcs?(\s+merged)?", low) or re.fullmatch(r"dlcs?\s+merged", low) \
                or low.startswith("dlc ") or low in ("dlcs", "bundle dlcs") or low == "game + dlc" \
                or low.startswith("all dlc"):
            f["kind"] = f["kind"] or "game+dlc"
            if "all" in low or "merged" in low or "bundle" in low:
                f["all_dlc"] = True
            if low.startswith("dlc ") and low not in ("dlc merged",):
                f["label"] = inner[4:]
        elif low.startswith("backport"):
            f["backport"] = True
            fm = FW_RE.search(inner)
            if fm:
                f["backport_target"] = fm.group(1) + "." + fm.group(2).lower()
        elif low == "exfat":
            f["format"] = "exFAT"
        elif low.startswith("only work"):
            fm = FW_RE.search(inner)
            if fm:
                f["fw"] = fm.group(1) + "." + fm.group(2).lower()
        elif FW_RE.fullmatch(inner) or FW_COMPACT_RE.fullmatch(inner):
            fm = FW_RE.fullmatch(inner)
            f["fw"] = fm.group(1) + ".xx" if fm else inner[0] + ".xx"
        elif re.search(r"\b(pkg|emu)\b", low) or "@" in inner or low in ("duplex", "pkg"):
            if re.search(r"\bpkg\b", low):
                f["format"] = f["format"] or "PKG"
            if re.search(r"\bemu\b", low):
                f["tags"].append("EMU")
            names = re.findall(r"@\s*([\w\-]+)", inner)
            if not names:
                names = [x for x in re.split(r"\s+-\s+|\s*&\s*|\s+", inner)
                         if x and x.lower() not in ("pkg", "emu", "")]
            if names:
                f["packager"] = PACKAGER_ALIASES.get(names[0].lower(), names[0])
        else:
            consumed = False
        if consumed:
            work = work.replace(mm.group(0), " ", 1)
    # bare "by Psykzhack"
    mm = take(r"\bby\s+@?([\w\-]+)")
    if mm:
        f["packager"] = PACKAGER_ALIASES.get(mm.group(1).lower(), mm.group(1))
    # 5. region and "X to Y Languages"
    mm = take(r"\b(USA|EUR|JPN|ASIA|HK|KOR)\s+to\s+(EUR|JPN|USA)\s+lang(?:uage)?s\b")
    if mm:
        f["region"] = mm.group(1).upper(); f["lang_from"] = mm.group(1).upper(); f["lang_to"] = mm.group(2).upper()
    mm = take(r"\b(USA|EUR|JPN|ASIA|HK|KOR)\b", 0)
    if mm and not f["region"]:
        f["region"] = mm.group(1)
    # 6. release number
    mm = take(r"\brelease\s+(\d+)\b")
    if mm:
        f["release"] = int(mm.group(1))
    # 7. format words
    if take(r"\bexFAT\b"):
        f["format"] = "exFAT"
    if take(r"\bf?fpkg\b"):
        f["format"] = "FPKG"
    if take(r"\bffpfs\b"):
        f["format"] = "FFPFS"
    if take(r"\bpkg\b"):
        f["format"] = f["format"] or "PKG"
    if take(r"\bdump\b"):
        f["format"] = f["format"] or "Dump"
    # 8. backport (+ target), standard build
    while True:  # 'BackPort backport - AKIA Backport (exFAT)' says it twice
        mm = take(r"\bback\s*port\b(?:\s+(?:([1-9]|1[0-9])\.(xx|\d\d)|([1-9])xx|backport))?")
        if not mm:
            break
        f["backport"] = True
        if mm.group(1):
            f["backport_target"] = mm.group(1) + "." + mm.group(2).lower()
        elif mm.group(3):
            f["backport_target"] = mm.group(3) + ".xx"
    if f["truncated"] and take(r"\s(?:b|ba|bac|back|backp|backpo|backpor)\s*$"):
        f["backport"] = True  # cut-off 'Backport' at the 63-byte limit
    if take(r"\b(?:standard|std)\b"):
        f["build"] = "standard"
    # 9. firmware tokens (left of the backport keyword = original build fw)
    fws = []
    while True:
        mm = take(r"(?<![\w.])(?:FW\s*)?([1-9]|1[0-9])\.(xx|\d\d)(\+)?(?![\w.])")
        if not mm:
            mm2 = take(r"(?<![\w.])([1-9])xx\b(\+)?")
            if not mm2:
                break
            fws.append((mm2.group(1) + ".xx", bool(mm2.group(2))))
            continue
        fws.append((mm.group(1) + "." + mm.group(2).lower(), bool(mm.group(3))))
    if fws:
        f["fw"] = f["fw"] or fws[0][0]
        f["fw_plus"] = any(p for _, p in fws)
        if f["backport"] and not f["backport_target"] and not f["region"]:
            # Pippo '4.xx BackPort', PFS '4.xx Backport', 'Backport - 4.xx+': the one fw IS the target.
            # DLPS 'USA 12.xx BackPort' (region first) names the ORIGINAL build; target unknown.
            f["backport_target"] = fws[-1][0]
            if len(fws) == 1:
                f["fw"] = None
    # 10. version: vNN.NNN or bare NN.NNN(.NNN)
    mm = take(r"(?<![\w.])v(\d{1,2}(?:\.\d{1,3}){1,3})(?![\w.])")
    if mm:
        f["version"] = f["version"] or mm.group(1)
    mm = take(r"(?<![\w.])(\d{1,2}\.\d{2,4}(?:\.\d{1,3}){0,2})(?![\w.])")
    if mm:
        f["version"] = f["version"] or mm.group(1)
    mm = take(r"\bupdate\b")
    if mm:
        f["kind"] = "update"
    # 11. kind words
    if take(r"\bfull\s+game\s+unlock\b"):      # PFS 'FULL GAME UNLOCK (DUPLEX - PKG) - DLC'
        f["label"] = "Full game unlock"
    has_game = bool(take(r"\b(?:download\s+)?(?:base\s+)?game\b|\bbase\b"))
    if has_game:
        while take(r"\bbase\b|\bgame\b"):
            pass
    dlcm = take(r"(?:\+\s*)?(?:\bonly\s+)?(?:\ball\s+)?\bdlcs?\b(?:\s+merged)?(?:\s+unlocker)?")
    while dlcm and take(r"\bdlcs?\b"):
        pass  # PFS repeats it: 'Data - DLC by Psykzhack - DLC'
    if dlcm:
        if "all" in dlcm.group(0).lower() or "merged" in dlcm.group(0).lower():
            f["all_dlc"] = True
    if f["kind"] == "update":
        pass
    elif has_game and (dlcm or f["kind"] == "game+dlc"):
        f["kind"] = "game+dlc"
    elif dlcm:
        f["kind"] = "dlc"
    elif f["kind"] == "game+dlc":
        pass
    elif has_game:
        f["kind"] = "base"
    elif take(r"\bfix\b"):
        f["kind"] = "fix"
    else:
        f["kind"] = "base"; f["kind_inferred"] = True
    # 12. leftovers: filler words, then whatever is left is a free label (e.g. DLC name)
    take(r"\blink\b"); take(r"\bpre[\s-]?order\s+bonus\b|\bpre[\s-]?order\b")
    left = norm_space(re.sub(r"[()\[\]+,]|(?<=\s)-(?=\s)", " ", work))
    left = norm_space(re.sub(r"(?i)\b(?:by|the)\b$", "", left))
    if left:
        if f["kind"] in ("dlc", "game+dlc"):
            f["label"] = (f["label"] + " " + left).strip()  # a DLC's own name: parsed, shown as text
        else:
            f["residue"] = left
    return f


def fw_key(fw):
    """'12.xx' -> 12.99 (upper bound of the family), '7.61' -> 7.61, None -> None."""
    if not fw:
        return None
    a, b = fw.split(".")
    return int(a) + (0.99 if b == "xx" else int(b) / 100.0)


def effective_min_fw(f):
    """The lowest firmware the link is said to run on."""
    return f["backport_target"] or f["fw"]


# ----------------------------------------------------------------- multipart sets --
def release_key(f):
    """Everything that identifies a release except host and part."""
    return (f["region"], f["lang_to"], f["fw"], f["backport"], f["backport_target"], f["kind"], f["all_dlc"],
            f["format"], f["build"], f["version"], f["release"], f["packager"], f["label"], tuple(f["tags"]))


def group_parts(links):
    """links: [(index, name, url)] of ONE package, any order.
    Returns {'singles': [(index, fields)], 'sets': [set]} where a set is
    {'release': key, 'host': str, 'links': [index...] in download order, 'numbers': [...],
     'status': 'complete'|'restarts'|'gaps'|'unnumbered', 'runs': [[n...], ...]}."""
    parsed = [(i, n, u, parse_link(n, u)) for i, n, u in sorted(links)]
    name_count = collections.Counter(n for _, n, _, _ in parsed)
    singles, sets = [], collections.OrderedDict()
    for i, n, u, f in parsed:
        is_part = f["part"] is not None or f["part_unknown"] or (f["truncated"] and name_count[n] > 1)
        if not is_part:
            singles.append((i, f)); continue
        key = (release_key(f), f["host"])
        s = sets.setdefault(key, {"release": key[0], "host": key[1], "links": [], "raw": [], "trusted": True})
        s["links"].append(i)
        s["raw"].append(f["part"])
        if f["truncated"] or f["part"] is None:
            s["trusted"] = False        # a 63-byte cut can turn 'part 12' into 'part 1'
    out = []
    for s in sets.values():
        if s["trusted"]:
            nums = s["raw"]
        else:
            nums = list(range(1, len(s["links"]) + 1))   # position in index order
        runs, cur = [], []
        for n in nums:
            if cur and n <= cur[-1]:
                runs.append(cur); cur = []
            cur.append(n)
        runs.append(cur)
        gaps = any(r != list(range(r[0], r[0] + len(r))) or r[0] != 1 for r in runs)
        status = ("unnumbered" if not s["trusted"] else "gaps" if gaps else
                  "restarts" if len(runs) > 1 else "complete")
        # download order = catalogue index order (parts are listed ascending inside a run)
        out.append({"release": s["release"], "host": s["host"], "links": s["links"],
                    "numbers": nums, "runs": runs, "status": status})
    return {"singles": singles, "sets": out}
