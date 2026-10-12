# Wings (native PS5 app)

A full-screen PS5 app, drawn by the GPU, that is a front end for Pegasus DL
running on the same console (`127.0.0.1:6970`). Forked from the TV app of Orbit
Store (GPL-3.0-or-later); see `../NOTICE`.

Status: phase 1 skeleton. The shell, tabs and chrome work against a fake
backend in the PC preview; on the console the app only probes Pegasus DL's
`/api/health` and shows "Waiting for Pegasus DL (127.0.0.1:6970)" until it
answers. See `../../docs/wings-app-skeleton.md` for what changed and what is
left.

Title ID `PPSA99178`, content ID `UP9000-PPSA99178_00-WINGSPEGASUS0001`
(`sce_sys/param.json`).

## Build

Inside WSL Ubuntu with Docker (from the repository root):

    wsl -d Ubuntu -u root -- bash -lc 'ln -sfn "/mnt/c/path/to/this/repository" /root/wb; bash /root/wb/tools/build-wings-wsl.sh'

Add `WITH_TEST=1` to run the host unit tests. Outputs: `../build-out/`
(`PPSA99178.ffpkg`, `.zip`, folder, verification record) and `preview-out/`
(PNG pictures of every tab and the waiting state).

Inside the build image the same targets are `make ffpkg`, `make preview`,
`make test`.

## Layout

- `src/orbit/` shell, screens, `Store`, `Backend` seam (namespace `orbit` kept
  from upstream to limit the diff)
- `src/orbit/backend.hpp` the seam; `probe_backend.*` is the console stand-in;
  `host/fake_backend.*` is the preview and test backend
- `src/ui`, `src/gfx`, `src/audio`, `src/core`, `src/platform`, `src/runtime`
  generic kit, untouched
- `host/` PC preview, `tests/unit/` host tests, `tools/` build scripts
