#!/usr/bin/env python3
"""Build popularity.v1.json. Python 3 stdlib only.

  WINGS_CONTACT=you@example.com python build_index.py --catalogue catalogue-titles.json
Secrets (IGDB only) are read from TWITCH_CLIENT_ID / TWITCH_CLIENT_SECRET in the environment."""
import argparse

from popularity.build import DEFAULT_PROVIDERS, run


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalogue", required=True, help="library.json or catalogue-titles.json (titleId + title)")
    ap.add_argument("--out", default="out/popularity.v1.json")
    ap.add_argument("--report", default="out/report.md")
    ap.add_argument("--overrides", default="overrides.json")
    ap.add_argument("--providers", default=",".join(DEFAULT_PROVIDERS),
                    help="comma list of wikipedia,steam,igdb (igdb is opt-in and needs env secrets)")
    ap.add_argument("--cache-dir", default=".cache/http")
    ap.add_argument("--cache-ttl-days", type=float, default=6)
    ap.add_argument("--limit", type=int, default=None, help="only the first N titles (smoke tests)")
    ap.add_argument("--max-bytes", type=int, default=400000)
    ap.add_argument("--now", default=None, help="YYYY-MM-DD, fixes generatedAt/fetchedAt for reproducible output")
    run(ap.parse_args(argv))


if __name__ == "__main__":
    main()
