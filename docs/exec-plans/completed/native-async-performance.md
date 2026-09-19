# Native calls and fulfilled await performance

Status: completed
Last reviewed: 2026-09-18
Owner: theMackabu

## Goal

Recover the remaining Math.min, Array.pop, and fulfilled-promise control
regressions after the exception-record migration. Preserve the 64-bit
native/JIT return ABI, exception ownership, and pending-error promotion.

## Outcome

The candidate skips general number conversion for numeric Math.min arguments,
uses the already-validated dense array pointer throughout pop, and queues a
fulfilled await directly when the promise has no reactions, active trigger,
processing batch, or parent chain. Await still takes a microtask turn and
retains the awaited promise for coroutine ownership and generator state.

Only `src/ant.c` and `src/modules/math.c` changed in runtime code. Native
dispatch, exception records, pending-error checks, and the 64-bit result ABI
remain unchanged. Isolated ABBA/BAAB runs (four samples per binary, same
existing profile) show Math.min -9.72% and pop -12.60% time. A separately
trained final profile has no runtime profile mismatches.

The fulfilled-promise control improves 11.64% at the original 250,000
iterations and 11.00% at 1,000,000 iterations. Fulfilled async iteration
improves 13.57%; async-next cases improve 5–6%; stream cases stay flat.
Direct resume jobs also preserve an intervening microtask between two awaits
of the same eligible promise, matching Node instead of batching both awaits
ahead of that microtask. The final test uses an async rejection handler with
explicit failure exit: it exits 1 on the pinned pre-change binary and passes
on Node and final Ant.

## Final qualification

Pinned installed `15.1.59a2d6b3.1` versus this dirty tree based on `5903d763`,
with a separate fresh profile. Four serial processes per binary for targeted
cases, in ABBA/BAAB order; two per binary for V8, in ABBA order. No builds,
tests, or profilers overlapped timings. Lower time is better.

| Target | Installed | Final | Time change |
| --- | ---: | ---: | ---: |
| Math.min, ns/op | 7.155 | 6.640 | -7.20% |
| Array.pop, ns/op | 18.730 | 17.360 | -7.31% |
| Fulfilled-promise control, ms | 19.269 | 17.300 | -10.22% |
| Control at 4x iterations, ms | 76.226 | 69.456 | -8.88% |

Map iteration is 6.77% faster, regex cases are 1.86–7.10% faster, and Game of
Life is effectively unchanged. V8 Richards, DeltaBlue, Crypto, RayTrace, and
NavierStokes are within 0.7%; EarleyBoyer and RegExp improve about 1%; Splay
improves 28.88%. Array push retains an 8.23% gap. The microbench pass covers
six rows, not all 43; no new full-microbench aggregate is claimed. Installed
build provenance is incomplete, so the final comparison includes build and
profile differences as well as source changes. Small deltas remain noisy.

All 4,240 specs in 102 files, 17 focused JS files, and native error-handoff
and exact-GC-edge tests pass. New coverage checks numeric errors/signed zero,
array fallbacks, await ordering, async-generator requests, queued-value GC
with conservative C-stack roots disabled, cancellation, and reference release.
Preflight, knowledge, and whitespace checks pass. No opcode, interpreter
handler, or compiler emitter changed, so the stack-depth sweep was not repeated.

The checked-in user profile and index were preserved. The main build uses
`pgo_profile` pointing into the artifact directory; all 937 vendor compile
commands remain free of profile-use flags.

Artifacts: `.cache/native-async-recovery-8p8y499t`, including the full report,
raw comparisons, source/assembly snapshots, profile, binary identities, and
validation logs. The initial premature math comparison is marked INVALID and
excluded; `math-valid` is its replacement.
