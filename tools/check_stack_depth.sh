#!/usr/bin/env bash
# Runs the spec suite, the runtime tests and the JIT examples with
# ANT_DEBUG=dump/vm:op-warn and fails if the bytecode compiler's operand-depth
# analysis rejected any function or a JIT compile overflowed its virtual
# stack. Run after touching include/silver/opcode.h, a handler in
# src/silver/ops/, or the loop/try emitters in src/silver/compiler.c.
#
# usage: tools/check_stack_depth.sh [path/to/ant]
# exit:  0 clean, 1 analysis rejected a function or a compile overflowed,
#        2 the binary or a suite runner could not run (coverage incomplete)
set -u
cd "$(dirname "$0")/.."
ANT=${1:-./build/ant}
export ANT_DEBUG=dump/vm:op-warn
log=$(mktemp)
trap 'rm -f "$log"' EXIT

if ! out=$("$ANT" -e 'console.log("probe")' 2>&1) || [ "$out" != "probe" ]; then
  echo "stack-depth: cannot run $ANT" >&2
  printf '%s\n' "$out" >&2
  exit 2
fi

failed_runs=()
run() {
  "$ANT" "$@" 2>>"$log" >/dev/null
  local rc=$?
  if [ $rc -ne 0 ]; then failed_runs+=("$1 (exit $rc)"); fi
  return $rc
}

incomplete=0
run examples/spec/run.js --all || incomplete=1
run examples/jit/run.js --all || incomplete=1
for t in tests/test_*.cjs tests/test_*.mjs; do
  [ -f "$t" ] && run "$t"
done

rejected=$(grep -c 'operand depth analysis failed' "$log" || true)
overflow=$(grep -c 'reason=vstack-overflow' "$log" || true)
status=0
if [ "$rejected" != "0" ] || [ "$overflow" != "0" ]; then
  echo "stack-depth: $rejected function(s) rejected by the analysis, $overflow JIT compile(s) overflowed:" >&2
  grep -E 'operand depth analysis failed|reason=vstack-overflow' "$log" | sort | uniq -c | sort -rn >&2
  status=1
fi
if [ ${#failed_runs[@]} -ne 0 ]; then
  echo "stack-depth: ${#failed_runs[@]} run(s) exited non-zero, coverage may be incomplete:" >&2
  printf '  %s\n' "${failed_runs[@]}" >&2
fi
if [ $incomplete -ne 0 ]; then
  echo "stack-depth: a suite runner failed; coverage incomplete" >&2
  [ $status -eq 0 ] && status=2
fi
[ $status -eq 0 ] && echo "stack-depth: analysis accepted every function, no JIT overflow"
exit $status
