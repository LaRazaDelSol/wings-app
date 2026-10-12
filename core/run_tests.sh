#!/usr/bin/env bash
# Builds and runs the Wings core tests with g++ (or $CXX). Run from anywhere.
# Works in Git Bash / MSYS2 (mingw g++) and in WSL/Linux. SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
OUT="${OUT:-build}"
mkdir -p "$OUT"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Iinclude -Itests)
LIB=(src/json.cpp src/http.cpp src/logic.cpp src/pegasus_v1.cpp src/connect.cpp)
TESTS=(tests/test_main.cpp tests/test_logic.cpp tests/test_samples.cpp tests/test_writes.cpp tests/test_connect.cpp tests/test_wire.cpp tests/test_stream.cpp)
EXE="$OUT/wings_core_tests"
"$CXX" "${FLAGS[@]}" "${LIB[@]}" "${TESTS[@]}" -o "$EXE" $( [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && echo -lws2_32 )
if [[ "${1:-}" == "--build-smoke" ]]; then
  "$CXX" "${FLAGS[@]}" "${LIB[@]}" tools/smoke.cpp -o "$OUT/wings_smoke" $( [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && echo -lws2_32 )
  echo "built $OUT/wings_smoke"
  exit 0
fi
"$EXE" "$@"
