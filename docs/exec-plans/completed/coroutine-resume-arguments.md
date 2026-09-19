# Coroutine resume arguments

Status: completed
Last reviewed: 2026-09-19
Owner: theMackabu

## Goal

Pass the resume value and error flag through the synchronous resume helpers
instead of retaining them in each coroutine. Preserve the existing queued-job
identity checks, GC lifetime, cancellation behavior, and 64-bit return ABI.

## Ownership

Direct resume dispatch already pins its value across the call. Promise
reactions keep their source promise and settlement value rooted while they
process. Neither path needs another root in the shared helpers. Legacy native
resume wrappers explicitly root their borrowed argument at their own boundary.
The VM resume slot is transient transport, not an independently traced GC root.

## Validation

Removed the fields and their initializers/GC edge, and deleted the temporary
settlement helper and single-element argument array. Tests now use real
suspended continuations and observable promise results, covering fulfillment,
rejection, cancellation/replacement, and minor/major GC with C-stack roots
disabled. The test continuation includes work after await so the compiler
cannot optimize it into promise adoption without a suspended activation.

Validation on a separate debugoptimized build with PGO and LTO disabled:

- All 4,240 specs in 102 files pass.
- Twelve focused async, generator, exception, and escaped-arguments tests pass.
- Native error-handoff and exact-GC-edge tests pass.
- Preflight, knowledge, and whitespace checks pass.

The separate build replaces the recommended main-tree build/test commands to
avoid disrupting concurrent work. No opcode or interpreter handler changed;
the change in `ops/async.h` only removes two initializer fields, so the operand
depth sweep was not repeated. The main build, its profile configuration, and
the native/JIT 64-bit return ABI are unchanged.

Base revision: `0e9b2a639445e1ae53e5e15301352f2ad0d61c0f`. Local logs and the
test binary are under `.cache/coroutine-resume-0k7jc2k7/`.

## Release benchmark follow-up

Compared the exact pre-refactor revision above with the staged refactor using
isolated release/O3/LTO builds. Both use one fresh profile merged from identical
training workloads on both variants; common functions share counts and changed
control-flow hashes retain their own records. Neither final runtime build
discards mismatched profile counts. The existing profile was stale even for
the baseline VM and was excluded from the final comparison.

Serial ABBA/BAAB runs, including a second block for the slower cases, produced:

| Case | Before, ms | After, ms | Time change |
| --- | ---: | ---: | ---: |
| Suspended async entry | 44.207 | 46.805 | +5.88% |
| Tight fulfilled-promise await loop | 46.895 | 48.745 | +3.94% |
| Fulfilled-promise control | 18.194 | 18.518 | +1.78% |
| Control at 4x iterations | 72.683 | 73.831 | +1.58% |
| Generator churn | 68.356 | 68.397 | +0.06% |

The repeated async rows have six processes per binary; generator churn has
four. No-await entry is flat, and dead-await entry is about 1.9% faster.
The coroutine struct shrinks from 144 to 136 bytes. Assembly shows the resume
helper spilling the incoming value/error flag and growing its stack frame
from 192 to 208 bytes, a plausible contributor to the regressions; the exact
timing attribution is not isolated. Direct-job GC rooting remains unchanged.

Broad controls include the six selected microbench rows, Map/array iteration,
fixed-work Game of Life, and all eight V8 cases. Most are close, but UTF-16
regex is 3.35% slower after repetition. Splay's median score is 9.51% higher
with a highly variable baseline; no uniform Splay gain or RSS reduction is
claimed. The full 43-row microbench was not run.

The optimized candidate passes all 4,240 specs, seven focused files, and the
two native GC/error-handoff tests. Main build/profile and staged changes were
preserved. Full results, binary/profile identities, immutable source snapshots,
and raw runs are in `.cache/coroutine-bench-xz26tztb/`.

The subsequent [performance recovery](coroutine-resume-performance.md) removes
the redundant ownership work and records the final matched-profile results.
