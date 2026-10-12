# Getting started

Wings is a front end for Pegasus DL. It shows what Pegasus knows and sends your choices back to it. Install it first: see the [README](../../README.md#install).

## Before you open it

1. Start Pegasus DL on the console and wait until its web UI answers. Wings never starts Pegasus.
   If you have no loader set up to start it, Wings can offer to load it for you; see step 1 of First run.
2. Add your own sources in Pegasus (its web UI, or Wings' **Sources** tab). Wings ships none.

## The tabs

L1 and R1 (or a swipe on the touchpad) switch tabs.

| Tab | What it is |
|---|---|
| Store | Your catalogue, one tile per game. Search, sort, filter, favourites. |
| Downloads | Pegasus' queue: progress, speed, pause, resume, retry, passwords. |
| Library | Games installed on the console, with APR Emu status. |
| Files | Browse the console's file system, copy, move, extract, install. |
| Sources | The sources Pegasus uses. |
| Settings | Two groups: **Wings** (kept by Wings) and **Pegasus DL** (changes Pegasus). |
| Logs | Pegasus' log, plus Wings' own diagnostics view. |

## First run

1. A pill at the top right says "Connecting", then shows the Pegasus version. "Waiting for Pegasus DL" means Pegasus is not answering at `127.0.0.1:6970`; start it, Wings keeps trying. Three choices appear under the message straight away (with a spinner while Wings looks for it for the first ten seconds): **Enable Pegasus-dl.elf Auto-Load**, **Load Pegasus-dl.elf once**, and **I will load Pegasus-dl.elf myself**. The first two download the official file once (checked by SHA-256) and send it to the payload loader on this console. You can change the choice any time in Settings > Pegasus DL > Pegasus DL start-up. Details: [Pegasus auto-load](pegasus-autoload.md).
2. Store shows a skeleton grid, then tiles. Tiles show their title until the cover downloads.
3. Open Settings, **Wings** group, and set once:
   - **Link ranking**: preferred firmware, region and format, and whether to prefer debrid. This is what makes the **Best** card fit your console. See [Link types](link-types.md).
   - **Cover art** and **Popularity data**: both are on by default; both fetch from the internet. See [Popularity data and privacy](popularity-and-privacy.md).
   - **Tile details**: Always, On focus (default) or Never.
4. Pick a game: [Finding and downloading a game](finding-and-downloading.md).

## What Wings changes on the console

Only its own folders: `/download0/wings/` and `/data/wings/`. Everything else is done by Pegasus at your request (downloads, installs, extractions, deleting a game, changing the download folder).
