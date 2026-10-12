# Troubleshooting and FAQ

## First, the diagnostics

Open `/data/wings/diagnostics.txt` in Pegasus' file manager (web UI, Files) or in Wings' Files tab. It is a plain-text report with no tokens or links, safe to share. The title log `/mnt/sandbox/PPSA99178_000/download0/hui/dev/app.log` has `[WINGS-ART]` lines (covers and popularity) and `[WINGS-FILES]` lines (Files tab); it can contain Pegasus log lines, so read it before posting.

## "Waiting for Pegasus DL"

Pegasus is not running or not answering on `127.0.0.1:6970`. Start it first; Wings keeps trying. After ten seconds Wings may offer to load it (see [Pegasus auto-load](pegasus-autoload.md)); it never does anything while Pegasus answers.

- "No payload loader found on port 9021. Load Pegasus DL with your loader.": nothing on the console is listening for payloads on 9021. Wings does not retry; send `pegasus_dl.elf` with your usual loader (it is saved under `/data/wings/pegasus/`).
- "Needs the internet once to fetch Pegasus DL": nothing is saved yet and the network is off. Turn on cover downloads in Settings > Wings (it is the same network switch), or load Pegasus DL yourself.
- "The file did not match its published SHA-256": the saved file was removed; choose Load once again to fetch a fresh copy.
- The choices are gone: you picked "I will load it myself". Change it in Settings > Pegasus DL > Pegasus DL start-up.

## The Wings tile does not appear

Wait ten seconds after copying. Check the file size and SHA-256. Check that kstuff and ShadowMountPlus are loaded. See [Install](../../README.md#install).

## No covers

- Settings, **Wings**, **Download store covers** must be On; **Cover art status** says why not.
- The console clock must be right: HTTPS fails on a wrong date ("certificate expired / not yet valid").
- A network that blocks `1.1.1.1` and `8.8.8.8` on port 443 leaves covers off.
- Roughly one in a few hundred may stay a placeholder (AVIF and animated images are not supported). **Retry failed covers** asks again at once.

## No popularity scores

Popularity needs cover art on. Settings shows the last result, "Last updated" and the number of titles. Titles the index does not know show no score.

## Extraction failed / Pegasus removed the files

**Some failed installs have been seen to end with Pegasus removing the downloaded files. That is Pegasus's behaviour, not Wings', and Wings cannot stop it.** Wings only asks Pegasus to do the work and shows what Pegasus reports. If the files are gone, check the Files tab first; if they are, download again after fixing the cause. Wings adds a plain line under such failures in Downloads. Common causes to check:

- A wrong or missing **password**: see [Archive passwords](archive-passwords.md).
- An archive with **several packages**: the job waits for your choice ([the waiting-choice dialog](archive-passwords.md#choose-what-to-keep-the-waiting-choice-dialog)). Answer it rather than leaving it.
- An **incomplete multi-part set**: a card warns when numbers are missing. Get the missing parts first.
- A **package for a newer firmware** than your console: check the card's firmware chip and your Link ranking firmware.
- **Not enough free space** on the target drive.

Read the job's message in Downloads and Pegasus' own log (Logs tab) for the actual reason.

## The game is not recognised after the download finishes

ShadowMount registers games only when it scans, and it will not scan while Wings is open. Wings asks it to scan when a download ends and shows a banner on the Downloads tab; close Wings (the banner's **Close Wings now**) and ShadowMount finishes. See [After a download: ShadowMount scan](after-install-scan.md).

## A move or copy seems stuck

Files, Options, select the operation, Square cancels it. A large cross-drive move may leave a partial file at the destination.

## Wings looks wrong or crashes

Send `diagnostics.txt` and the `[WINGS-ART]` lines of `app.log` (and `app.prev.log` after a crash).
## FAQ

**Does Wings download games itself?** No. Pegasus does.

**Does Wings include game links?** No. Add your own sources in Pegasus.

**Can I use Wings without Pegasus?** No; it shows "Waiting for Pegasus DL".

**Which Pegasus versions work?** Tested against Pegasus 1.11.0. Other 1.x are tried and their features probed; other major versions are refused.

**Can Wings manage or update Pegasus?** Wings shows the running and the latest official version (Settings > Pegasus DL) and, only when you confirm, downloads the author's official release and, when you choose Load once or Auto-Load on the waiting screen, sends it to the payload loader on this console. It never does that while Pegasus answers and does not redistribute it.
