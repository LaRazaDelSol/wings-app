# wings-popularity

Builds `popularity.v1.json`, a small static file of popularity and rating numbers keyed by PlayStation titleId
(PPSA = PS5, CUSA = PS4), for the Wings homebrew browser. The app never calls the data providers; it only downloads
this one file (opt-in, cached). Python 3 standard library only. Scripts are MIT licensed (see `LICENSE`).

## Output

```
{"schemaVersion":1,"generatedAt":"...Z","sources":["steam","wikipedia"],
 "attribution":{"steam":"...","wikipedia":"..."},
 "titles":{"PPSA01234":{"score":72,"rating":88,"ratingCount":1234,"sources":["steam","wikipedia"],
                        "confidence":0.93,"fetchedAt":"2026-10-11",
                        "hero":"https://cdn.cloudflare.steamstatic.com/steam/apps/1145350/library_hero.jpg"}}}
```

`score` is 0-100 (sort key for "Most popular"), `rating` 0-100 and `ratingCount` are absent when no rating source
matched, `confidence` is 0-1 (the app should hide records below 0.6). Keys are sorted, output is compact JSON and
deterministic for a given input, cache and `--now`. About 150 bytes per record, so well under 200 KB for 1,318 titles.
Blend weights and missing-source handling are documented at the top of `popularity/blend.py`.

### Optional `hero` field (game artwork URL)

`hero` is an optional, additive https URL of 16:9-ish artwork that the console may fetch lazily when a game page
opens. `schemaVersion` stays 1 and older readers ignore it. Rules:

* Source order: Steam `https://cdn.cloudflare.steamstatic.com/steam/apps/<appid>/library_hero.jpg` for the matched
  appid; else, only when the IGDB provider ran, the first IGDB artwork (then screenshot) as
  `https://images.igdb.com/igdb/image/upload/t_1080p/<image_id>.jpg`.
* Emitted only when that provider's match confidence is >= 0.85 (`matcher.ACCEPT`) and not ambiguous; otherwise
  the field is absent. Only public ids go into the URL; nothing is downloaded, hashed or stored.
* Size: about 84 bytes per Steam record and 70 per IGDB record, so a full 1,318-title index grows by at most
  roughly 110 KB (about 230 bytes per record in total, still under `--max-bytes 400000`).
* The builder does not check that the file exists (`library_hero.jpg` is missing for some apps, and the app should
  hide the image on a 404 or fall back to `.../header.jpg` in the same folder). Optional manual check, never in CI:

  ```
  set WINGS_CONTACT=you@example.com
  python verify_hero.py out/popularity.v1.json --sample 25          # HEAD requests, 1 req/s, report only
  python verify_hero.py out/popularity.v1.json --fix                # 404 -> header.jpg, else drop the field
  ```
* Terms caveat: IGDB images carry the same attribution requirement as IGDB data and the same
  republishing question (confirm with partner@igdb.com). The Steam store art terms for linking or reusing
  `steamstatic.com` assets are NOT verified here; the index only links, it does not copy, and the Wings app must
  fetch the image itself. Treat this as unconfirmed until Valve's terms are checked, and drop `hero` (one line in
  `popularity/blend.py`) if that is a problem.

## Run locally

```
set WINGS_CONTACT=you@example.com        (PowerShell: $env:WINGS_CONTACT="you@example.com")
python extract_titles.py path\to\library.json catalogue-titles.json
python build_index.py --catalogue catalogue-titles.json --out out/popularity.v1.json --report out/report.md
python -m unittest discover -s tests     (pytest also works)
```

* `--catalogue` accepts `library.json` directly (path argument; it is never copied into this repo),
  `catalogue-titles.json` or any `titles.json` of `[{"titleId","title"}]` (also `{"titleId": "title"}`); see
  `docs/CUSTOM-CATALOGUE.md` for running it on your own catalogue. `--limit 20` is handy for a smoke test.
* `WINGS_CONTACT` is required for network use. It goes into the User-Agent
  (`Wings-popularity/0.1 (contact: ...)`), as Wikimedia requires.
* Politeness: at most 3 concurrent workers, Wikimedia 4 req/s, Steam 1 req/s per host, IGDB 4 req/s, retries honour
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
| igdb | `total_rating`, rating count | off | needs `--providers wikipedia,steam,igdb` and the two secrets below |

## GitHub setup

Workflow: `.github/workflows/popularity.yml` (weekly, Monday 04:17 UTC, plus manual run). Enable Pages with source
"GitHub Actions". The job commits `heartbeat.txt` each run so the 60-day inactivity rule does not disable the schedule.
Actions are pinned by commit SHA; permissions are minimal and per job.

* Repository variable `WINGS_CONTACT`: contact string for the User-Agent.
* Repository variable `WINGS_PROVIDERS` (optional): `wikipedia,steam,igdb` to turn IGDB on.
* Repository secrets `TWITCH_CLIENT_ID` and `TWITCH_CLIENT_SECRET` (only for IGDB; Twitch 2FA account, confidential
  app). Secrets are read from the environment only, never from arguments, never printed, never cached, and one token is
  minted per run.

## Attribution

* Wikipedia pageviews: Wikimedia Foundation analytics data, CC0.
* Steam: review summaries from Steam (Valve). Only the percent positive and count are published. Not affiliated with Valve.
* IGDB (if enabled): "Ratings and popularity data from IGDB.com". Free non-commercial use under the Twitch Developer
  Services Agreement; confirm republishing with partner@igdb.com before enabling it publicly.
* RAWG is deliberately not used (its terms forbid redistribution).

## Privacy

The file contains no user data. The Wings app downloads it with a plain GET and sends no identifiers beyond a User-Agent;
the feature is off by default in the app. This repo's CI sends only the contact string you configure to the providers.
