# First console test of Wings (PPSA99178)

Status: sections 1 to 11 were written for the cover-art build (run). Section 12 "Second test" covers the later build in `wings/build-out/` (sha256 in `package-verification.json`). Nothing in this file has been run on a console yet; it is the plan for the first run and the form to report it. Section 13 "Third test" covers the build with archive-password auto-try, Download all parts, the prompt keys and the Files polish.

Goal of this run: find out whether the title starts, whether it talks to your Pegasus DL, and whether HTTPS store covers work from inside a native title (TLS, the name lookup, the clock, the random source, the cache folder). You do not need to understand the code: the app writes a plain text report and you read it.

## 0. What this run does and does not do

- Wings reads from Pegasus DL (health, queue, library, sources, installed games). Do not press actions that change Pegasus during this test (cancel, clear finished, add or delete a source, direct link, download). Looking is enough.
- Wings never starts Pegasus. Start Pegasus DL yourself first, as you normally do.
- DNS-over-HTTPS (cover art) and popularity data are on by default on a fresh install (existing choices are kept). Turn them off in Settings and Wings makes no internet connection at all. With them on, Wings sends the host names of the cover servers (for example `image.api.playstation.com`) to Cloudflare (1.1.1.1) and Google (8.8.8.8), then downloads the cover images over HTTPS.
- Wings writes only to its own folders: `/download0/wings/` (title storage: cover cache, settings, diagnostics) and `/data/wings/` (diagnostics and a copy of the settings, so you can read them in the file manager).
- Orbit (`PPSA99177`) is a different title and is not touched.

## 1. Prerequisites

- The same setup Orbit's guide asks for: a jailbreak that loads **kstuff** (fake-signed apps) and **ShadowMountPlus** (mounts the package and adds its tile).
- Pegasus DL running on the console (your instance answers at `10.0.0.113:6970` from the LAN; on the console Wings uses `127.0.0.1:6970`).
- The console clock set correctly (HTTPS certificates are checked against it). Settings > Date and Time on the PS5.
- Internet access on the console for the cover test (steps 6 to 9).

## 2. Files and checksum

