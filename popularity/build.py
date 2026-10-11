"""Orchestration: catalogue -> providers -> blend -> popularity.v1.json + report.md."""
import concurrent.futures
import datetime
import hashlib
import json
import os
import sys

from . import SCHEMA_VERSION, blend as blendmod, report as reportmod
from .models import Entry, ProviderError
from .net import CachedHttp, RateLimiter, UrlLibHttp
from .providers.igdb import Igdb
from .providers.steam import Steam
from .providers.wikipedia import Wikipedia

ATTRIBUTION = {
    "wikipedia": "Pageview data from Wikimedia (CC0).",
    "steam": "Review data from Steam (percent positive and count only).",
    "igdb": "Ratings and popularity data from IGDB.com",
}
INTERVALS = {"en.wikipedia.org": 0.25, "wikimedia.org": 0.25, "store.steampowered.com": 1.0,
             "api.steampowered.com": 1.0, "api.igdb.com": 0.25, "id.twitch.tv": 1.0}
MAX_WORKERS = 3
DEFAULT_PROVIDERS = ("wikipedia", "steam")  # igdb is opt-in


def load_catalogue(path, limit=None):
    """Accepts library.json ({"packages": [...]}), catalogue-titles.json ([{titleId,title}]) or
    {titleId: title}. One entry per titleId (first seen), sorted by titleId; non-game ids skipped."""
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict) and "packages" in data:
        data = data["packages"]
    if isinstance(data, dict) and "titles" in data:
        data = data["titles"]
    if isinstance(data, dict):
        data = [{"titleId": k, "title": v} for k, v in data.items()]
    seen = {}
    for p in data:
        tid, title = p.get("titleId"), p.get("title")
        if not tid or not title or not (tid.startswith("PPSA") or tid.startswith("CUSA")):
            continue
        seen.setdefault(tid, Entry(tid, title, p.get("year")))
    entries = [seen[k] for k in sorted(seen)]
    return entries[:limit] if limit else entries


def load_overrides(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    return {k: v for k, v in raw.items() if not k.startswith("_")}


def override_for(overrides, tid, source):
    o = overrides.get(tid)
    if o is None:
        return None
    if o == "ignore" or (isinstance(o, dict) and o.get("ignore") is True):
        return "ignore"
    v = o.get(source) if isinstance(o, dict) else None
    return v if v is not None else None


def make_providers(names, http, today, environ=None):
    table = {"wikipedia": lambda: Wikipedia(http, today), "steam": lambda: Steam(http),
             "igdb": lambda: Igdb(http, environ)}
    active, skipped = [], {}
    for n in names:
        if n not in table:
            raise SystemExit("unknown provider: %s" % n)
        p = table[n]()
        ok, why = p.available()
        if ok:
            active.append(p)
        else:
            skipped[n] = why
    return active, skipped


def collect(entries, providers, overrides, workers=MAX_WORKERS, progress=None):
    results = {}

    def one(e):
        res = {}
        for p in providers:
            ov = override_for(overrides, e.title_id, p.name)
            if ov == "ignore":
                res[p.name] = "ignored"
                continue
            try:
                res[p.name] = p.lookup(e, ov)
            except ProviderError as err:
                res[p.name] = "error"
                if progress:
                    progress("  %s %s: %s" % (p.name, e.title_id, err))
        return e.title_id, res

    with concurrent.futures.ThreadPoolExecutor(max_workers=min(workers, MAX_WORKERS)) as ex:
        for i, (tid, res) in enumerate(ex.map(one, entries), 1):
            results[tid] = res
            if progress and i % 50 == 0:
                progress("%d/%d titles" % (i, len(entries)))
    return results


def build_document(entries, results, providers_used, now):
    signals = {tid: [s for s in r.values() if hasattr(s, "confidence")] for tid, r in results.items()}
    day = now.strftime("%Y-%m-%d")
    records = blendmod.blend(signals, day)
    doc = {
        "schemaVersion": SCHEMA_VERSION,
        "generatedAt": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "sources": sorted(providers_used),
        "attribution": {k: ATTRIBUTION[k] for k in sorted(providers_used)},
        "titles": records,
    }
    return doc, records


def dump(doc):
    return json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def run(args, environ=None, http=None, log=lambda m: print(m, file=sys.stderr)):
    env = os.environ if environ is None else environ
    now = (datetime.datetime.strptime(args.now, "%Y-%m-%d").replace(tzinfo=datetime.timezone.utc)
           if args.now else datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0))
    names = [n.strip() for n in args.providers.split(",") if n.strip()]
    if http is None:
        contact = env.get("WINGS_CONTACT", "").strip()
        if not contact:
            raise SystemExit("WINGS_CONTACT is required (contact string for the User-Agent, e.g. an email)")
        ua = "Wings-popularity/0.1 (contact: %s) python-urllib/3" % contact
        http = UrlLibHttp(ua, RateLimiter(INTERVALS, default=1.0))
        if args.cache_dir:
            http = CachedHttp(http, args.cache_dir, ttl_seconds=args.cache_ttl_days * 86400)
    entries = load_catalogue(args.catalogue, args.limit)
    overrides = load_overrides(args.overrides)
    providers, skipped = make_providers(names, http, now.date(), env)
    for n, why in skipped.items():
        log("provider %s skipped: %s" % (n, why))
    used = [p.name for p in providers]
    results = collect(entries, providers, overrides, progress=log)
    doc, records = build_document(entries, results, used, now)
    text = dump(doc)
    size = len(text.encode("utf-8"))
    if size > args.max_bytes:
        raise SystemExit("output %d bytes exceeds --max-bytes %d" % (size, args.max_bytes))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    with open(args.out + ".sha256", "w", encoding="utf-8", newline="\n") as f:
        f.write("%s  %s\n" % (hashlib.sha256(text.encode("utf-8")).hexdigest(), os.path.basename(args.out)))
    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as f:
            f.write(reportmod.render(entries, results, records, used, doc["generatedAt"], skipped))
    log("wrote %s: %d of %d titles, %d bytes" % (args.out, len(records), len(entries), size))
    return doc, results
