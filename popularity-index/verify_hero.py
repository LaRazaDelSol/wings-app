#!/usr/bin/env python3
"""MANUAL, optional: HEAD-check hero URLs in a built index (not run in CI).

  python verify_hero.py out/popularity.v1.json [--fix] [--sample 25]

Checks `hero` and `heroLow`. Steam `library_hero.jpg` does not exist for every app; with --fix a 404 is
replaced by the same app's `header.jpg` if that exists, otherwise `hero` is dropped (and `heroLow` with it,
since it only ever accompanies a hero). A dead `heroLow` alone is just dropped. Without --fix it only reports.
Image shape is enforced by the builder (ratio 1.5-3.2); this script only checks that the URLs answer.
One request per URL, 1 req/s, honest User-Agent from WINGS_CONTACT. Python 3 stdlib only."""
import json
import os
import sys
import time
import urllib.error
import urllib.request


def head_ok(url, opener=urllib.request.urlopen, ua="Wings-popularity/0.1"):
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": ua})
    try:
        with opener(req, timeout=15) as r:
            return 200 <= r.status < 300
    except urllib.error.HTTPError as e:
        if e.code in (404, 403, 410):
            return False
        raise


def verify(doc, check, fix=False, sample=None, sleep=lambda s: None):
    """Returns (checked, bad ids). With fix, mutates doc. check(url)->bool."""
    ids = [t for t in sorted(doc["titles"]) if "hero" in doc["titles"][t]]
    ids = ids[:sample] if sample else ids
    bad = []
    for tid in ids:
        rec = doc["titles"][tid]
        sleep(1.0)
        hero_ok = check(rec["hero"])
        low_ok = True
        if "heroLow" in rec:
            sleep(1.0)
            low_ok = check(rec["heroLow"])
        if hero_ok and low_ok:
            continue
        bad.append(tid)
        if not fix:
            continue
        if not low_ok:
            del rec["heroLow"]
        if not hero_ok:
            alt = rec["hero"].replace("library_hero.jpg", "header.jpg")
            if alt != rec["hero"] and check(alt):
                rec["hero"] = alt
            else:
                del rec["hero"]
                rec.pop("heroLow", None)
    return len(ids), bad


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("index")
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--sample", type=int, default=None)
    a = ap.parse_args(argv[1:])
    contact = os.environ.get("WINGS_CONTACT", "").strip()
    if not contact:
        print("WINGS_CONTACT is required")
        return 2
    ua = "Wings-popularity/0.1 (contact: %s)" % contact
    with open(a.index, "r", encoding="utf-8") as f:
        doc = json.load(f)
    n, bad = verify(doc, lambda u: head_ok(u, ua=ua), fix=a.fix, sample=a.sample, sleep=time.sleep)
    print("checked %d, missing %d %s" % (n, len(bad), bad[:20]))
    if a.fix and bad:
        with open(a.index, "w", encoding="utf-8") as f:
            f.write(json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False))
        print("rewrote %s (regenerate the .sha256 if you publish it)" % a.index)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
