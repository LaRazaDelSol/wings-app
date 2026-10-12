# Building Wings

Wings is built inside Docker in WSL2 (Ubuntu). The result is `PPSA99178.ffpkg` in `build-out/`.

## Requirements
- Windows with WSL2 (Ubuntu), or a Linux machine, with Docker running
- `rsync`, `curl` and an internet connection (the build downloads pinned SDKs and ports; each is hash-pinned in `app/tools/*.sh`)
- Roughly 5 GB of free disk space

## Build
From the repository root:

```
bash tools/build-wings-wsl.sh
```

On Windows, from PowerShell (the symlink avoids quoting trouble with spaces in paths):

```
wsl -d Ubuntu -u root -- bash -lc 'ln -sfn "/mnt/c/path/to/this/repository" /root/wb; bash /root/wb/tools/build-wings-wsl.sh'
```

Environment variables: `WORK` (ext4 build directory), `OUT` (artifact directory), `JOBS`, `MEM`,
`WITH_TEST=1` (run the host unit tests), `WITH_PREVIEW=1` (render the PC preview pictures, on by default).

## Build identity and releases
Every build is stamped: `assets/build-info.json` inside the package holds the build's UTC time (`20261012-203105`) and its
channel, `test` or `release`. A build is a `test` build unless you set `WINGS_CHANNEL=release`; `WINGS_BUILD_ID` overrides
the time. Settings shows a test build as `0.1.1 · test 20261012-203105` and a release as `0.1.1`. The version itself is
`contentVersion` in `app/sce_sys/param.json` (`00.001.001` is the release tag `v0.1.1`); bump it before building a release.

To publish a package you already built and tested, turn it into a release package without compiling again:

```
bash tools/mark-release-wsl.sh
```

(with the same `WORK` and `OUT` as the build; on Windows use the `wsl ... bash -lc` form above). It changes only the channel,
in the app folder, the zip and the `.ffpkg` (same-length bytes, nothing moves in the image), extracts the image again,
checks every file against the folder (`app/tools/verify-package.sh`) and rewrites `SHA256SUMS.txt` and
`package-verification.json`. The compiled program is the one that was tested. Every build also rewrites `SHA256SUMS.txt`,
and removes the files of the previous build from the output folder, so the checksum list always belongs to the package next to it.

## Notes
- Build from a WSL ext4 path, not `/mnt/c`: Docker on a Windows mount is slow.
- `core/` (the Pegasus adapter library) builds with any C++17 compiler; see [core/README.md](../core/README.md).
- Some host tests read recorded Pegasus API samples from `recon/samples`. Those samples are third-party data and are
  not part of this repository; tests that need them are skipped or fail until you capture your own.
- The popularity data tooling is separate: see [popularity-index/README.md](../popularity-index/README.md).
