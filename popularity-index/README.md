# wings-popularity

Builds `popularity.v1.json`, a small static file of popularity and rating numbers keyed by PlayStation titleId
(PPSA = PS5, CUSA = PS4), for the Wings homebrew browser. The app never calls the data providers; it only downloads
this one file (opt-in, cached). Python 3 standard library only. Scripts are MIT licensed (see `LICENSE`).

## Output

```
{"schemaVersion":1,"generatedAt":"...Z","sources":["steam","wikipedia"],
 "attribution":{"steam":"...","wikipedia":"..."},
 "titles":{"PPSA01234":{"score":72,"rating":88,"ratingCount":1234,"sources":["steam","wikipedia"],
                        "confidence":0.93,
                        "hero":"https://cdn.prosperopatches.com/titles/PPSA01234_<64 hex>/pic0.webp",
                        "heroLow":"https://images.igdb.com/igdb/image/upload/t_screenshot_big/<id>.jpg"}}}
```

`score` is 0-100 (sort key for "Most popular"), `rating` 0-100 and `ratingCount` are absent when no rating source
matched, `confidence` is 0-1 (the app should hide records below 0.6). Keys are sorted, output is compact JSON and
deterministic for a given input, cache and `--now`. There is no per-title timestamp; `generatedAt` is the only date.
Blend weights and missing-source handling are documented at the top of `popularity/blend.py`.

### Optional `hero` and `heroLow` fields (game artwork URLs)

Both are optional, additive https URLs (`schemaVersion` stays 1, older readers ignore them). The app fetches
them lazily when a game page opens.

* `hero`: the best full-quality image, ideally true 16:9. Chosen in this order: (1) `pic0` (below, 1920x1080),
  (2) the IGDB screenshot closest to 16:9 at `t_1080p`, (3) the IGDB artwork closest to 16:9, (4) Steam
  `library_hero.jpg` (always 1920x620, logo-free; the app fits it to the width).
* `heroLow`: a small (about 60 KB), roughly 16:9 image for a fast first paint, the IGDB screenshot at
  `t_screenshot_big` (889x500). Omitted when there is none, or it equals `hero`, or there is no `hero`.
* IGDB images are picked by real shape: the query returns width and height, only images at least 1280 px wide
  and within 15% of 16:9 qualify, ties go to the wider one. Banners and odd shapes (for example 1920x399) are
  never used. No image with a width/height ratio outside 1.5-3.2 is ever emitted.
* Art is attached only when the match confidence is >= 0.85 (`matcher.ACCEPT`) and not ambiguous. URLs are https,
  at most 300 characters, with no spaces or `@` in the host. Only public ids go into URLs; nothing is stored.
* The builder does not check that Steam or IGDB files exist (`library_hero.jpg` is missing for some apps; the app
  should hide the image on a 404). Optional manual check, never in CI (checks `hero` and `heroLow`):

  ```
  set WINGS_CONTACT=you@example.com
  python verify_hero.py out/popularity.v1.json --sample 25          # HEAD requests, 1 req/s, report only
  python verify_hero.py out/popularity.v1.json --fix                # 404 -> header.jpg, else drop the field
  ```
* Terms caveat: IGDB images carry the same attribution requirement as IGDB data and the same
  republishing question (confirm with partner@igdb.com). The Steam store art terms for linking or reusing
  `steamstatic.com` assets are NOT verified here; the index only links, it does not copy, and the Wings app must
  fetch the image itself. Drop `hero` (one line in `popularity/blend.py`) if that is a problem.

#### The `pic0` source

`pic0` (provider `popularity/providers/pic0.py`) finds the 1920x1080 PS5 background of a PPSA title on
Prosperopatches. It supplies art only (never listed in `sources`, never affects `score`, never creates a record
by itself). For each PPSA id it reads `https://prosperopatches.com/<PPSAxxxxx>`, takes the `twitter:image` meta
tag and accepts it only if it is exactly `https://cdn.prosperopatches.com/titles/<same id>_<64 hex>/icon0.webp`
(unknown ids return a generic site banner, which is rejected). It then probes `.../pic0.webp` with a 64-byte
Range request, parses the WebP header (VP8, VP8L or VP8X) and requires width >= 1280 and ratio 1.5-2.0.
CUSA ids are skipped. Any failure just means no `pic0` for that title; it can never fail the run.

Politeness: both hosts share one budget of 1 request per second, with the honest User-Agent from `WINGS_CONTACT`.
Results (including misses) are cached in `.cache/http` (page lookups 30 days, image headers 180 days; only the
small extracted URL and 64 bytes are stored), so a repeat run only fetches new titles. The first run over the
whole catalogue takes roughly 45 minutes.

## Run locally

