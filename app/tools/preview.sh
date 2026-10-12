#!/usr/bin/env bash
# Wings TV app - Render the app on the PC (Mesa surfaceless EGL) to PNG pictures.
# Adapted from ps5-homebrew-ui's tools/host-snapshots.sh (GPL-3.0-or-later).
#
# usage: tools/preview.sh [output dir] [scenario|all] [width height]
#
# Builds everything under src/ except the console platform layer, the
# runtime shims and the console entry point, plus host/, and runs the
# scripted scenarios in host/preview_main.cpp, against the fake backend in
# host/fake_backend.cpp.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/ninja-build.sh"
cxx=$(command -v "${HOST_CXX:-clang++}")
cc=$(command -v "${HOST_CC:-clang}")
build="$root/build/preview"
ninja_begin "$build/build.ninja"

sources=()
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$root/host" "$root/src" -type f \( -name '*.cpp' -o -name '*.c' \) \
    ! -path "$root/src/platform/*" ! -path "$root/src/runtime/*" ! -path "$root/src/main.cpp" \
    ! -name snapshot_main.cpp ! -name manifest.cpp -print0 | sort -z)

objects=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    if [[ $source == *.c ]]; then
        ninja_inputs=("$source" "$cc")
        ninja_edge CC "$object" "${compiler_cache[@]}" "$cc" -std=c11 -O2 -w -I"$root/src" \
            -MD -MF "$object.d" -c "$source" -o "$object"
    else
        warnings=(-Wall -Wextra)
        [[ $relative == src/orbit/* || $relative == host/* || $relative == src/main.cpp ]] && warnings+=(-Werror)
        [[ $relative == src/wings_core/* ]] && warnings=(-w)
        ninja_inputs=("$source" "$cxx")
        ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" -std=c++20 -O2 "${warnings[@]}" \
            -DGL_GLEXT_PROTOTYPES=1 -I"$root/src" -I"$root/src/wings_core/include" -I"$root/host" -MD -MF "$object.d" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
ninja_inputs=("${objects[@]}")
ninja_edge LINK "$build/wings_preview" "$cxx" "${objects[@]}" -lEGL -lGL -lwebp -lpthread -lm \
    -o "$build/wings_preview"
if ! ninja_run >"$build/build.log" 2>&1; then
    grep -E 'error|FAILED' -A6 "$build/build.log" | head -120 >&2
    exit 1
fi
grep -E 'warning' -A4 "$build/build.log" | head -40 >&2 || true

output=${1:-"$root/build/preview/pictures"}
mkdir -p "$output"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    "$build/wings_preview" "$root/assets" "$output" "${@:2}"
