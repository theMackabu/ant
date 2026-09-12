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

### In-loop promotion (the last follow-up)

A once-called function that never leaves its OSR'd cold loop used to stay
on level-1 code for the whole call. Cold-tier code now promotes itself:
`jit_emit_promote_check` (`emit_control.c`) puts a countdown on the
back-edges of *outermost* loops only, so inner loops pay nothing per
iteration; a lone loop checks every `JIT_COLD_PROMOTE_CHECK_EVERY` (4096)
turns, an outer loop that contains loops on every turn, since each turn is
long. At a check, `jit_helper_promote_due` banks the cold time since the
last check into an 8-byte slot in the cold code's own MIR module (three
words: banked time, budget, last check) and compares it with the budget.
Banking on the code rather than the frame lets a kernel called repeatedly
for sub-budget runs promote too, without charging idle time between calls,
and keeps `sizeof(sv_func_t)` at 208. When the budget is spent the code
takes a second resume trampoline into `jit_helper_promote_resume`, which
unpublishes the cold code, leaves `jit_code_cold` set as the "next compile
is hot" mark (`sv_jit_promote_pending`), primes `back_edge_count` and
resumes the interpreter at the jump; the re-taken back-edge OSR-compiles
`SV_JIT_TIER_HOT` and enters it. Not a deopt: no bailout count, no
recompile delay, and the hot recompile is exempt from the "no new
feedback" refusal.

The budget is the cost model, measured rather than estimated. Two earlier
versions were wrong in instructive ways:

- A per-byte estimate of the hot compile (50 us x `code_len`) was off by
  up to 4x: compile time scales with locals (level-3 register allocation),
  not bytes. Measured, cold vs hot: N-body 592 B / 17 locals 22 vs 66 ms;
  a 667 B / 62 locals for-of shape 42 vs 115 ms. Every promoted run paid a
  fixed ~100 ms it never recouped.
- The assumed 1.2x hot gain is the best case. Long-run hot-by-call versus
  cold-only: for-of 1 to 4%, integer `%` 3 to 5%, `Math.abs` 8%, sqrt 12%,
  closure calls 20%, N-body 20%.

So the cold compile's own measured duration is written into the slot as
the budget times `JIT_COLD_PROMOTE_COMPILE_MULTIPLE` (20) times
`JIT_HOT_COMPILE_COLD_RATIO` (3, measured 2.7 to 3.2): 60 cold-compile
times, about 1.3 s of cold code for the kernel and 2.5 s for the 60-local
shapes. Promotion is a safety net for loops that have already run seconds:
its worst case is one hot compile, a few percent of elapsed, and its upside
is 5 to 20% of everything after. Measured on the kernel at 2000 steps:
2.6 s cold-only, 2.4 s promoted at 1.6 s. A per-back-edge check on inner
loops cost 8% on the tightest for-of body, hence outermost-only.

A gate that skipped promotion for "helper-bound" loops was tried and
reverted: its premise (hot == cold on such loops) did not survive
controlled measurement, and it declined the for-of shape where hot was
faster.

## Validation status

- `tests/test_jit_osr_large_function.cjs` (new): once-called >512-byte body
  is OSR-compiled on the cold tier and re-tiered hot after 150 calls, also
  when every call comes from compiled code, and promoted from inside a
  single long loop, with checksums matching node.
- `tests/test_jit_vstack_depth.cjs` (new): 120-operand calls, literals and
  expressions compile and run correctly through the JIT.
- `tests/test_labeled_jump_for_of_close.cjs` (new): labeled break/continue
  across for-of and for-await-of close the crossed iterators in spec order.
- `tools/check_stack_depth.sh`: every function in the spec suite, `tests/`
  and the JIT examples accepted by the analysis.
- `tests/test_jit_*.cjs`: 70 pass (plus the two `.mjs` JIT tests).
- `examples/jit/run.js --all`: pass.
- `examples/spec/run.js --all`: see checkpoint below.

## Two cliffs the probes exposed (fixed)

Both reproduced on the installed release, so neither came from this plan;
the tiering probes only made them visible.

**OSR entry rejected by locals declared after the loop.** The OSR entry
guards every local the inference marks numeric before resuming the loop. A
`var s = 0` (or `let`) written after a hot loop is hoisted, so at the loop
head it still holds `undefined` (var) or the TDZ mark (let). The guard failed,
the generated code returned the retry sentinel, and the interpreter ran the
whole loop while retrying the entry every `jit_osr_threshold` back-edges.
The guard now accepts both values for a numeric local that has no
entry-integer register, which is exactly the state a normal entry starts
from: the local's immediate init runs before any read, and a read that would
need a TDZ check makes the function ineligible. `sv_jit_try_osr` logs
`jit: osr entry-rejected func=... offset=...` under op-warn when an entry is
refused, so the shape is now diagnosable.

