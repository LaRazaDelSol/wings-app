#!/usr/bin/env bash
# Wings TV app - run the host unit tests with live progress and hang detection.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs build/tests/unit_tests (already built by tools/build-tests.sh) and prints, every few seconds:
#   [tests 142/297  ok 142  fail 0  4.2s]  now: App.SurvivesRandomInputEverywhere
# A test that does not finish inside TEST_TIMEOUT seconds (default 240) is killed and named, and that one test
# is re-run with SIGSEGV so the sanitizer prints the stack it was stuck in.
#   env: TEST_TIMEOUT, TEST_FILTER (gtest filter), TEST_LOG (full log path), TEST_TICK (seconds between lines)
set -uo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bin="$root/build/tests/unit_tests"
[[ -x $bin ]] || { echo "run-tests: $bin missing (build the tests first)" >&2; exit 2; }
log=${TEST_LOG:-$root/build/tests/last-run.log}
filter=${TEST_FILTER:-*}
timeout_s=${TEST_TIMEOUT:-240}
tick=${TEST_TICK:-5}
export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

total=$("$bin" --gtest_list_tests --gtest_filter="$filter" 2>/dev/null | grep -c '^  ')
echo "run-tests: $total tests, timeout ${timeout_s}s, log $log"
: >"$log"
start=$(date +%s)
timeout -k 5 "$timeout_s" stdbuf -oL "$bin" --gtest_color=no --gtest_filter="$filter" >"$log" 2>&1 &
pid=$!
last=""
while kill -0 "$pid" 2>/dev/null; do
    sleep "$tick"
    ok=$(grep -c '^\[       OK \]' "$log")
    bad=$(grep -c '^\[  FAILED  \] .* (' "$log")
    now=$(grep '^\[ RUN      \]' "$log" | tail -n1 | sed 's/^\[ RUN      \] //')
    el=$(( $(date +%s) - start ))
    line="[tests $((ok + bad))/$total  ok $ok  fail $bad  ${el}s]  now: ${now:-starting}"
    # One line per change, plus a heartbeat when the same test keeps running (that is how a hang shows).
    if [[ $line != "$last" ]]; then echo "$line"; last=$line; fi
done
wait "$pid"
rc=$?
ok=$(grep -c '^\[       OK \]' "$log")
bad=$(grep -c '^\[  FAILED  \] .* (' "$log")
if [[ $rc -eq 124 || $rc -eq 137 ]]; then
    hung=$(grep '^\[ RUN      \]' "$log" | tail -n1 | sed 's/^\[ RUN      \] //')
    echo "run-tests: HUNG after ${timeout_s}s in: ${hung:-unknown}  (ok $ok, fail $bad)"
    if [[ -n ${hung:-} ]]; then
        echo "run-tests: stack of the stuck test (top frames):"
        timeout -s SEGV 20 "$bin" --gtest_color=no --gtest_filter="$hung" 2>&1 |
            grep -E '^\s+#[0-9]+ ' | head -n 12 | sed 's/(BuildId:[^)]*)//' | cut -c1-200
    fi
    exit 124
fi
grep -E '^\[  FAILED  \] |Failure|error:|runtime error' "$log" | head -n 40
echo "run-tests: ok $ok  fail $bad  exit $rc  ($(( $(date +%s) - start ))s)"
[[ $rc -eq 0 ]] && echo "run-tests: PASS" || echo "run-tests: FAIL (full log: $log)"
exit "$rc"
