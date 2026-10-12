#!/usr/bin/env bash
# Dev helper: copy wings/ to the ext4 work dir the way build-wings-wsl.sh does (no build).
set -euo pipefail
WING=${WING:-/root/wb}; WORK=${WORK:-/root/build/wings}
mkdir -p "$WORK"
rsync -a --delete --exclude /core --exclude /art --exclude /build-out --exclude /app/.deps \
  --exclude /app/build --exclude /app/dist --exclude /app/preview-out \
  --exclude /app/src/wings_core --exclude /app/src/platform/wings_art \
  --exclude /app/assets/ca-bundle.crt "$WING/wings/" "$WORK/"
mkdir -p "$WORK/app/src/wings_core"
rsync -a --delete "$WING/wings/core/include" "$WING/wings/core/src" "$WORK/app/src/wings_core/"
bash "$WING/wings/art/tools/stage-into-app.sh" "$WORK/app"
