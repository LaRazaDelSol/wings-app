#!/usr/bin/env bash
# PS5 cross-link probe (run from WSL): copies the already built Wings app tree, adds the art module,
# the libc shims and the on-console probe, builds with the app's PS5 toolchain, and reports whether
# everything compiles (-fno-exceptions -fno-rtti), links and signs. Output app: $PROBE/app/dist/.
#   bash wings/art/tools/link-probe.sh            # DoH-only probe (safe to launch)
#   SCENET=1 bash wings/art/tools/link-probe.sh   # also imports libSceNet and tries the console resolver
#                                                 # (a separate build: a bad import may stop the title loading)
# Env: SRC_TREE (default /root/build/wings, made by tools/build-wings-wsl.sh), PROBE_TREE, IMAGE.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
art=$(cd "$here/.." && pwd)
src=${SRC_TREE:-/root/build/wings}
probe=${PROBE_TREE:-/root/build/art-probe${SCENET:+-scenet}}
image=${IMAGE:-wings-app-build:0.1}
[ -d "$probe" ] || cp -a "$src" "$probe"
dest=$probe/app/src/wings_art
rm -rf "$dest"; mkdir -p "$dest"
cp -r "$art/include" "$dest/include"
cp "$art"/src/*.cpp "$art"/src/*.hpp "$dest/"
cp "$art/shims/ps5_libc_shims.c" "$dest/"
cp "$art/probe/art_probe.cpp" "$dest/"
mkdir -p "$probe/app/assets"
cp "$art/ca-bundle.crt" "$probe/app/assets/ca-bundle.crt"
defs=""; stubs=""
if [ "${SCENET:-0}" = 1 ]; then
  defs="WINGS_ART_SCENET=1"
  stubs=".deps/native/ps5-payload-sdk/target/lib/libSceNet.so"
fi
docker run --rm -v "$probe":/work -w /work/app "$image" bash -c '
  export PACBREW_STATIC_ARCHIVES="lib/libwebp.a lib/libsharpyuv.a lib/libssl.a lib/libcrypto.a"
  export APP_INCLUDE_PATHS="src/wings_art/include"
  export APP_DEFINITIONS="'"$defs"'"
  export APP_IMPORT_STUBS="'"$stubs"'"
  make app BUILD_JOBS=8 2>&1 | tail -${TAIL:-60}'