From `wings\build-out\` on the PC:

| File | Size (bytes) | SHA-256 |
|---|---|---|
| `PPSA99178.ffpkg` | 52,690,944 | `8d9cf3fb2e666fe9a2cb43eca31447b4fba3298f6c74940d06513d948f65d469` |

Check it on Windows before copying (PowerShell):

```
Get-FileHash .\PPSA99178.ffpkg -Algorithm SHA256
```

or `certutil -hashfile PPSA99178.ffpkg SHA256`. The value must equal the one above (case does not matter). If it does not, the copy is damaged or a different build: stop and rebuild or recopy. The same hash is in `wings\build-out\package-verification.json` (`imageSha256`). The folder `wings\build-out\PPSA99178\` holds the same files unpacked, and `PPSA99178.zip` is that folder zipped; use either the image or the folder, not both.

## 3. Install (like Orbit's guide)

1. Copy **one** of these to the console (FTP or your usual file transfer):
   - `PPSA99178.ffpkg` into `/data/homebrew/`, or
   - the folder `PPSA99178/` to `/data/homebrew/PPSA99178/`.
2. Wait about ten seconds after the copy finishes. ShadowMountPlus registers it and a **Wings** tile appears in the Games row (blue icon with a white W).
3. Check the size on the console side if you can (FTP listing): the image must be 52,690,944 bytes. A shorter file means the copy was interrupted.
4. If a previous Wings build (the earlier skeleton) is installed under the same title id, replace the file and also remove the staged copies ShadowMountPlus made (see Orbit's note: it stages `param.json` and the icon once), then reboot the jailbreak chain, so the new package is mounted rather than the old one.

Title id `PPSA99178`, content version `00.001.000`.

## 4. First launch (cover art off)

1. Start Pegasus DL first, wait until its web UI answers.
2. Open **Wings** from the Games row.
3. What to expect, in order:
   - A short splash, then the Wings shell (tabs: Store, Downloads, Library, Files, Sources, Settings, Logs). A pill at the top right says "Connecting", then "Pegasus DL 1.11.0" (your Pegasus version).
   - **Store**: a skeleton grid, then tiles for your catalogue. **Every tile is a placeholder with its title** (shown until the cover art is downloaded; it is on by default). That is correct.
   - **Library**: your installed games **with their icons**. Those come from Pegasus (`/appdb/icon`) and need no internet. If these icons show, the art path through the backend works.
   - **Settings**: scroll to the bottom. The **Cover art** section has four rows: "Download store covers (DNS-over-HTTPS)" = Off, "Cover art status" with the sentence "Cover art is off. Turn on DNS-over-HTTPS in Settings ...", "Clear art cache", "Retry failed covers".
4. Leave the app open for about 15 seconds, then read the diagnostics (section 7). Do this before turning anything on: it records the start-up facts (random source, clock, cache folder, certificate bundle) that decide whether HTTPS can work at all.

## 5. What to look for at first launch

| Check | Good | Bad means |
|---|---|---|
| App starts | Shell appears, pill changes | Black screen or returns to the home screen: a crash at start. Send `app.log` and `app.prev.log` (section 7). Most likely suspects: the new TLS code linked into the title, the libc shims. |
| Pegasus link | Pill shows the Pegasus version | "Waiting for Pegasus DL": Pegasus is not running or not on port 6970. |
| Installed game icons | Icons show in Library | Initials only: the backend art path failed (report it). |
| `RAND_status` | `1` | `0`: HTTPS will refuse every connection ("no random number source"). |
| Clock | "looks right: yes" | "NO": certificates will look expired; fix the console date. |
| Cache folder | writable: yes, disk cache on: yes | NO: covers are kept in memory only (smaller, lost on exit). Report which path. |
| CA bundle | 121 certificates from `/app0/assets/ca-bundle.crt` | 0: no HTTPS ("no usable certificate bundle"). |
| Pegasus responsiveness | Downloads tab refreshes about once a second while a job runs | Visible lag with covers on: the cover workers are competing with polling. Report it. |

## 6. Turn cover art on (needs internet)

1. Settings, scroll to **Download store covers (DNS-over-HTTPS)**, press Cross. A dialog repeats the privacy sentence. Choose **Turn on**.
2. A toast says "Cover downloads turned on" and the row shows **On**. The status line becomes "Cover art is on."
3. Go to **Store**. Within a few seconds the visible tiles should fill with real covers; tiles whose cover cannot be fetched stay placeholders. Scroll down a few rows: covers just off screen should already be there (they are prefetched).
4. Expect roughly one in a few hundred to fail or stay a placeholder: AVIF and animated images are not supported, and some hosts reject unusual clients.

If tiles stay placeholders after about 30 seconds, open Settings: the status line says why (examples in section 8). Press **Retry failed covers** once after fixing the cause.

## 7. Cache, restart and the other buttons

1. After covers have loaded, close Wings (PS button, Close Application) and open it again. Covers should appear **at once** (they are on disk), and DNS-over-HTTPS should still be **On** (the choice is saved).
2. Settings: turn DNS-over-HTTPS **Off** (no dialog is needed to turn it off). Restart Wings: covers you already downloaded still show; new ones are not fetched. Status line: "Cover art is off ... Covers that are already downloaded still show."
3. Turn it back on, press **Clear art cache**: toast "Art cache cleared". Covers on screen stay (they are in memory); after a restart they are downloaded again.
4. **Retry failed covers** makes covers that failed (negative cache, 24 h) be asked for again at once.

## 8. Reading the diagnostics

The app writes a short human-readable report. It is written at start, again 10 seconds later (once Pegasus and the catalogue have answered), then every minute and whenever you change a cover art setting.

Where:

- **`/data/wings/diagnostics.txt`**: open Pegasus DL's file manager (web UI, for you `http://10.0.0.113:6970/`, Files) and browse to `/data/wings/`. This is the easy one.
- **`/download0/wings/diagnostics.txt`** inside the title's storage: over FTP while the title runs, `/mnt/sandbox/PPSA99178_000/download0/wings/diagnostics.txt`.
- The title log: `/mnt/sandbox/PPSA99178_000/download0/hui/dev/app.log` (current run) and `app.prev.log` (previous run). Lines from the cover art module start with `[WINGS-ART]`.

The report ends with the list of places it was written to. If `/data/wings/` could not be written (no such folder access from a title), only the `/download0/wings/` copy and the log exist; say so in your report.

What the start-up lines of the title log look like when everything is fine:

