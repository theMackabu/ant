# Silver Recursive JIT Performance

Status: active
Last reviewed: 2026-06-20
Owner: theMackabu

## Goal

Close the gap on simple hot recursive numeric functions such as:

```js
function fib(num) {
  if (num < 2) return num;
  return fib(num - 1) + fib(num - 2);
}
```

Current measured behavior on the rebuilt local tree:

- `node -e ... fib(40)`: about 0.60s
- `./build/ant -e ... fib(40)`: about 1.71s

The target is to reduce Ant's overhead for this workload without weakening
general JavaScript call semantics or using a benchmark-specific shortcut.

## Current Findings

- The slowdown is not process startup; the same gap is visible inside the timed
  `fib(40)` body.
- The slowdown is not a steady interpreter fallback; `ANT_DEBUG='dump/vm:op-warn'`
  reports no JIT bailouts for the repro.
- The generated MIR self-recurses through `jit_fib_*`, but each recursive edge
  still uses the normal boxed JIT function ABI.
- The generated function still emits entry stack-overflow helper checks, boxed
  argument scratch storage, boxed numeric conversions, and full recursive
  `jit_proto` calls.
- `args_buf` is currently allocated from a coarse scratch size. For this
  one-argument function, the dump still shows `alloca args_buf, 512` because
  compiled functions use `max_stack = max_locals + 64`.

## Scope

- `src/silver/swarm.c`
- `src/silver/compiler.c`
- `include/silver/engine.h`
- Focused runtime tests or benchmarks under `tests/`

Avoid changing JavaScript semantics, broad call handling, or unrelated JIT
features while pursuing this plan.

## Task List

1. Add a focused benchmark/regression fixture for hot recursive numeric calls.
   - Include the `fib(40)` repro or a smaller deterministic variant suitable
     for local runs.
   - Record both whole-process and in-process timings in notes, but avoid
     fragile wall-clock assertions in normal tests.

2. Tighten function stack/scratch sizing.
   - Replace the coarse `max_stack = max_locals + 64` accounting with real
     compiler stack-depth tracking.
   - Make JIT `args_buf` allocation use the actual maximum scratch need for
     call/bailout paths, not the inflated VM stack budget.
   - Verify bytecode that needs larger temporary depth still reports enough
     stack capacity.

3. Add a lightweight JIT self-call path for non-tail recursive calls.
   - Keep current behavior for bound functions, `super`, async/generator
     functions, `new.target`, exception handling, and captured locals.
   - For proven same-closure self calls, avoid unnecessary function/closure
     resolution and minimize argument packing.
   - Preserve bailout and error propagation behavior.

4. Reduce per-entry recursive stack-overflow overhead.
   - Check whether the JIT can use cheaper depth accounting or periodic checks
     while preserving `RangeError: Maximum call stack size exceeded`.
   - Compare behavior against Node for normal and excessive recursion depth.

5. Investigate numeric-specialized hot function entry/results.
   - Use existing type feedback to identify number-only parameters and returns.
   - Consider a specialized internal entry for monomorphic numeric self calls
     that passes unboxed doubles where semantics allow it.
   - Keep boxed ABI as the deopt/bailout-compatible general path.

6. Validate incrementally after each optimization.
   - Rebuild with `meson compile -C build`.
   - Run the focused recursive benchmark and the relevant JIT tests.
   - Run `maid preflight` and follow its recommended validation before landing.
   - For semantics-sensitive changes, run `./build/ant examples/spec/run.js --all`
     or document why a narrower spec scope is sufficient.

## Decision Log

- 2026-06-20: Initial investigation found the hot function stays JITted and
  self-recurses directly, so the first optimization target is call ABI overhead,
  not interpreter dispatch.
- 2026-06-20: Do not solve this by disabling JIT paths or special-casing
  Fibonacci. The useful fix should improve hot recursive and numeric call
  workloads generally.

## Validation Status

- Rebuilt current configured tree with `meson compile -C build`.
- Reproduced `fib(40)` timing gap after rebuild: Node about 0.60s, Ant about
  1.71s.
- Confirmed no JIT bailout warnings for the repro.
- Confirmed generated MIR includes direct recursive `jit_fib_*` calls plus the
  full boxed call ABI shape.

## Follow-Ups

- Decide whether stack-depth tracking belongs entirely in the compiler pass or
  whether bytecode emission should maintain a max-depth counter as it emits.
- After a self-call fast path exists, re-sample `fib(42)` to confirm samples move
  out of recursive call setup rather than into a new helper bottleneck.
- If numeric specialization is pursued, add deopt tests for mixed numeric/string
  calls after warmup.