| 12M-iteration double loop, entered by OSR | before | after | node |
|---|---|---|---|
| `var s = 0` after the loop | 236 ms | 20 ms | 16 ms |
| `let s = 0` after the loop | 208 ms | 25 ms | 16 ms |

Regression test: `tests/test_jit_osr_late_locals.cjs`.

**Integer `%` was an unconditional helper call.** `OP_MOD` flushed the
vstack and called `jit_helper_mod` for every operand pair. `fmod` itself
costs ~12 ns per call on this machine, so a direct `fmod` fast path would
have bought little. `%` now follows the divide emission: type feedback picks
the shape, and two doubles take an inline integer remainder when the
dividend is an exact non-negative integer and the divisor a non-zero exact
integer (the only case where the integer result matches JS, including the
sign of zero; `-0 % n` is sent to fmod). Everything else numeric goes to the
helper's `fmod`; non-numbers bail as before. Two known-range integer slots
take a bare `MIR_MOD` in `jit_emit_integer_arithmetic`.

| 12M iterations of `(s + i * 7) % 1000003` | before | after | node |
|---|---|---|---|
| | 185 ms | 70 ms | 35 ms |

Regression test: `tests/test_jit_mod.cjs` (bit-exact against the interpreter
with `Object.is`, and no bailouts on the numeric shapes).

**JIT open-upvalue list swept under a live frame (hang).** Found through
the PGO training run: `tests/bench_includes_breakdown.cjs` printed all its
results and then never exited on every build since #102, while the stable
build exits. The module's top level (536 B) is now OSR-compiled on the cold
tier, so its closures are created by JIT code and their upvalues live in the
frame's private open list, whose head is a native stack slot. The collector
reaches that list only through the conservative stack scan in
`src/gc/objects.c`, and that chain walk stopped at the first node already
marked, which happens whenever the head's closure is still alive. Every node
behind it was swept while still linked; the next capture got a swept node
back from the arena and linked it into a list that still pointed at it, a
two-node cycle, and closing the frame's upvalues spun on it forever. The walk
now marks every node and is bounded by the arena element count, and every
close path clears `next` on the node it unlinks. The old 512-byte gate had
kept large bodies with captures out of the JIT's OSR path, which is why the
sweep never lined up before.

Regression test: `tests/test_jit_open_upvalue_gc.cjs` (hangs the unfixed
binary, passes with the fix).

**JIT compile memory (RSS step after a hot compile).** A report measured
35 → 57 MiB RSS across the promotion of a 541-byte function. Reproduced on
the N-body kernel: 9 MiB peak on the stable build, which never compiles it,
33 MiB after the cold and hot compiles. This is not new cost, it is new
reach: the stable build peaks at 41 MiB compiling the same function on the
call path, and every call-compiled large function has always paid it. Since
#102, once-called functions that only get hot through their loop pay it too.
Per-zone heap inspection split the retained memory into three pools:

- MIR's generator tables (live ranges, SSA, register allocator free lists),
  ~10 MB live, sized for the largest function seen and never shrunk.
  `jit_release_gen_scratch` tears the generator down and re-creates it after
  every compile. The teardown costs what the compile allocated (0.8–1.4 ms
  after the 15–20 ms kernel compiles, 0.01 ms after a tiny one) and the
  re-init about 0.03 ms, so tiny compiles are unaffected.
- The MIR IR of each compiled function, ~2 MB per 600-byte body, kept
  forever although never read after `MIR_gen`. Dropped with
  `MIR_remove_insn` right after generation.
- Freed-but-dirty allocator pages, ~17 MiB, from MIR's churn interleaved
  with Ant's long-lived objects. MIR now allocates from its own malloc zone
  (`MIR_init2` with a `MIR_alloc_t`), so the churn stays out of Ant's heap.
  On glibc `malloc_trim` returns the pages after a compile. macOS libmalloc
  ignores `malloc_zone_pressure_relief` (measured: 0 bytes, and a destroyed
  zone still keeps ~10 MiB of regions cached), so there the resident number
  follows only as far as its own large-span release goes.

| | before | after |
|---|---|---|
| live malloc after cold+hot compile of the kernel | 14.4 MB | 1.0 MB |
| peak RSS, kernel cold+hot | 34.4 MiB | 31.4 MiB |
| peak RSS, 30 functions × 3 loops | 35.4 MiB | 21.2 MiB |
| peak RSS, 30 functions × 12 loops | 49.2 MiB | 27.3 MiB |
| live malloc held after the kernel compiles, no reset / reset | 13.3 MB | 3.9 MB |
| total compile time, 30 × 12 loops (3 interleaved rounds) | 264–300 ms | 288–306 ms |
| cheap-tier compile (6-byte body) | 0.1 ms | 0.1 ms |

