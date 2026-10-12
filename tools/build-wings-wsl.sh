#!/usr/bin/env bash
# Build the Wings TV app (.ffpkg) and its PC preview inside Docker in WSL2.
# Adapted from tools/build-wsl.sh (the stock Orbit build). No payload is bundled.
# Run inside WSL (Ubuntu) as a user that can use docker (root here):
#   wsl -d Ubuntu -u root -- bash -lc 'ln -sfn "/path/to/this/repository" /root/wb; bash /root/wb/tools/build-wings-wsl.sh'
# Env: WORK (ext4 build dir), OUT (artifact dir), JOBS, MEM,
#      WITH_PREVIEW=1 (default) renders the preview pictures, WITH_TEST=1 runs `make test`.
set -euo pipefail
WING="${WING:-/root/wb}"
SRC="${SRC:-$WING}"
WORK="${WORK:-/root/build/wings}"
OUT="${OUT:-$SRC/build-out}"
PREVIEW_OUT="${PREVIEW_OUT:-$SRC/app/preview-out}"
JOBS="${JOBS:-4}"; MEM="${MEM:-8g}"
IMAGE="${IMAGE:-wings-app-build-restyle2:0.1}"
t0=$(date +%s); lap(){ echo "== [$(( $(date +%s)-t0 ))s] $*"; }

lap "copy source to ext4 ($WORK)"
mkdir -p "$WORK" "$OUT"
# wings/ as a whole, so that ../i18n exists next to app/ (build.sh copies catalogues from it).
rsync -a --delete --exclude /.git --exclude /.github --exclude /popularity-index --exclude /core --exclude /art --exclude /build-out --exclude /app/.deps \
  --exclude /app/build --exclude /app/dist --exclude /app/preview-out \
  --exclude /app/src/platform/wings_art --exclude /app/assets/ca-bundle.crt "$SRC/" "$WORK/"
# The Pegasus adapter library (wings/core) is compiled into the app: its headers and
# sources are copied under app/src/wings_core (the app build globs src/**). The core's
# own tests and tools stay out.
mkdir -p "$WORK/app/src/wings_core"
rsync -a --delete "$SRC/core/include" "$SRC/core/src" "$WORK/app/src/wings_core/"
# The HTTPS cover art module (wings/art; C++17, OpenSSL) is staged under
# app/src/platform/wings_art by its own script: sources and the libc shims for
# the console build only (the preview and unit-test builds skip src/platform),
# plus assets/ca-bundle.crt. wings/art stays the source of truth.
bash "$SRC/art/tools/stage-into-app.sh" "$WORK/app"
# The host tests read the recorded Pegasus samples (recon/samples) at /recon.
RECON_MOUNT=()
[ -d "$WING/recon/samples" ] && RECON_MOUNT=(-v "$WING/recon:/recon:ro")
# The first stock build already downloaded the pinned SDKs and ports; reuse them.
if [ ! -d "$WORK/app/.deps" ] && [ -d /root/build/orbit/app/.deps ]; then
  lap "seed .deps from the stock build"
  cp -a /root/build/orbit/app/.deps "$WORK/app/.deps"
fi

cd "$WORK"
lap "docker build $IMAGE"
docker build -t "$IMAGE" app

lap "make ffpkg"
docker run --rm --cpus "$JOBS" --memory "$MEM" -v "$WORK:/work" -w /work/app -e BUILD_JOBS="$JOBS" \
  "$IMAGE" make ffpkg

lap "verify-package"
docker run --rm --cpus "$JOBS" --memory "$MEM" -v "$WORK:/work" -w /work/app "$IMAGE" \
  bash tools/verify-package.sh | tee "$WORK/app/dist/verify-package.log"

lap "copy artifacts"
cp -a "$WORK/app/dist/." "$OUT/"
cp -a "$WORK/app/build/package-verification.json" "$OUT/" 2>/dev/null || true
ls -la "$OUT"

if [ "${WITH_TEST:-0}" = 1 ]; then
  lap "make test (host unit tests, sanitizers)"
  docker run --rm --cpus "$JOBS" --memory "$MEM" -v "$WORK:/work" "${RECON_MOUNT[@]}" -w /work/app "$IMAGE" make test \
    | tee "$OUT/unit-tests.log"
fi
if [ "${WITH_TEST:-0}" = 1 ]; then
  lap "library peak heap (real sample through the backend)"
  docker run --rm --cpus "$JOBS" --memory "$MEM" -v "$WORK:/work" "${RECON_MOUNT[@]}" -w /work/app "$IMAGE" \
    bash tools/library-peak.sh /recon/samples/library.json | tee "$OUT/library-peak.log"
fi
if [ "${WITH_PREVIEW:-1}" = 1 ]; then
  lap "make preview (PC host target, fake backend)"
  docker run --rm --cpus "$JOBS" --memory "$MEM" -v "$WORK:/work" -w /work/app "$IMAGE" make preview
  rm -rf "$PREVIEW_OUT"; mkdir -p "$PREVIEW_OUT"
  cp -a "$WORK/app/build/preview/pictures/." "$PREVIEW_OUT/"
  lap "preview done: $(ls "$PREVIEW_OUT" | wc -l) pictures"
fi
lap "done"
