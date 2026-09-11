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
   level-1 MIR context (`SV_JIT_TIER_COLD`) and set `jit_code_cold`. The
   cold code promotes itself: `jit_setup_frame` emits a prologue that bumps
   `call_count` on every entry and, past `SV_JIT_THRESHOLD`, calls
   `jit_helper_tier_up` and tail-calls the hot entry it returns. The first
   cut hooked the interpreter's two call paths instead, which missed direct
   calls from compiled code entirely (post-merge review of #102: a compiled
   driver called the cold kernel 200 times without promotion). Call-path
   compiles that happen to use the cheap context are *not* marked cold; an
   earlier draft did, and every such function re-tiered every 100 calls
   forever (Richards fell from 5775 to 183).
   `sv_jit_osr_threshold_for` is a static inline in `silver/jit.h` because
   the bytecode compiler calls it on builds without the native JIT
   (`packages/wasm` links `jit_stub.c`; the first cut broke that build).

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


## Operand-stack depth (`max_stack`) made real

Reviewing `sv_func_t` for dead fields showed `max_stack` was always
`max_locals + 64`: a guess, not a bound. The JIT sized its virtual stack
from it with no bounds check in `vstack_push`, so any expression holding
more than 64 pending operands (a 120-argument call, a 120-element literal)
wrote past the arrays `setup_frame.c` allocates: MIR reported an undeclared
register and guard-malloc crashed. The interpreter had the same hole in
principle, since its frame reservation only covered args and locals.

Design, mirroring QuickJS's `compute_stack_size`:

1. The `n_pop` / `n_push` columns of `OP_DEF` in `include/silver/opcode.h`
   are now authoritative and consumed. `sv_op_stack_effect()` (compiler.c)
   returns an op's effect from the table plus its count operand, which the
   format column declares: `npop` (u16 at +1), `u8_npop` (u16 at +2),
   `npop_u8_u8` (two u8 counts), `map_template` (descriptor count). Rows
   that had drifted were corrected: `CATCH` (the unwinder pushes the caught
   value), `FINALLY_RET`, `COPY_DATA_PROPS`, `AWAIT_ITER_NEXT`, the `YIELD`
   family, and the `APPLY` family plus `CALL_STRING_INTRINSIC`,
   `CALL_STABLE_BUILTIN` and `CALL_CALL`, whose counts were not declared.
2. `sv_func_compute_max_stack()` runs once when bytecode is finalized: a
   worklist over offsets that processes each at its highest incoming depth.
   It adds only control flow: label targets, the terminal set, handler entry
   at the saved depth plus one, and no edge for `CATCH`'s bookkeeping label.
   A loop head re-entered deeper than first seen, an underflow, or a bad
   jump fails the analysis; release builds then fall back to
   `code_len + 64`, verification builds exit.
3. `sv_stage_frame_args` reserves `max_stack` on top of args and locals
   without moving `sp`; the JIT sizes `vs.max` as `max_stack +
   JIT_VSTACK_SLACK` and `vstack_push` sets an overflow flag that abandons
   the compile instead of writing out of bounds.
4. `tools/check_stack_depth.sh` runs the spec suite, `tests/` and the JIT
   examples under `ANT_DEBUG=dump/vm:op-warn` and fails if the analysis
   rejects any function. During this work a throwaway in-tree verification
   build also compared every dispatched op's real `sp` delta with its table
   row; it was removed once the table was correct, since a drifted row
   shows up as an analysis rejection anyway. (`AWAIT_ITER_NEXT` re-executes
   after a resume with the resume value already pushed; its row lists the
   combined effect.)

The verification pass ran the spec suite (102 files), all `tests/`, the
JIT examples and bench-v8 with zero diagnostics. It found one real
compiler bug on the way:

**Labeled jumps across `for...of` leaked and skipped IteratorClose.**
`emit_loop_exit_jump` popped the handler entries of crossed loops but never
closed a crossed for-of iterator or dropped its three stack slots. So
`continue outer` from an inner for-of leaked three slots per iteration,
never called the inner iterator's `return()`, produced extra iterations
once the VM stack grew, and hung the release binary on a generator inner
loop; `break outer` landed on the outer loop's close sequence with the inner
triple on top and closed the wrong iterator. The emitter now retires unwind
entries one by one, innermost first: pop a try/catch or for-of handler and
close that loop's iterator at its exact position, discard a finally body we
are inside, and run a try/finally's block right there through a
one-handler `UNWIND_JMP` landing on the next instruction. That keeps spec
order (an inner iterator closes before an outer finally runs), which node
confirms. Regression: `tests/test_labeled_jump_for_of_close.cjs`.

`sizeof(sv_func_t)` is 208 (was 200): `jit_osr_threshold` plus one
bitfield spill. `max_stack` stays because it now carries real information.

### Review fix: unreachable bytecode

Review of the first cut found that the analysis only walked reachable
code while the JIT and the inliner walk bytecode linearly, resetting their
virtual stack only at branch targets. A deep expression after `return`
therefore exceeded the inliner's arrays and crashed with `undeclared reg`.
The pass now has two phases: the reachable worklist (exact, underflow is an
error) and a linear sweep in which unreachable ops inherit the depth of the
op before them and never fail the analysis. The release build also reports
`jit: compiled` and `jit: compile-failed ... reason=vstack-overflow` on the
op-warn channel, `tools/check_stack_depth.sh` fails on either an analysis
rejection or an overflow and refuses to report success for a binary it
cannot run, and `tests/test_jit_vstack_depth.cjs` asserts that each
JIT-eligible case actually compiled.

## Validation status

- `tests/test_jit_osr_large_function.cjs` (new): once-called >512-byte body
  is OSR-compiled on the cold tier and re-tiered hot after 150 calls, with
  checksums matching node.
- `tests/test_jit_vstack_depth.cjs` (new): 120-operand calls, literals and
  expressions compile and run correctly through the JIT.
- `tests/test_labeled_jump_for_of_close.cjs` (new): labeled break/continue
  across for-of and for-await-of close the crossed iterators in spec order.
- `tools/check_stack_depth.sh`: every function in the spec suite, `tests/`
  and the JIT examples accepted by the analysis.
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
- Ant closes abandoned generators only through GC pressure (~850 MB RSS for
  2M short generator loops vs 58 MB in node); unrelated to this plan but
  visible in its probes.
- The committed PGO profile no longer matches seven changed functions
  (`sv_execute_frame`, `sv_stage_frame_args` callers, `vstack_push`,
  `compile_for_each`, ...); the release flow should re-profile. An
  interleaved A/B against the installed release showed no difference beyond
  run-to-run noise on the kernels or bench-v8.
- Pre-existing failures on the release binary too: `test_hono_adapter`,
  `test_throw_stack`, `test_wasm_exported_memory_grow`, `test_with_strict`.
