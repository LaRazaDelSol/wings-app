#!/usr/bin/env bash
# Usage (from WSL, repo reachable as /root/wb):  bash run.sh [tls_art_test args...]
# Builds the prototype image once, then builds and runs the test in Docker.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../.." && pwd)
docker image inspect wings-tls-art:0.1 >/dev/null 2>&1 || docker build -t wings-tls-art:0.1 "$here"
ca=$root/wings/prototypes/tls-art/ca-bundle.crt
[ -f "$ca" ] || { echo "missing $ca (copy PacBrew etc/ca-bundle.crt here)"; exit 2; }
docker run --rm -v "$root":/work:ro -v wings-art-cache:/cache -w /work wings-tls-art:0.1 bash -c \
  'bash wings/prototypes/tls-art/build.sh >/dev/null && /tmp/tls-art-build/tls_art_test --ca wings/prototypes/tls-art/ca-bundle.crt --library recon/samples/library.json --cache /cache "$@"' _ "$@"
