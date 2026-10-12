#!/usr/bin/env bash
# Wings TV app - Builds and runs tests/tools/library_peak.cpp (no sanitizers).
# usage: tools/library-peak.sh /path/to/library.json
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/build/library-peak"
mkdir -p "$out"
cxx=${HOST_CXX:-clang++}
"$cxx" -std=c++20 -O2 -w -pthread -I"$root/src" -I"$root/src/wings_core/include" \
    "$root/tests/tools/library_peak.cpp" "$root/src/orbit/pegasus_backend.cpp" \
    "$root"/src/wings_core/src/*.cpp -o "$out/library_peak"
"$out/library_peak" "${1:-/recon/samples/library.json}"
