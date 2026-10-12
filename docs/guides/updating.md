# Updating Wings

## How you find out

When Wings starts, and then at most every 6 hours, it asks GitHub for the latest release of this project (one small file). If that release is newer than the Wings you are running:

- a message at the bottom of the screen says so, once per start, and
- Settings, **Wings** group, **Updates** > **Wings update** shows the new version. Press Cross on it to see the steps below on the screen. On any other state, Cross checks again right away.

It needs **Download store covers (DNS-over-HTTPS)** on, like popularity data, because it uses the same connection. To stop the check, turn **Check for Wings updates** off in the same section (**Wings update** still checks when you press Cross on it). With cover downloads off Wings makes no internet connection at all.

What the other side learns: GitHub sees the console's IP address and the fixed User-Agent `WingsTV/0.1`. Nothing about you, your games or your settings is sent.

## Wings never installs itself

A package that is open cannot be replaced, and ShadowMountPlus only registers a new package after the old one is gone. So the update is a few steps by hand:

1. On a PC, download `PPSA99178.ffpkg` from the [latest release](https://github.com/LaRazaDelSol/wings-app/releases/latest) and check its SHA-256 against the value on the release page.
2. Copy it to `/data/homebrew/` on the console, over the old file. Check that the size matches the release page.
3. Remove the staged copies ShadowMountPlus made of the old package.
4. Restart the jailbreak chain so the new package is mounted, then open Wings.

Replacing the package does not touch `/data/wings/` (the settings copy and diagnostics). The same steps are in the main [Install](../../README.md#install) section.

## What updates by itself

- **Popularity data** is fetched at most once per 24 hours, and once more on the first start after you update Wings, so a new version never runs on old data. If a data file is ever published in a format your Wings does not know, Settings says "this data needs a newer Wings".

## Pegasus DL is separate

Settings, **Pegasus DL** group, shows the running Pegasus DL version, the latest official release, and whether Wings was tested with it. Updating Pegasus DL is its own decision; see [Pegasus auto-load](pegasus-autoload.md).

## Check the version you run

Settings, **Wings** group, **About** > **Wings** shows the version. A build that is not a published release also shows `test` and the time it was built, for example `0.1.1 · test 20261012-203105`; a published release shows only `0.1.1`. The diagnostics report (`/data/wings/diagnostics.txt`) lists the same version and build.
