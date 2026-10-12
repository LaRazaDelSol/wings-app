#!/usr/bin/env bash
# Turn the TEST package in build-out into a RELEASE package without compiling again.
#
# A package carries a build stamp (assets/build-info.json: the build's time and its channel, "test" or
# "release"; see app/tools/wings-stamp.py). This changes only the channel, in the app folder, the zip and
# the .ffpkg image (same-length bytes, nothing moves), then extracts the image again and checks every file
# against the folder (tools/verify-package.sh) and rewrites SHA256SUMS.txt and package-verification.json.
# The compiled program is byte-for-byte the one that was tested.
#
# Run inside WSL (Ubuntu) as a user that can use docker, after a build:
#   wsl -d Ubuntu -u root -- bash -lc 'ln -sfn "/path/to/this/repository" /root/wb; bash /root/wb/tools/mark-release-wsl.sh'
# Env: the same WORK and OUT as the build used (WORK holds app/.deps with the packaging tool), MEM, IMAGE.
set -euo pipefail
WING="${WING:-/root/wb}"
SRC="${SRC:-$WING}"
WORK="${WORK:-/root/build/wings}"
OUT="${OUT:-$SRC/build-out}"
MEM="${MEM:-4g}"
IMAGE="${IMAGE:-wings-app-build-restyle2:0.1}"

[ -d "$WORK/app/.deps/UFS2Tool" ] || { echo "no build workspace at $WORK (run the build first, with the same WORK)" >&2; exit 2; }
ls "$OUT"/PPSA*.ffpkg >/dev/null 2>&1 || { echo "no package in $OUT" >&2; exit 2; }
# The helper scripts of the tree being released, in the workspace the image mounts.
cp "$SRC/app/tools/wings-stamp.py" "$SRC/app/tools/verify-package.sh" "$WORK/app/tools/"

docker run --rm --memory "$MEM" -v "$WORK:/work" -v "$OUT:/dist" -w /work/app -e WINGS_DIST=/dist "$IMAGE" \
  bash -c 'python3 tools/wings-stamp.py release /dist && bash tools/verify-package.sh' | tee "$OUT/verify-package.log"
cp -a "$WORK/app/build/package-verification.json" "$OUT/"
echo "== release package:"
cat "$OUT/SHA256SUMS.txt"
