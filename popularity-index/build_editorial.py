#!/usr/bin/env python3
"""Publish notes.v1.json and curated.v1.json next to popularity.v1.json. Python 3 stdlib only.

  python build_editorial.py --notes-env NOTES_JSON --curated-env CURATED_JSON --out site
Each env var holds the JSON text or base64(gzip(JSON)) (same packing as pack_titles.py). Unset or empty means that file
is not written. Invalid content exits 1 with a message that never includes the content."""
import argparse
import os
import sys

from popularity import editorial


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--notes-env", default="NOTES_JSON", help="name of the env var holding the notes JSON")
    ap.add_argument("--curated-env", default="CURATED_JSON", help="name of the env var holding the curated JSON")
    ap.add_argument("--out", default="site", help="output directory")
    args = ap.parse_args(argv)
    try:
        files = editorial.build(os.environ.get(args.notes_env), os.environ.get(args.curated_env))
    except editorial.EditorialError as e:
        sys.stderr.write("::error::editorial input rejected: %s\n" % e)
        return 1
    names = editorial.write(files, args.out)
    print("editorial files written: %s" % (", ".join(names) if names else "none (secrets unset)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