```
[WINGS-ART] clock 2026-10-11 14:03:07 UTC plausible=1
[WINGS-ART] RAND_status=1 (before start)
[WINGS-ART] cache dir /download0/wings/art writable=1
[WINGS-ART] preferences default: DNS-over-HTTPS off
[WINGS-ART] trust store /app0/assets/ca-bundle.crt: ...
[WINGS-ART] start=1 phase=off (DNS-over-HTTPS is not enabled) ca=121 (/app0/assets/ca-bundle.crt) disk_ok=1 dns=off
[WINGS-ART] Cover art is off. Turn on DNS-over-HTTPS in Settings to download covers ...
[WINGS-ART] diagnostics written to 2 of 2 places: /data/wings/diagnostics.txt
```

and what the report contains (shortened):

```
Wings diagnostics
App          version 00.001.000, title id PPSA99178, uptime 12 s
Console clock now (UTC) 2026-10-11 14:03:07, looks right: yes
Pegasus DL   state online, address 127.0.0.1:6970, catalogue 2962 packages, 1084 with an https cover URL
Cover art    state off (DNS-over-HTTPS is not enabled)
             DNS mode off (DNS-over-HTTPS is OFF: nothing is downloaded; this is the default)
             CA bundle 121 certificates from /app0/assets/ca-bundle.crt
             RAND_status 1 (ok)
             cache folder /download0/wings/art, folder writable yes, disk cache on yes
             requests ..., failures ... temporary, ... permanent, ... refused as too large
Memory       app heap live ... MiB, peak ... MiB (limit 128 MiB)
```

### Common status lines and what they mean

| Status line or report line | Meaning | What to do |
|---|---|---|
| "Cover art is off. Turn on DNS-over-HTTPS ..." | Default; nothing is downloaded | Turn it on in Settings (section 6). |
| "Cover art is off: this system has no random number source for secure connections." | `RAND_status` is 0 | Report; the title cannot do HTTPS until it is seeded another way. |
| "Cover art is off: no usable certificate bundle was found ..." | CA bundle missing or unreadable | Report the `CA bundle` line. |
| "Cannot reach the name server. Cover art will retry shortly." | DNS-over-HTTPS to 1.1.1.1 / 8.8.8.8 does not work from this console or network | Check that the console has internet, that the router lets TCP 443 to those two addresses through. Report. |
| "... secure connections fail with 'certificate expired / not yet valid'" | Console clock wrong | Set the date and time, press Retry. |
| "Cover art is paused while the app is short of memory." | Heap above 100 MiB | Report the memory line; covers resume under 92 MiB. |
| Status "Cover art is on." but all placeholders | Fetches fail for another reason | Read `failures` and `recent problems` in the report. |

## 9. Report the result

Send back (in this order of usefulness):

1. `diagnostics.txt` **twice**: once from step 4 (cover art off) and once after covers were on for a minute (step 6).
2. The `[WINGS-ART]` lines of `app.log` (or the whole `app.log` and `app.prev.log` if the app crashed).
3. A phone photo of the Settings > Cover art section and of the Store grid after enabling.
4. This form, filled in:

```
Console / firmware / jailbreak:
Pegasus DL version (from the Wings pill):
sha256 matched (yes/no):
Wings tile appeared (yes/no):
Wings starts (yes/no, if no: what happened):
Pill shows Pegasus version (yes/no):
Store grid shows tiles (yes/no):
Library icons show (yes/no):
RAND_status (from diagnostics):
Clock looks right (from diagnostics):
Cache folder writable / disk cache on:
CA bundle certificates:
Where diagnostics were written (/data/wings, /download0/wings):
After turning DNS-over-HTTPS on: covers appeared (yes/no, how long):
Status line text while on:
Covers still there after restart (yes/no):
Setting still On after restart (yes/no):
Pegasus Downloads tab still updating smoothly with covers loading (yes/no):
Anything else:
```

Please do not post the files with tokens or links in public: the diagnostics report contains none, but `app.log` can contain Pegasus log lines.

## 10. Uninstall

1. Close Wings (PS button, Close Application).
2. Delete `/data/homebrew/PPSA99178.ffpkg` (or the folder `/data/homebrew/PPSA99178/`).
3. The tile goes away after ShadowMountPlus's next scan or a reboot of the jailbreak chain. If it stays, reboot and run the jailbreak again without the file in place.
4. Optional leftovers (nothing else is written):
   - `/data/wings/` (diagnostics and a copy of the settings) can be deleted by hand.
   - `/download0/wings/` and `/download0/hui/` live in the title's own storage (`/mnt/sandbox/PPSA99178_000/download0/` while it runs) and go with the title; the cover cache there is at most 96 MiB.
5. Orbit, Pegasus DL and its downloads are not touched by installing or removing Wings.

