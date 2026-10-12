#!/usr/bin/env bash
# Wings TV app - Rebuild the baked SDF fonts in assets/fonts.
# Adapted from ps5-homebrew-ui (GPL-3.0-or-later).
#
# Each line of the table is: source TTF, output name, pixel size, SDF range,
# atlas size and glyph set. Catalogue titles and descriptions use curly quotes,
# dashes, the multiplication sign and accented letters, so every face bakes the
# `european` set (accented Latin, Greek, Cyrillic, quotes, trade mark) on a
# 2048 atlas.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
mkdir -p "$root/build/host" "$root/assets/fonts"
"$cxx" -std=c++20 -O2 -w "$root/tools/font-baker/bake_font.cpp" \
    -o "$root/build/host/bake_font"
while read -r source output size range atlas glyphs; do
    [[ -n $source ]] || continue
    "$root/build/host/bake_font" "$root/third_party/fonts/$source" \
        "$root/assets/fonts/$output.huifont" "$size" "$range" "$atlas" "$glyphs"
done <<'FONTS'
Inter-Regular.ttf inter-regular 56 8 2048 european
Inter-Medium.ttf inter-medium 56 8 2048 european
Inter-SemiBold.ttf inter-semibold 56 8 2048 european
InterDisplay-Light.ttf inter-display-light 56 8 2048 european
FONTS
cp "$root"/third_party/fonts/*-LICENSE.txt "$root/assets/fonts/"
