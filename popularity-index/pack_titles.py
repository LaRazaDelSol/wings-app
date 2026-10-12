#!/usr/bin/env python3
"""Pack catalogue-titles.json into one line of base64(gzip) for the CATALOGUE_TITLES_GZ Actions secret.

  python extract_titles.py path/to/library.json catalogue-titles.json   # local only, never commit
  python pack_titles.py catalogue-titles.json                           # prints the secret value
GitHub secrets hold up to 48 KB; the packed list is far smaller. Re-run both when the catalogue changes.
"""
import base64
import gzip
import json
import sys


def pack(path):
    with open(path, "r", encoding="utf-8") as f:
        rows = json.load(f)
    raw = json.dumps(rows, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return base64.b64encode(gzip.compress(raw, 9, mtime=0)).decode("ascii"), len(rows)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    text, n = pack(sys.argv[1])
    if len(text) > 48000:
        sys.exit("packed list is %d bytes, over the 48 KB secret limit" % len(text))
    sys.stderr.write("%d titles, %d bytes packed\n" % (n, len(text)))
    print(text)