```
set WINGS_CONTACT=you@example.com        (PowerShell: $env:WINGS_CONTACT="you@example.com")
python extract_titles.py path\to\library.json catalogue-titles.json
python build_index.py --catalogue catalogue-titles.json --out out/popularity.v1.json --report out/report.md
python -m unittest discover -s tests     (pytest also works)
```

* `--catalogue` accepts `library.json` directly (path argument; it is never copied into this repo),
  `catalogue-titles.json` or any `titles.json` of `[{"titleId","title"}]` (also `{"titleId": "title"}`). `--limit 20` is handy for a smoke test.
* `WINGS_CONTACT` is required for network use. It goes into the User-Agent
  (`Wings-popularity/0.1 (contact: ...)`), as Wikimedia requires.
* Politeness: at most 3 concurrent workers, Wikimedia 4 req/s, Steam 1 req/s per host, IGDB 4 req/s, Prosperopatches (page and image hosts together) 1 req/s, retries honour
  `Retry-After`. Responses are cached in `.cache/http` (6 days) so reruns are cheap.
* `out/report.md` lists coverage per source, low-confidence matches and unmatched titles.

### Catalogue input for CI

The catalogue (`library.json`) is third-party data and is never committed here, and neither is the title list.
CI gets the list of `{"titleId","title"}` pairs from the repository secret `CATALOGUE_TITLES_GZ` (gzip + base64,
masked in logs, written to a temporary file at run time). To create or refresh it:

```
python extract_titles.py path/to/library.json catalogue-titles.json   # git-ignored
python pack_titles.py catalogue-titles.json                           # prints the secret value
```

Paste the printed line into Settings > Secrets and variables > Actions > New repository secret. Titles are cleaned of
`[exFAT]`, versions and `@packager` tags, and non-game ids are dropped. The published `popularity.v1.json` is keyed by
title ID only (public PlayStation identifiers): no names, no links.

### Matching and fixes

Titles are normalised (accents, trademark symbols, roman numerals, `&`, leading "the", format and edition words), then
candidates are ranked by token Jaccard plus Jaro-Winkler, with a platform filter (Steam: Windows build; IGDB: PS5/PS4),
DLC/bundle/soundtrack rejection and optional year disambiguation. If two different candidates score within 0.04 the
result is flagged ambiguous with confidence capped at 0.5 instead of guessing. Below 0.6 there is no match.
Fix mistakes in `overrides.json`:

```
{"PPSA01234": {"wikipedia": "Hades II", "steam": 1145350, "igdb": 12345},
 "PPSA09999": "ignore"}
```

A forced id gets confidence 1.0. `"ignore"` skips every provider for that title.

## Providers

| provider | data | default | notes |
|---|---|---|---|
| wikipedia | 30-day mean daily pageviews (CC0) | on | article chosen by search, needs a video-game article |
| steam | percent positive and review count | on | official `IUserReviewsService/GetAppReviews`; name search uses the undocumented `storesearch` |
| igdb | `total_rating`, rating count, 16:9 screenshots/artwork | off | needs `--providers wikipedia,steam,igdb` and the two secrets below |
| pic0 | 1920x1080 background art only | on in CI | Prosperopatches, PPSA ids only; add to `--providers` locally |

## GitHub setup

Workflow: `.github/workflows/popularity.yml` (weekly, Monday 04:17 UTC, plus manual run). Enable Pages with source
"GitHub Actions". The job commits `heartbeat.txt` each run so the 60-day inactivity rule does not disable the schedule.
Actions are pinned by commit SHA; permissions are minimal and per job.

* Repository variable `WINGS_CONTACT`: contact string for the User-Agent.
* Repository variable `WINGS_PROVIDERS` (optional): when set it REPLACES the CI default (`wikipedia,steam,pic0`), so list every
  provider you want, for example `wikipedia,steam,pic0,igdb` to turn IGDB on. A list without `pic0` produces no pic0 art.
* Repository secrets `TWITCH_CLIENT_ID` and `TWITCH_CLIENT_SECRET` (only for IGDB; Twitch 2FA account, confidential
  app). Secrets are read from the environment only, never from arguments, never printed, never cached, and one token is
  minted per run.

## Attribution

* Wikipedia pageviews: Wikimedia Foundation analytics data, CC0.
* Steam: review summaries from Steam (Valve). Only the percent positive and count are published. Not affiliated with Valve.
* IGDB (if enabled): "Ratings and popularity data from IGDB.com". Free non-commercial use under the Twitch Developer
  Services Agreement; confirm republishing with partner@igdb.com before enabling it publicly.
* pic0 (if enabled): artwork links from Prosperopatches (prosperopatches.com); the image files stay on their host.
* RAWG is deliberately not used (its terms forbid redistribution).

## Privacy

The file contains no user data. The Wings app downloads it with a plain GET and sends no identifiers beyond a User-Agent;
the feature is on by default in the app and can be turned off in Settings. This repo's CI sends only the contact string you configure to the providers.
