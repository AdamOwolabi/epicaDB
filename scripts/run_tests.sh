#!/usr/bin/env bash
# scripts/run_tests.sh -- runs every check in the project, in order, and
# stops at the first failure. Each stage can be run alone:
#
#   scripts/run_tests.sh            all stages
#   scripts/run_tests.sh 3          only stage 3
#   scripts/run_tests.sh 3 6        stages 3 through 6
#
# Stages
#   1  configure + build (Debug)
#   2  C++ unit + integration tests (GoogleTest, 88 tests)
#   3  WAL crash test        (wal_driver, kill -9, recover)
#   4  whole-DB crash test   (db_driver, kill -9, verify)
#   5  Java unit tests       (Maven, no engine needed)
#   6  Java integration test against a live epica_server
#   7  end-to-end: nc -> Java query server -> C++ engine
#   8  sanitizer build + tests (ASan + UBSan; slow, ~2-3 min)
#
# See TESTING.md for what each stage proves and how to do it by hand.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# JDK 17 (Homebrew's mvn defaults to a newer JDK; the pom targets 17).
if [ -z "${JAVA_HOME:-}" ] && [ -x /usr/libexec/java_home ]; then
  export JAVA_HOME="$(/usr/libexec/java_home -v 17 2>/dev/null || true)"
fi

FROM="${1:-1}"
TO="${2:-${1:-8}}"
PORT_ENGINE="${EPICA_TEST_ENGINE_PORT:-7481}"
PORT_QUERY="${EPICA_TEST_QUERY_PORT:-7482}"
TMP="${TMPDIR:-/tmp}/epica_test_$$"
mkdir -p "$TMP"

PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -TERM "$p" 2>/dev/null || true; done
  sleep 0.3
  rm -rf "$TMP"
}
trap cleanup EXIT

banner() { printf '\n\033[1;34m== stage %s: %s ==\033[0m\n' "$1" "$2"; }
pass()   { printf '\033[1;32mPASS\033[0m %s\n' "$1"; }
fail()   { printf '\033[1;31mFAIL\033[0m %s\n' "$1"; exit 1; }
want()   { [ "$FROM" -le "$1" ] && [ "$1" -le "$TO" ]; }

if want 1; then
  banner 1 "configure + build"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
  cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" 2>&1 | grep -E "error|warning: " && fail "build" || true
  [ -x build/engine/engine_tests ] || fail "engine_tests not built"
  pass "build"
fi

if want 2; then
  banner 2 "C++ tests (GoogleTest)"
  ./build/engine/engine_tests --gtest_brief=1 2>/dev/null | tail -3
  ./build/engine/engine_tests >/dev/null 2>&1 || fail "engine_tests"
  pass "engine tests"
fi

if want 3; then
  banner 3 "WAL crash test (kill -9)"
  D="$TMP/wal"
  ./build/engine/wal_driver "$D" >"$TMP/wal_run.log" 2>&1 & W=$!
  sleep 2; kill -9 "$W" 2>/dev/null || true; wait "$W" 2>/dev/null || true
  ./build/engine/wal_driver --recover "$D" | tee "$TMP/wal_rec.log"
  grep -q "sequence contiguous" "$TMP/wal_rec.log" || fail "WAL recovery lost or reordered records"
  pass "WAL survives kill -9"
fi

if want 4; then
  banner 4 "whole-DB crash test (kill -9, twice)"
  D="$TMP/db"
  for round in 1 2; do
    ./build/engine/db_driver "$D" >"$TMP/db_run$round.log" 2>&1 & W=$!
    sleep 3; kill -9 "$W" 2>/dev/null || true; wait "$W" 2>/dev/null || true
    ./build/engine/db_driver --verify "$D" 2>/dev/null | grep -E "verified|empty" | tee "$TMP/db_verify$round.log"
    grep -q "(OK)" "$TMP/db_verify$round.log" || fail "DB verify round $round"
  done
  pass "DB recovers every acknowledged write across two crashes"
fi

if want 5; then
  banner 5 "Java unit tests (Maven)"
  ( cd server && mvn -q -B test ) || fail "mvn test"
  grep -h "Tests run" server/target/surefire-reports/*.txt | sed 's/ -- in / /' 
  pass "java unit tests"
fi

if want 6; then
  banner 6 "Java integration test against live engine (port $PORT_ENGINE)"
  ./build/engine/epica_server "$TMP/itest" "$PORT_ENGINE" >"$TMP/engine6.log" 2>&1 & E=$!; PIDS+=("$E")
  sleep 0.7
  ( cd server && EPICA_ENGINE_PORT="$PORT_ENGINE" mvn -q -B test -Dtest=EngineIntegrationTest -Dsurefire.failIfNoSpecifiedTests=false ) || fail "integration test"
  grep -h "Tests run" server/target/surefire-reports/*Integration*.txt
  grep -q "Skipped: 0" server/target/surefire-reports/*Integration*.txt || fail "integration test was skipped"
  kill -TERM "$E"; wait "$E" 2>/dev/null || true
  pass "java <-> engine wire contract"
fi

if want 7; then
  banner 7 "end-to-end: nc -> Java query server ($PORT_QUERY) -> engine ($PORT_ENGINE)"
  [ -f server/target/epica-query.jar ] || ( cd server && mvn -q -B package -DskipTests )
  ./build/engine/epica_server "$TMP/e2e" "$PORT_ENGINE" >"$TMP/engine7.log" 2>&1 & E=$!; PIDS+=("$E")
  sleep 0.5
  "$JAVA_HOME/bin/java" -jar server/target/epica-query.jar server "$PORT_QUERY" "127.0.0.1:$PORT_ENGINE" >"$TMP/query7.log" 2>&1 & Q=$!; PIDS+=("$Q")
  sleep 1.5
  printf 'PUT user:1 "Ada Lovelace"\nPUT user:2 "Alan Turing"\nGET user:1\nGET nope\nSCAN PREFIX user: WHERE VALUE CONTAINS Alan LIMIT 1\nBATCH DEL user:1; PUT user:3 Grace END\nCOUNT PREFIX user:\nquit\n' \
    | nc 127.0.0.1 "$PORT_QUERY" | tee "$TMP/e2e.out"
  grep -q 'VALUE "Ada Lovelace"' "$TMP/e2e.out" || fail "GET through query server"
  grep -q 'ROW user:2 "Alan Turing"' "$TMP/e2e.out" || fail "filtered scan"
  grep -q 'COUNT 2' "$TMP/e2e.out" || fail "batch + count"
  kill -TERM "$Q" "$E"; wait 2>/dev/null || true
  pass "full stack"
fi

if want 8; then
  banner 8 "sanitizers (ASan + UBSan)"
  cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEPICA_SANITIZE=ON >/dev/null
  cmake --build build-asan -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" 2>&1 | grep -E "error" && fail "asan build" || true
  ./build-asan/engine/engine_tests --gtest_brief=1 2>&1 | grep -E "PASSED|FAILED|ERROR: " | tail -3
  ./build-asan/engine/engine_tests >/dev/null 2>&1 || fail "asan tests"
  pass "no memory errors or undefined behaviour"
fi

printf '\n\033[1;32mALL REQUESTED STAGES PASSED\033[0m\n'