The peak-RSS win is the zone plus the IR drop; the reset takes the held
live set from ~10 MB to 3.9 MB (what remains without it is VARR and bitmap
capacity sized for the largest function).

**Generator node arena (MIR fork, `mir-gen.c`).** The reset's teardown used
to cost 0.8–1.4 ms after the kernel and 73 ms after a 12.9 KB body: the
generator allocated every bb, bb_insn, edge, SSA edge, live range, dead var
and GVN expr with malloc (5.2 M blocks for that body), parked them on free
lists across functions, and `MIR_gen_finish` freed 3.4 M of them one at a
time. Those node types now come from a per-function arena: 1 MiB bump
chunks with per-size free lists so mid-function frees still recycle, the
three cross-function free lists reset with it, and the chunks released in
bulk after each function's code is published (the oldest chunk is kept).
Context-level structures and the lazy bb-version path stay on malloc. A
scratch-zone design on Ant's side was ruled out first: about one 16-byte
block per insn allocated during link is persistent (temporary register names
interned in the context string table), so nothing outside MIR can discard
the generator's allocations wholesale.

| compile | before arena | with arena |
|---|---|---|
| kernel cold+hot | 30.5–32.2 ms | 27.0–27.3 ms |
| 2.7 KB body, hot | 34–36 ms | 28 ms |
| 6.1 KB body, hot | 139–142 ms | 116–118 ms |
| 12.9 KB body, hot | 645–652 ms | 574–575 ms |
| 30 functions × 12 loops | 292 ms | 208–218 ms |

Every compile is now faster than the pre-change binary with no reset at all;
peak RSS is unchanged (+1 MiB for the retained chunk per context). The same
change is in `~/Developer/mir` (`mir-gen.c`), byte-identical to the vendored
copy.

Seen on the way, not fixed: a numeric local read inside the loop before its
post-loop declaration (`t = s` before `var s = 0`) returns register garbage
from JIT code on a normal entry as well; the interpreter returns `undefined`.
It predates this plan and is independent of the OSR entry change.

## Follow-ups

- `Math.sqrt` and the other `Math.*` intrinsics as JIT fast paths.
- Typed-array element fast path in `src/jit/emit_properties.c`.
- MIR level-3 compile time is superlinear in body size (262 ms for 5 KB);
  worth profiling before raising `JIT_OSR_COLD_COMPILE_MIN_BYTES`.
- Abandoned generators (fixed 2026-09-12): a finished generator's coroutine
  and captured activation were only *retired* while any JS was running,
  since a resume may still hold the pointer after releasing it, and reaped
  at the next event loop turn; a synchronous loop therefore kept all of them
  (~375 B each, 757 MiB for 2M against 57 MiB in node). The retired list is
  gone: a release at refcount zero destroys the coroutine on the spot. Two
  things made that possible. The generator resume now holds its own
  reference across the call, as the async resume already did, so no caller
  keeps a raw pointer past a release. And `sv_activation_seal` is
  collection-aware: during a collection, after marking, it leaves upvalues
  the sweep is about to free alone (`gc_upvalue_is_live`) and skips the
  write barrier, so a finalizer can destroy a coroutine inside the sweep.
  2M abandoned generators: 15 MiB. Test: `tests/test_generator_abandon_memory.cjs`.
- Captured activations invisible to minor collections (fixed 2026-09-12):
  a suspended coroutine's activation holds locals and open upvalues; it is
  scanned when its owner is marked, which a minor collection does not do
  for an old owner, so anything young in an old generator's activation was
  freed under it and the next resume or seal read a freed upvalue. Stable
  segfaults at address 0 on 20k such generators. Every capture now puts the
  coroutine in a remembered set (`gc_remember_coroutine`) that minor
  collections scan, cleared each cycle, left on destruction.
  Test: `tests/test_generator_open_upvalue_gc.cjs`.
- The committed PGO profile no longer matches seven changed functions
  (`sv_execute_frame`, `sv_stage_frame_args` callers, `vstack_push`,
  `compile_for_each`, ...); the release flow should re-profile. An
  interleaved A/B against the installed release showed no difference beyond
  run-to-run noise on the kernels or bench-v8.
- Pre-existing failures on the release binary too: `test_hono_adapter`,
  `test_throw_stack`, `test_wasm_exported_memory_grow`, `test_with_strict`.
