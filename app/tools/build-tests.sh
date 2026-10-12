#!/usr/bin/env bash
# Wings TV app - Incremental host GoogleTest compilation.
# Adapted from ps5-homebrew-ui (GPL-3.0-or-later).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds build/tests/unit_tests from tests/unit/*_test.cpp plus the
# platform-neutral application sources listed in tests/unit/sources.txt.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/ninja-build.sh"
gtest=$(bash "$root/tools/setup-test-dependencies.sh")
cxx=$(command -v "${HOST_CXX:-clang++}")
cc=$(command -v "${HOST_CC:-clang}")
# HUI_TEST_SYNTAX_ONLY=1: compile every unit-test source with -fsyntax-only (no objects, no gtest-all, no link)
# into a separate build dir; this is the fast "does it compile" gate (tools/wings.sh check).
syntax_only="${HUI_TEST_SYNTAX_ONLY:-0}"
build="${HUI_TEST_BUILD_DIR:-$root/build/tests}"
[[ $syntax_only == 1 ]] && build="${HUI_TEST_BUILD_DIR:-$root/build/check}"
ninja_begin "$build/build.ninja"
if [[ $syntax_only == 1 ]]; then
    read -r -a flags <<< "${HOST_TEST_CXXFLAGS:--std=c++20 -O0 -Wall -Wextra -Wpedantic -Werror -fsyntax-only}"
else
    read -r -a flags <<< "${HOST_TEST_CXXFLAGS:--std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections}"
fi
read -r -a ldflags <<< "${HOST_TEST_LDFLAGS:--Wl,--gc-sections}"
read -r -a sanitizers <<< "${HOST_TEST_SANITIZERS:--fsanitize=address,undefined -fno-omit-frame-pointer}"

sources=("$gtest/googletest/src/gtest-all.cc" "$gtest/googletest/src/gtest_main.cc")
while IFS= read -r -d '' test_source; do
    sources+=("$test_source")
done < <(find "$root/tests/unit" -type f -name '*_test.cpp' -print0 | sort -z)
while IFS= read -r line; do
    line=${line%%#*}
    line=${line//[[:space:]]/}
    [[ -n $line ]] || continue
    if [[ $line == *'*'* ]]; then
        mapfile -t matches < <(cd "$root" && compgen -G "$line" | sort)
        (( ${#matches[@]} )) || { echo "tests/unit/sources.txt: no match for $line" >&2; exit 2; }
        for match in "${matches[@]}"; do sources+=("$root/$match"); done
        continue
    fi
    [[ -f $root/$line ]] || { echo "tests/unit/sources.txt: missing $line" >&2; exit 2; }
    sources+=("$root/$line")
done < "$root/tests/unit/sources.txt"

objects=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    relative=${relative#"$gtest/"}
    object="$build/obj/${relative//\//_}.o"
    # Syntax-only skips gtest's own translation units and the vendored C.
    if [[ $syntax_only == 1 && ( $source == "$gtest/"* || $source == *.c ) ]]; then continue; fi
    ninja_inputs=("$source" "$cxx")
    if [[ $source == "$gtest/"* ]]; then
        ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" -std=c++20 -O2 -pthread \
            "${sanitizers[@]}" -isystem "$gtest/googletest/include" -I"$gtest/googletest" \
            -MD -MF "$object.d" -c "$source" -o "$object"
    elif [[ $source == *.c ]]; then
        # Vendored C builds without -Werror.
        ninja_inputs=("$source" "$cc")
        ninja_edge CC "$object" "${compiler_cache[@]}" "$cc" -std=c11 -O2 -w \
            "${sanitizers[@]}" -I"$root/src" -MD -MF "$object.d" -c "$source" -o "$object"
    elif [[ $source == "$root/src/wings_core/"* ]]; then
        # The Pegasus adapter library is built as published (it has its own tests).
        ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" -std=c++20 -O2 -w -pthread \
            "${sanitizers[@]}" -I"$root/src/wings_core/include" -MD -MF "$object.d" \
            -c "$source" -o "$object"
    else
        ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" "${flags[@]}" -pthread \
            "${sanitizers[@]}" "-DHUI_SOURCE_DIR=\"$root\"" -I"$root/src" -I"$root/tests" \
            -I"$root/src/wings_core/include" -I"$root/host" -isystem "$gtest/googletest/include" -MD -MF "$object.d" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
if [[ $syntax_only == 1 ]]; then ninja_run; exit 0; fi
ninja_inputs=("${objects[@]}" "$cxx")
ninja_edge LINK "$build/unit_tests" "$cxx" "${flags[@]}" -pthread "${sanitizers[@]}" \
    "${objects[@]}" "${ldflags[@]}" -lwebp -lm -o "$build/unit_tests"
ninja_run
