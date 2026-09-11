#!/usr/bin/env bash
# Checks that the bytecode compiler's operand-depth analysis accepts every
# function in the spec suite, the runtime tests and the JIT examples.
#
# The analysis (sv_func_compute_max_stack in src/silver/compiler.c) derives
# each op's stack effect from the n_pop/n_push columns of OP_DEF in
# include/silver/opcode.h. A wrong row, or an emitter that leaves a loop
# unbalanced, makes the analysis reject the function; the release build then
# logs "jit: operand depth analysis failed" under ANT_DEBUG=dump/vm:op-warn
# and falls back to a generous bound. This script fails if any such line
# appears. Run it after touching opcode.h, a handler in src/silver/ops/, or
# the loop/try emitters in the compiler.
#
# usage: tools/check_stack_depth.sh [path/to/ant]
set -u
cd "$(dirname "$0")/.."
ANT=${1:-./build/ant}
export ANT_DEBUG=dump/vm:op-warn
log=$(mktemp)
trap 'rm -f "$log"' EXIT

run() { "$ANT" "$@" 2>>"$log" >/dev/null || true; }

run examples/spec/run.js --all
run examples/jit/run.js --all
for t in tests/test_*.cjs tests/test_*.mjs; do
  [ -f "$t" ] && run "$t"
done

fails=$(grep -c 'operand depth analysis failed' "$log" || true)
if [ "$fails" != "0" ]; then
  echo "stack-depth: $fails function(s) rejected by the operand-depth analysis:" >&2
  grep 'operand depth analysis failed' "$log" | sort | uniq -c | sort -rn >&2
  exit 1
fi
echo "stack-depth: analysis accepted every function"
