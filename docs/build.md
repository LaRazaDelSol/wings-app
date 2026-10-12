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

## Notes
- Build from a WSL ext4 path, not `/mnt/c`: Docker on a Windows mount is slow.
- `core/` (the Pegasus adapter library) builds with any C++17 compiler; see [core/README.md](../core/README.md).
- Some host tests read recorded Pegasus API samples from `recon/samples`. Those samples are third-party data and are
  not part of this repository; tests that need them are skipped or fail until you capture your own.
- The popularity data tooling is separate: see [popularity-index/README.md](../popularity-index/README.md).
