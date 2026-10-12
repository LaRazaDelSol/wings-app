#!/usr/bin/env bash
# Hermetic host test run in Docker (from WSL; repo reachable as /root/wb, see docs/build.md):
#   bash /root/wb/wings/art/tools/run-tests.sh            # unit + local TLS fixture + safety suite
#   bash /root/wb/wings/art/tools/run-tests.sh --real     # plus real CDNs via DNS-over-HTTPS
#   bash /root/wb/wings/art/tools/run-tests.sh --filter safety
# Image: wings-tls-art:0.1 (the app build image + libssl-dev + g++), built by the TLS prototype's
# Dockerfile; created here from the app image if it is missing.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../.." && pwd)
if ! docker image inspect wings-tls-art:0.1 >/dev/null 2>&1; then
  docker build -t wings-tls-art:0.1 "$root/wings/prototypes/tls-art"
fi
docker run --rm -v "$root":/work:ro -w /work wings-tls-art:0.1 bash -c '
  set -e
  bash wings/art/tools/build-host.sh
  bash wings/art/tests/gen_certs.sh /tmp/art-certs >/dev/null
  /tmp/art-build/art_tests --certs /tmp/art-certs --ca wings/art/ca-bundle.crt "$@"
  /tmp/art-build/adapter_test' _ "$@"
