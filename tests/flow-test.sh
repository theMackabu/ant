#!/bin/sh
# End-to-end flow test: boots every server example, drives it with enough
# keep-alive load to cross the JIT threshold (SV_JIT_THRESHOLD=100 calls)
# and several minor-GC cycles, and fails on any non-200, runtime error, or
# hang. Catches the class of bug that only appears after N requests
# (JIT miscompiles, GC corruption, keep-alive state rot) which single-shot
# smoke tests miss.
#
# Usage: tests/flow-test.sh [path-to-ant-binary]
# Requires: oha (falls back to a curl loop if missing)

set -u
BIN=${1:-./build/ant}
PORT=3000
REQUESTS=600
CONCURRENCY=8
PASS=0
FAIL=0
LOG=$(mktemp /tmp/ant-flow.XXXXXX)

[ -x "$BIN" ] || { echo "no ant binary at $BIN"; exit 2; }
HAVE_OHA=0
command -v oha >/dev/null 2>&1 && HAVE_OHA=1

drive() {
  # $1 = url; prints "ok" or reason
  if [ "$HAVE_OHA" = 1 ]; then
    rate=$(timeout 30 oha -n $REQUESTS -c $CONCURRENCY --no-tui "$1" 2>/dev/null \
      | grep -oE "Success rate:[[:space:]]+[0-9.]+" | grep -oE "[0-9.]+$")
    [ "${rate:-}" = "100.00" ] && echo ok || echo "success-rate=${rate:-hang}"
  else
    i=0
    while [ $i -lt 200 ]; do
      code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "$1") || { echo "curl-fail@$i"; return; }
      [ "$code" = "200" ] || { echo "http-$code@$i"; return; }
      i=$((i + 1))
    done
    echo ok
  fi
}

run_server() {
  # $1 = name, $2 = entry (relative to repo root), $3 = url path
  name=$1; entry=$2; path=${3:-/}
  pkill -f "$entry" 2>/dev/null; sleep 0.5
  : > "$LOG"
  "$BIN" "$entry" > "$LOG" 2>&1 &
  pid=$!
  sleep 2
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "FAIL $name (server exited at startup)"; sed -n 1,6p "$LOG"
    FAIL=$((FAIL + 1)); return
  fi
  verdict=$(drive "http://127.0.0.1:$PORT$path")
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  errs=$(grep -cE "TypeError|ReferenceError|Invalid |SIGBUS|panic" "$LOG")
  if [ "$verdict" = "ok" ] && [ "$errs" -eq 0 ]; then
    echo "PASS $name"
    PASS=$((PASS + 1))
  else
    echo "FAIL $name ($verdict, $errs runtime errors)"
    grep -m3 -E "TypeError|ReferenceError|Invalid |panic" "$LOG"
    FAIL=$((FAIL + 1))
  fi
}

run_script() {
  # $1 = name, $2 = entry; passes if exit 0 and no error markers
  name=$1; entry=$2
  out=$(timeout 60 "$BIN" "$entry" 2>&1)
  code=$?
  errs=$(printf '%s' "$out" | grep -cE "TypeError|ReferenceError|SIGBUS|panic")
  if [ $code -eq 0 ] && [ "$errs" -eq 0 ]; then
    echo "PASS $name"
    PASS=$((PASS + 1))
  else
    echo "FAIL $name (exit=$code, $errs errors)"
    printf '%s\n' "$out" | tail -4
    FAIL=$((FAIL + 1))
  fi
}

echo "== server examples (${REQUESTS} reqs, keep-alive, c=${CONCURRENCY}) =="
run_server hono    examples/npm/hono/src/index.ts
run_server express examples/npm/express/index.cjs
run_server h3      examples/npm/h3
run_server elysia  examples/npm/elysia

echo "== script examples =="
run_script smoke examples/npm/smoke

echo "== spec suites =="
spec_fail=0
for f in examples/spec/*.js; do
  [ "$f" = "examples/spec/run.js" ] && continue
  out=$(timeout 60 "$BIN" "$f" 2>&1)
  code=$?
  failed=$(printf '%s' "$out" | grep -oE "Failed: [0-9]+" | grep -oE "[0-9]+" | tail -1)
  if [ $code -ne 0 ] || { [ -n "${failed:-}" ] && [ "$failed" -ne 0 ]; }; then
    echo "FAIL spec/$(basename "$f") (exit=$code failed=${failed:-?})"
    spec_fail=$((spec_fail + 1))
  fi
done
[ $spec_fail -eq 0 ] && { echo "PASS spec suites"; PASS=$((PASS + 1)); } || FAIL=$((FAIL + spec_fail))

echo "== regression tests =="
for t in tests/test_promise.cjs tests/test_arguments_async.cjs tests/test_upvalue_gc.cjs tests/test_jit_derived_ctor.cjs; do
  [ -f "$t" ] || continue
  if timeout 60 "$BIN" "$t" > /dev/null 2>&1; then
    echo "PASS $(basename "$t")"; PASS=$((PASS + 1))
  else
    echo "FAIL $(basename "$t")"; FAIL=$((FAIL + 1))
  fi
done

rm -f "$LOG"
echo
echo "flow-test: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
