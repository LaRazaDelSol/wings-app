#!/usr/bin/env bash
# Worker-count sweep: fresh disk cache each time, same sample.
set -u
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
for w in ${WORKERS:-1 2 4 8}; do
  echo "=== workers=$w"
  bash "$here/run.sh" --sample "${SAMPLE:-250}" --workers "$w" --cache "/tmp/c$w-$$" --no-priority-demo 2>&1 |
    grep -E "RESULT|network ms|first byte|setup|keep-alive|decode ms|peak|FAIL|from disk|compressed|^sample"
done
