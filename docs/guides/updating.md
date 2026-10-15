# Updating Wings

## How you find out

When Wings starts, and then at most every 6 hours, it asks GitHub for the latest release of this project (one small file). If that release is newer than the Wings you are running:

- a message at the bottom of the screen says so, once per start, and
- Settings, **Wings** group, shows a **Wings update** row that opens the update page below.

It needs **Download store covers (DNS-over-HTTPS)** on, like popularity data, because it uses the same connection. To stop the check, turn **Check for Wings updates** off in the same section (**Wings update** still checks when you press Cross on it). With cover downloads off Wings makes no internet connection at all.

What the other side learns: GitHub sees the console's IP address and the fixed User-Agent `WingsTV/0.1`. Nothing about you, your games or your settings is sent.

## Installing the update from Wings

When a newer version exists, Wings asks once per version when it starts (**Later** waits three days), and Settings, **Wings** group, shows a **Wings update** row at the top. Both open the update page, which has four steps:

1. **Check.** Wings reads the release and its `PPSA99178.ffpkg.sha256` file. The SHA-256 in the release data and in that file must agree.
2. **Download.** Wings starts its small update helper (see below), which downloads the package into `/data/wings-helper/` and checks it: the listed size, that SHA-256, and that it is a package image. A file that fails any check is deleted. **Cancel** stops it. A finished, checked download is kept, so leaving the page or restarting Wings does not start over: the page goes straight to Install.
3. **Install** (asks first). The helper checks the downloaded file again and puts it in place in `/data/homebrew/PPSA99178.ffpkg` in one step, keeping the old one as `PPSA99178.ffpkg.old` until the new version has run once. Nothing is downloaded at this step and it takes a few seconds. If anything fails, your current version stays.
4. **Restart Wings.** Press **Quit Wings now**, then open Wings again. That is all: the new version runs.

### The update helper

The Wings app itself may not write to `/data/homebrew/`, so Wings ships a small helper inside its package. When you start a download, Wings sends it to the payload loader on this console (`127.0.0.1:9021`), the same way it can load Pegasus DL. The helper:
- only listens on the console itself (`127.0.0.1:9178`) and only answers Wings,
- only downloads the official package of this project from `github.com` (and GitHub's file hosts), over HTTPS,
- only writes `/data/wings-helper/` and `/data/homebrew/PPSA99178.ffpkg` (plus the `.old` copy),
- quits by itself two minutes after its last request.

Things Wings will not do:
- It never installs beside a folder copy (`/data/homebrew/PPSA99178`). It says so and changes nothing.
- The first time, the package in `/data/homebrew/` may be one you placed yourself: the question says so and replaces it only if you confirm.
- It never installs a file whose SHA-256 it could not verify, and if the console loses power during Install, the helper puts the old package back the next time it runs.

## By hand (if the page says it cannot do it)

1. On a PC take `PPSA99178.ffpkg` from the [latest release](https://github.com/LaRazaDelSol/wings-app/releases/latest) and check its SHA-256 against the value on the release page.
2. Copy it to `/data/homebrew/` on the console, over the old file (the Files tab can do this: Copy, then Replace). Check that the size matches the release page.
3. Remove the staged copies ShadowMountPlus made of the old package.
4. Close Wings and open it again.

Replacing the package does not touch the settings copy and diagnostics in `/data/wings/`. The same steps are in the main [Install](../../README.md#install) section.

## What updates by itself

- **Popularity data** is fetched at most once per 24 hours, and once more on the first start after you update Wings, so a new version never runs on old data. If a data file is ever published in a format your Wings does not know, Settings says "this data needs a newer Wings".

## Pegasus DL is separate

Settings, **Pegasus DL** group, shows the running Pegasus DL version, the latest official release, and whether Wings was tested with it. Updating Pegasus DL is its own decision; see [Pegasus auto-load](pegasus-autoload.md).

## Check the version you run

Settings, **Wings** group, **About** > **Wings** shows the version. A build that is not a published release also shows `test` and the time it was built, for example `0.1.1 · test 20261012-203105`; a published release shows only `0.1.1`. The diagnostics report (`/data/wings/diagnostics.txt`) lists the same version and build.
