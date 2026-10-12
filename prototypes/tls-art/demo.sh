#!/usr/bin/env bash
# 1) priority + cold cache, 2) same run again (disk hits), 3) negative tests.
set -u
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
docker volume rm wings-art-cache >/dev/null 2>&1 || true
echo "##### run 1 (cold cache, 4 workers, priority demo, 8 MiB budget forces eviction)"
bash "$here/run.sh" --sample 150 --workers 4 --budget-mb 2 2>&1 | tail -32
echo "##### run 2 (same cache volume)"
bash "$here/run.sh" --sample 150 --workers 4 --budget-mb 64 --no-priority-demo 2>&1 | grep -E "RESULT|from disk|disk cache|decode ms"
echo "##### negative tests"
bash "$here/run.sh" --negative 2>&1 | tail -20
