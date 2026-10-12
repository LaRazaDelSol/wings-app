#!/usr/bin/env bash
# Cross-link probe: copy the already-built Orbit app tree, add the prototype
# client, build with the app's PS5 toolchain inside the app build image, and
# report whether libssl/libcrypto/getaddrinfo/sockets resolve. Run from WSL.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=${SRC_TREE:-/root/build/orbit}
probe=${PROBE_TREE:-/root/build/probe}
if [ ! -d "$probe" ]; then cp -a "$src" "$probe"; fi
mkdir -p "$probe/app/src/wings_probe"
cp "$here/tls_client.cpp" "$here/tls_client.hpp" "$here/probe_app.cpp" "$here/ps5_libc_shims.c" "$here/doh.cpp" "$here/doh.hpp" "$probe/app/src/wings_probe/"
mkdir -p "$probe/app/assets"
cp "$here/ca-bundle.crt" "$probe/app/assets/ca-bundle.crt"
docker run --rm -v "$probe":/work -w /work/app orbit-app-build:0.1 bash -c '
  export PACBREW_STATIC_ARCHIVES="lib/libwebp.a lib/libsharpyuv.a lib/libssl.a lib/libcrypto.a"
  export APP_INCLUDE_PATHS="src/wings_probe"
  make app ORBIT_PAYLOAD=none BUILD_JOBS=8 2>&1 | tail -${TAIL:-60}'
