#!/usr/bin/env python3
"""Write catalogue-titles.json (titleId + cleaned title ONLY) from a local library.json.

library.json is third-party data and is not stored in this repo. Run this locally, review the
output, and commit only catalogue-titles.json if you decide that is acceptable:

  python extract_titles.py path/to/library.json catalogue-titles.json
"""
import json
import sys

from popularity.normalize import clean_title


def extract(src):
    with open(src, "r", encoding="utf-8") as f:
        data = json.load(f)
    pkgs = data["packages"] if isinstance(data, dict) else data
    seen = {}
    for p in pkgs:
        tid = p.get("titleId", "")
        if tid[:4] in ("PPSA", "CUSA") and p.get("title") and tid not in seen:
            seen[tid] = clean_title(p["title"])
    return [{"titleId": k, "title": seen[k]} for k in sorted(seen)]


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    rows = extract(argv[1])
    with open(argv[2], "w", encoding="utf-8", newline="\n") as f:
        json.dump(rows, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")
    print("wrote %d titles" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
