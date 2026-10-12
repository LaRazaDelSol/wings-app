# Popularity for your own catalogue

Wings can show popularity and ratings for titles in a catalogue it does not ship with. The app never talks to the data
providers; you build `popularity.v1.json` yourself on a PC, host it anywhere that serves a static file over https, and
point Wings at the URL.

## 1. Get a title list

You need a JSON file of title IDs and names, exactly:

```
[{"titleId":"PPSA01234","title":"Hades II"},{"titleId":"CUSA00001","title":"Elden Ring"}]
```

* A `titles.json` written by the Wings in-app title export (planned) has this shape. `build_index.py` already reads
  it unchanged, as it does `catalogue-titles.json`, a `{"titleId": "title"}` object, or a raw `library.json`.
* Only IDs starting `PPSA` (PS5) or `CUSA` (PS4) are used; duplicates keep the first name; anything else is skipped.
* Names are used for matching only and never end up in the output. From a `library.json`, you can clean names first:
  `python extract_titles.py library.json titles.json`.

## 2. Build the index

Requires Python 3 (standard library only). From a clone of this repo:

```
set WINGS_CONTACT=you@example.com
python build_index.py --catalogue titles.json --out out/popularity.v1.json --report out/report.md
```

PowerShell: `$env:WINGS_CONTACT="you@example.com"`. The contact string only goes into the User-Agent that Wikipedia
requires. Start with `--limit 20` to check it works. A full run takes a while because requests are rate limited
(Steam 1 req/s); reruns reuse the cache in `.cache/http`. Look at `out/report.md` for unmatched or low-confidence
titles and correct them in `overrides.json` (see README).

Optional: `--providers wikipedia,steam,igdb` adds IGDB (needs `TWITCH_CLIENT_ID` and `TWITCH_CLIENT_SECRET` in the
environment, see README for the terms).

## 3. Host it

Any static host that serves the file over https with a normal GET works: GitHub Pages, a gist raw URL, an S3 or
object-storage bucket, your own web server. Upload `out/popularity.v1.json` (the `.sha256` file is optional). The file
holds only public title IDs, numbers and artwork links, no names and nothing about you.

## 4. Point Wings at it

On the console: Settings > Popularity URL, enter the https URL, then enable popularity. Wings downloads the file with
a plain GET and caches it. To refresh, rebuild and re-upload; keep the URL stable.

## Notes

* Rebuild whenever the catalogue changes. Old records stay valid; missing titles just have no badge.
* Records with confidence below 0.6 are hidden by the app. The optional `hero` artwork field is only present for
  confident matches and its image terms are discussed in the README.
