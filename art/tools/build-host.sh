#!/usr/bin/env bash
# Builds the module and its tests on the host (run inside the build container, see run-tests.sh).
# Env: WB_ROOT (repo root, default /work), OUT (default /tmp/art-build)
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=${WB_ROOT:-/work}
art=$root/wings/art
out=${OUT:-/tmp/art-build}
mkdir -p "$out"
CXX=${CXX:-clang++-18}
common=(-std=c++17 -O2 -g -Wall -Wextra -Wpedantic -DWINGS_ART_TESTING=1 -I"$art/include")
# The module itself must compile exactly as the console build does: no exceptions, no RTTI.
mkdir -p "$out"; rm -f "$out"/*.o
for f in platform dns doh image_probe disk_cache tls_client art_service; do
  $CXX "${common[@]}" -fno-exceptions -fno-rtti -c "$art/src/$f.cpp" -o "$out/$f.o"
done
# Same sources with the console resolver compiled in (syntax check only; not linked).
$CXX "${common[@]}" -DWINGS_ART_SCENET -fno-exceptions -fno-rtti -fsyntax-only "$art/src/dns.cpp"
$CXX "${common[@]}" -Wno-unused-result -c "$art/tests/test_main.cpp" -o "$out/test_main.o"
$CXX "$out"/*.o -o "$out/art_tests" -lssl -lcrypto -lpthread
# The adapter uses the app's own interface header (wings/app/src/orbit/backend.hpp).
app=$root/wings/app/src
$CXX "${common[@]}" -I"$app" -c "$art/tests/adapter_test.cpp" -o "$out/adapter_test.o.x"
$CXX "$out/adapter_test.o.x" "$out"/{platform,dns,doh,image_probe,disk_cache,tls_client,art_service}.o   -o "$out/adapter_test" -lssl -lcrypto -lpthread
echo "built $out/art_tests"