## 11. Known risks of this first run

Nothing below has been verified on hardware; these are what the first run is for.

- The title may fail to load or crash at start because of the added OpenSSL code (the build only proves it compiles, links, signs and imports the same system libraries as the stock app).
- `RAND_status` may be 0 on the console (the app tries `/dev/urandom` as a top-up and logs the result).
- A title may not be allowed to write `/data/wings` or even `/download0/wings` (the app falls back and says where it wrote).
- Name lookup uses DNS-over-HTTPS to fixed addresses; a network that blocks 1.1.1.1 and 8.8.8.8 will leave covers off. The console's own resolver is not used (it is not available to a native title as far as can be told).
- Four cover workers share the app with Pegasus polling; the cover workers are bounded (4 connections) but their effect on frame time and polling latency on the console is a measurement still to be taken.

---

## 12. Second test (build with settings, library, files, logs and link colours)

Applies to the build whose SHA-256 is recorded in `wings\build-out\package-verification.json` (`imageSha256`). Install it like section 3. Everything below was verified only against a fake Pegasus and recorded replies; this is the first time it meets your real one.

### What is newly testable
- Package page: link colours and order (green automatic / supported, yellow debrid, neutral unknown, red not supported), legend, "Best" chip.
- Settings: capabilities list, download folder picker, download connections, Real-Debrid / TorBox token entry, check token, DNS helper line.
- Logs: level filter, follow / pause, Wings diagnostics view, clear.
- Library: installed games with APR Emu status, details, APR update and reinstall, delete.
- Files: locations, folders, rename, copy, move, delete, extract, pkg install, operations panel.

