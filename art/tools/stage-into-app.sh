#!/usr/bin/env bash
# Copies the module into an app work tree the way the build expects it (same idea as wings_core):
#   <app>/src/platform/wings_art/{include,*.cpp,*.hpp,ps5_libc_shims.c}   compiled by the console build only
#   <app>/assets/ca-bundle.crt                                            shipped as /app0/assets/ca-bundle.crt
# src/platform/ is excluded from the PC preview and unit-test builds by tools/preview.sh and
# tools/build-tests.sh, which is exactly what the module + shims need (they link OpenSSL).
# Usage: stage-into-app.sh <path to the app dir, e.g. /root/build/wings/app>
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
art=$(cd "$here/.." && pwd)
app=${1:?usage: stage-into-app.sh <app dir>}
dest=$app/src/platform/wings_art
rm -rf "$dest"; mkdir -p "$dest" "$app/assets"
cp -r "$art/include" "$dest/include"
cp "$art"/src/*.cpp "$art"/src/*.hpp "$dest/"
cp "$art/shims/ps5_libc_shims.c" "$dest/"
cp "$art/ca-bundle.crt" "$app/assets/ca-bundle.crt"
echo "staged wings/art into $dest"
