# OSR Size-Scaled Tiering

Status: active
Last reviewed: 2026-09-11
Owner: theMackabu

## Goal

Fix [issue #101](https://github.com/theMackabu/ant/issues/101): a once-called
function with a hot loop ran ~30x slower than node because it never left the
interpreter. Replace the hard OSR size gate with a size-scaled threshold, stop
the gate from re-firing, and bound the synchronous compile pause for large
bodies with a cheap first tier.

## Evidence

The N-body kernel in the issue compiles to 592 bytes of bytecode. Before this
plan `sv_jit_try_osr` refused any function over `JIT_OSR_MAX_CODE_BYTES`
(512) and instead bumped `call_count` so the *next call* would compile. The
benchmark calls the function once, so the whole run stayed interpreted.

Why it re-fired: the refusal reset `back_edge_count` to 0 and returned
`SV_JIT_RETRY_INTERP`, but nothing recorded the deferral, so the interpreter's
`JIT_OSR_BACK_EDGE` macro re-entered `sv_jit_try_osr` every 500 back-edges.
On the 2.7M-iteration kernel that was 5437 refusals per run.

Measured on arm64 macOS, release build, `Date.now()` around the kernel:

| configuration | cold call | 2nd call | 3rd call |
|---|---|---|---|
| stock (512-byte gate) | 380 ms | 89 ms | 42 ms |
| gate raised to 1024, hot compile | 75 ms | 33 ms | 33 ms |
| this plan (cold OSR tier, hot re-tier after 100 calls) | 50 ms | 39 ms | 32 ms after tier-up |
| node v22 | 11 ms | | |

The remaining gap to node is not tiering: `Math.sqrt` has no JIT intrinsic
(generic global + field + method call, about half of steady state) and typed
arrays have no JIT element fast path at all.

## How V8 and JSC handle it

Neither engine has an OSR-specific size limit.

- V8 (`src/execution/tiering-manager.cc`, `src/flags/flag-definitions.h`):
  the only size caps are global "may this ever be optimized" ceilings, 60 KB
  for TurboFan and 512 KB for Maglev. The interrupt budget that drives OSR
  urgency is `invocation_count_for_osr (500) x bytecode_length`, so bigger
  bodies wait proportionally longer but always get there. OSR compiles run
  concurrently and the interpreter enters the code at the next `JumpLoop`.
- JSC (`runtime/OptionsList.h`, `bytecode/CodeBlock.cpp`): global ceilings of
  100,000 bytecode cost for DFG and 60,000 for FTL. Size changes *when*, not
  *whether*: `CodeBlock::optimizationThresholdScalingFactor` multiplies the
  base threshold by `0.826 + 0.0615 * sqrt(cost + 1.02)`. Compiles run on a
  background worklist; baseline keeps looping and OSR-enters when ready.

Ant compiles synchronously, so the cheap-tier-first step below stands in for
the background compile both engines rely on.

## Design

1. `sv_func_t.jit_osr_threshold` is computed once per function in
   `sv_func_init_code_and_map_templates` by `sv_jit_osr_threshold_for()` and
   the back-edge macro compares against it instead of the constant. The
   curve is V8's linear budget: `SV_JIT_OSR_THRESHOLD x code_len /
   JIT_OSR_THRESHOLD_SCALE_BYTES` (500 per 512 bytes, floor 500). The gate
   in `sv_jit_try_osr` is gone, which also removes the re-fire: the single
   remaining "defer" outcome is a compile failure, which sets
   `jit_compile_failed` and stops the macro.
2. One absolute ceiling, `SV_JIT_MAX_CODE_BYTES` (64 KiB), lives in
   `jit_is_eligible` so the OSR and call paths agree.
3. OSR compiles of bodies over `JIT_OSR_COLD_COMPILE_MIN_BYTES` (512) use the
   level-1 MIR context (`SV_JIT_TIER_COLD`) and set `jit_code_cold`. Both
   call-path entry points call `sv_jit_maybe_tier_up()`, which recompiles on
   the hot context (`SV_JIT_TIER_HOT`) once `call_count` passes
   `SV_JIT_THRESHOLD`. Call-path compiles that happen to use the cheap
   context are *not* marked cold; an earlier draft did, and every such
   function re-tiered every 100 calls forever (Richards fell from 5775 to
   183).

Compile cost that motivated step 3 (MIR, this machine):

| function | bytecode | level 3 compile | level 1 compile | level-1 code vs level-3 |
|---|---|---|---|---|
| N-body kernel | 592 B | 30 ms | 11 ms | ~1.2x slower |
| synthetic 5 KB body | ~5 KB | 262 ms | 74 ms | not measured |

## Curve choice

Both reference curves were implemented behind a temporary env switch and
compared on the issue kernel, a 5 KB synthetic body, `examples/jit/osr.js`
and `examples/bench-v8` (three interleaved rounds).

run A, config order flat, v8, jsc, v8/cold, jsc/cold (3 rounds):

| config | Richards | DeltaBlue | Crypto | RayTrace | EarleyBoyer | RegExp | Splay | NavierStokes | geomean |
|---|---|---|---|---|---|---|---|---|---|
| flat/-1 | 6197 | 6716 | 12022 | 13108 | 11806 | 4704 | 7184 | 23813 | 9426 |
| v8/-1 | 6309 | 6795 | 12052 | 12715 | 11721 | 4645 | 7122 | 23605 | 9384 |
| jsc/-1 | 6132 | 6647 | 11944 | 13117 | 11636 | 4625 | 6888 | 23886 | 9312 |
| v8/512 | 6212 | 6678 | 11984 | 13278 | 11430 | 4520 | 6305 | 22174 | 9116 |
| jsc/512 | 5772 | 5867 | 10153 | 10695 | 8882 | 4109 | 6159 | 23201 | 8135 |

run B, config order jsc/cold, v8/cold, flat, jsc/cold2048 (2 rounds):

| config | Richards | DeltaBlue | Crypto | RayTrace | EarleyBoyer | RegExp | Splay | NavierStokes | geomean |
|---|---|---|---|---|---|---|---|---|---|
| jsc/512 | 6232 | 6827 | 12154 | 13176 | 11931 | 4802 | 7330 | 24153 | 9549 |
| v8/512 | 6250 | 6926 | 12148 | 13146 | 11937 | 4716 | 7400 | 23366 | 9517 |
| flat/-1 | 6082 | 6536 | 11805 | 12425 | 9120 | 4028 | 5668 | 20790 | 8419 |
| jsc/2048 | 5400 | 6625 | 11780 | 13041 | 11456 | 4706 | 7314 | 24253 | 9228 |

Config keys: curve/cold-tier cutoff in bytes, `-1` = always hot compile,
`flat` = constant 500 threshold with the gate removed. Committed
`score.json` baseline before this plan: geomean 8673 (Richards 5775).

Reading: every config is inside the 5-10% run-to-run spread (RegExp, which
has no OSR-sensitive loop, moves ~2% between configs). One run in each
batch lost ~40% across several workloads; in run A it landed on jsc/512,
in run B on flat/-1, so it is machine noise rather than a configuration
effect. The v8 linear form was chosen on structural grounds: integer-only,
matches the back-edge budget Ant already has, and more conservative for
very large bodies where the synchronous compile pause is the real risk.

Cold-tier usage on bench-v8 at the 512-byte cutoff (`ANT_DEBUG=dump/vm:op-warn`):
Crypto 3 cold OSR compiles / 2 re-tiers, EarleyBoyer 2 / 2, RegExp 5 / 5,
NavierStokes 1 / 0 (its once-called driver stays on level-1 code, no
measurable score change), Richards, DeltaBlue, RayTrace, Splay 0.


## Validation status

- `tests/test_jit_osr_large_function.cjs` (new): once-called >512-byte body
  is OSR-compiled on the cold tier and re-tiered hot after 150 calls, with
  checksums matching node.
- `tests/test_jit_*.cjs`: 70 pass (plus the two `.mjs` JIT tests).
- `examples/jit/run.js --all`: pass.
- `examples/spec/run.js --all`: see checkpoint below.

## Follow-ups

- `Math.sqrt` and the other `Math.*` intrinsics as JIT fast paths.
- Typed-array element fast path in `src/jit/emit_properties.c`.
- MIR level-3 compile time is superlinear in body size (262 ms for 5 KB);
  worth profiling before raising `JIT_OSR_COLD_COMPILE_MIN_BYTES`.
- Tier-up only fires from the call path. A once-called function that stays in
  its OSR'd cold loop never re-tiers; V8 solves this with OSR-from-Maglev.