### SAFE to try on the real console (read-only or easily undone)
- Looking: every tab, the package page link colours, Settings rows, Capabilities list, Logs level filter and the Wings diagnostics view, Library details, Files browsing (locations and folders), the operations list.
- Opening the folder picker and backing out (Circle at the top level). Do not press "Use this folder" unless you mean it.
- Library: "Update APR Emu" / "Reinstall" menu items only up to their confirmation, then Cancel.
- Settings > connections: change to another value and back (16 is Pegasus' default). Harmless.
- Files: create a new folder inside a folder you do not mind (for example `/data/wings-test`), rename it, then delete it.
- Debrid token: entering a token is reversible (Remove token), but it sends the token to Pegasus DL, so only do it with an account you are fine with. Wings never logs it or keeps it; it shows only dots.

### DESTRUCTIVE: test only on a throwaway title or folder
- **Library > Delete this game**: permanently removes the game, its library entry and tile. Two confirmations (second one: type the title id). Use a game you can reinstall or a disposable test title.
- **Library > Update / Reinstall APR Emu**: changes the APR Emu runtime of that title. Try on a title you do not care about first; the 503 on the version list means "reinstall a specific build" will be unavailable on your Pegasus (updating to the latest is a separate route).
- **Files > Delete**, **Move**, **Copy with Replace**, **Extract**: change or remove real files. Do these only inside a throwaway folder you created (for example `/data/wings-test`), never in the downloads folder, a game folder or a system location.
- **Files > Install package**: hands the .pkg to the PS5 installer.
- **Settings > Download folder**: changes where all future downloads go (reversible by choosing the old folder: `/data/pegasus-dl/downloads` on your console).
- **Logs > Clear Pegasus DL's log**: empties Pegasus' log (not reversible, harmless otherwise).

Recommendation: before touching anything destructive, make a throwaway folder (Files > Triangle > New folder) and a copy of one small file in it, and run rename / copy / move / delete / extract there. For delete-game and APR update use one disposable title. If anything surprises you, press Circle and report what was on screen.

### Report
Add to the form in section 9: link colours match what Pegasus shows (yes/no), folder picker opened and saved (yes/no), token result text, Library APR state text for one game, 503 message shown for reinstall (yes/no), Files operations finished (yes/no), anything that looked wrong or froze.

---

## 13. Third test (build with Files polish, Download all parts, password auto-try, prompt keys, tile details)

Applies to the build whose SHA-256 is recorded in `wings\build-out\package-verification.json` (`imageSha256`). Install it like section 3. Everything below was verified only on a PC against a fake Pegasus; this is the first time it meets your console. Do the SAFE parts first. Use a scratch folder for anything that changes files (for example `/data/wings-test`, made with Files > Triangle > New folder), never the downloads folder, a game folder or a system location.

Cover art and popularity data are on by default in this build (existing choices are kept). On a fresh install Wings connects to Cloudflare, Google, the cover servers and `larazadelsol.github.io` right away; turn **Download store covers** off first if you want a run with no internet.

### 13.1 Files: the three reported problems (B1 to B3) on a scratch folder
1. **B1, going up and back to the main drive.** Start at `/`. Go into an external drive (for example `/mnt/ext0`) and press Circle repeatedly. Expected: you can always get back to the main drive and `/`; the drive chips and the breadcrumb show where you are.
2. **B2, big files visible.** Open a folder on an external drive that holds large files (for example `/mnt/ext0/homebrew`). Expected: every file is listed with its size, including multi-GB ones, and the list refreshes (Triangle > Refresh).
3. **B3, a move can be cancelled.** Put a file of a few hundred MB in the scratch folder, Move it to another drive (Triangle > Move to... > choose). While it runs, press Options, select the operation, press Square. Expected: the screen stays usable the whole time, Square asks Pegasus to cancel, a toast says "Cancel requested", the operation ends as cancelled. Note whether a partial file is left at the destination (name and size).
4. Also: copy a small file, rename it, delete it; extract a small ZIP into the scratch folder.

### 13.2 Download all parts
1. Find a game whose page shows a multi-part card (for example "parts 1-3") with few, small parts. Cross, **Choose parts**, **Download all parts**.
2. Expected: a question first; then the parts are queued one at a time; Downloads shows them; a toast or the page says what was queued. Press Circle while one is in flight: it stops after that part.
3. Re-open the page: parts already queued are left out of a second "Download all parts".
4. Note: did every part appear in Pegasus' own queue (web UI)?

### 13.3 Archive password auto-try
Use a small ZIP or RAR with a password.
1. A set whose password is one of the well-known ones: queue it or extract it from Files. Expected: the job shows "Trying known passwords (n/6)" and then continues without a prompt.
2. A set whose password matches none: Expected: after the tries the job shows "Archive password required"; Cross opens the keyboard with a note "Tried 6 known passwords"; typing the right password continues it.
3. Report how Pegasus behaved on a wrong password (did the job text change, did it go through extracting and come back, or did nothing change for about 8 seconds). The first two are assumed; this is the only way to confirm them.
4. Also try a job that waits at "Choose which file to keep" if you have an archive with two packages: Cross, **Choose what to keep**, pick one.

### 13.4 Text-prompt keys (search, Settings, passwords, rename)
In any prompt (Triangle on the Store grid for search; a Files rename; a debrid token):
- R2 and Options submit; Circle cancels; L1 / R1 move the cursor; L2 shift; R3 symbols; L3 clears; Square deletes; Triangle is space, or in a secret field shows and hides the text.
- Move the cursor to the start, type a few letters, then submit with trailing spaces: the text must be accepted and trimmed (this was a bug, the fix is unverified).

### 13.5 Store: tile details and swipe direction
- Settings > Wings > **Tile details**: cycle Always, On focus, Never; tiles show tier and regions accordingly, with the legend in the grid header.
- Swipe on the touchpad: a swipe moves the list or tab the way your finger moves (this was reversed before). Report if it is still opposite.
- Touchpad on the keyboard: slide over the keys and click to type.

### 13.6 Popularity data
1. Settings > Wings > **Popularity data** On: the status line shows "Last updated" and a title count within a minute.
2. Store sort: **Most popular** and **Highest rated** give a plausible order; tiles and pages show a score and rating where the index knows the game.
3. Turn it Off: the badges and the two sorts disappear. Turn it On again: they come back without a restart.
4. Turn **Download store covers** Off: popularity must not fetch (no internet connection at all); the Settings text says it needs cover art.

### 13.7 Diagnostics: the Touchpad block
Open `/data/wings/diagnostics.txt` after you used the touchpad (swipes and key clicks) and wait for the next rewrite (up to a minute). Copy the **Touchpad** block: samples with a finger, contacts, most fingers at once, x and y ranges (expected within the pad's 0 to width-1 and 0 to height-1) and the ids seen. "samples with an impossible touch count" means the touch offsets are wrong.

### 13.8 Report
Add to the form in section 9: B1 to B3 results and whether a partial file was left; Download all parts (queued all, stop worked); auto-try (worked / prompt shown / how Pegasus signalled a wrong password); prompt keys; swipe direction; tile details; popularity (updated, titles, sorts); the Touchpad block; anything that froze.
