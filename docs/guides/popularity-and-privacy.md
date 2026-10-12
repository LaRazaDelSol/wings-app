# Popularity data and privacy

## What it is

An optional score (0-100) and rating, shown on tiles and on a game's page, plus the sorts **Most popular** and **Highest rated**. It comes from one small JSON file built weekly by a separate public repository, so no API key and no scraping ever touches your console. The file is keyed by PlayStation title ID only. Low-confidence matches (below 0.6) are hidden.

Sources, each credited in [CREDITS.md](../../CREDITS.md):

- **IGDB.com**: "Ratings and popularity data from IGDB.com".
- **Steam**: percent positive and review count only.
- **Wikimedia**: Wikipedia pageviews.

There is also a local counter of what *you* queued from Wings (the "my downloads" sort). It stays on the console.

## What the console connects to

| Feature | Connection | What the other side learns |
|---|---|---|
| Pegasus DL | `127.0.0.1:6970`, on the console | Nothing leaves the console. |
| Cover art (on by default) | DNS-over-HTTPS to Cloudflare `1.1.1.1` and Google `8.8.8.8`; then HTTPS to the cover servers in your catalogue | Cloudflare and Google see the host names of the cover servers. Cover servers see your IP address and the image you asked for. |
| Popularity (on by default) | HTTPS GET of one file from `larazadelsol.github.io` (GitHub Pages), at most once per 24 hours and once after Wings is updated | GitHub sees your IP address. Wings sends no identifier beyond the User-Agent `WingsTV/0.1`. The file holds no user data. |
| Update check (on by default) | HTTPS GET of the latest release of this project from `api.github.com`, when Wings starts and then at most every 6 hours | GitHub sees your IP address and the same User-Agent. Nothing about you or your games. See [Updating Wings](updating.md). |

Popularity and the update check use the same HTTPS client as cover art and, as Settings says, need cover art (DNS-over-HTTPS) on. With cover art off, neither fetches and Wings makes no internet connection of its own.

## Turning it off

- Settings, **Wings** group, **Download store covers (DNS-over-HTTPS)**: Off. Covers already downloaded still show.
- Settings, **Wings** group, **Popularity data**: Off.
- Settings, **Wings** group, **Updates** > **Check for Wings updates**: Off.
- Settings, **Wings** group, **Art sources**: where art comes from. **Cover icons from** is *Catalogue* by default (the covers your catalogue links to); *Prosperopatches* uses each title's PS5 icon from `cdn.prosperopatches.com` instead, for titles the popularity data lists there (the rest keep their catalogue cover). **16:9 game art from** is *All sources* by default (Prosperopatches first, then IGDB, then Steam); *Prosperopatches only* uses just the PS5's own background and nothing from IGDB or Steam. Both need Popularity data on.
- **Popularity source URL** can point at another `https://` file, or stay Default.
- **Clear art cache** deletes downloaded covers.

## What Wings keeps on the console

`/download0/wings/` (cover cache up to 96 MiB, settings, diagnostics) and `/data/wings/` (diagnostics and a copy of the settings). The diagnostics report contains no tokens or links.

Debrid tokens you enter are sent to Pegasus DL and shown as dots; Wings does not log them.

Pegasus DL has its own network behaviour (sources, debrid, downloads). That is Pegasus, not Wings, and is outside this page.
