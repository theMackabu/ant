# JIT PUT_FIELD GC Performance

Status: completed
Owner: theMackabu
Started: 2026-07-13

## Problem

The direct Silver JIT `PUT_FIELD` fast path requires a generational GC write
barrier. The correctness fix in `0f5e061c` coincides with a roughly 41% RSC
throughput regression and substantially more major-GC sampling. That commit
also contains an IC transition OOM-safety reorder and an HTTP header behavior
change, so the barrier cost has not yet been isolated.

## Constraints

- Do not weaken or remove the old-to-young write barrier.
- Preserve the capacity-before-shape transition ordering.
- Benchmark equivalent release builds against the same RSC endpoint.
- Keep diagnostic overhead disabled unless explicitly requested.

## Plan

1. Add opt-in aggregate GC telemetry for minor/major frequency, survival,
   promotion, remembered-set pressure, controller state, and pause time.
2. Starting from `bdc5d49f`, benchmark cumulative variants containing only:
   the barrier changes; then transition safety; then header case folding.
3. Use telemetry to select the smallest collector-policy adjustment.
4. Validate the deterministic JIT barrier invariant, focused GC tests, and RSC
   throughput before retaining a policy change.

## Decision Log

- The barrier is correctness-critical. A forced-minor diagnostic showed the
  pre-fix JIT store leaves an old writer out of the remembered set, while
  `0f5e061c` records it.
- `bdc5d49f` is useful only for isolating performance; it is not a valid
  shippable baseline because its direct stores can lose live young values.
- Ten-second variant runs were phase-sensitive because conservative stack
  roots change retention, but 30-second runs showed the broken and corrected
  barrier variants converging to the same major-GC bottleneck.
- The allocation-driven 50 ms fallback caused full major collections without
  live or pool pressure. It cannot collect during true idle because
  `gc_maybe()` is allocation-driven, so it was removed.
- Live-pressure collection now runs a minor first while young objects exist.
  This reduced an allocation-heavy benchmark from about 1,100 major
  collections to 28 and improved its runtime by roughly one third.
- The nursery starts at 32K objects and grows to at most 64K only under
  sustained remembered-writer pressure. It remains at 32K for low-mutation
  allocation workloads.
- Major growth-factor adjustments now saturate at their intended bounds rather
  than under- or overshooting by one adjustment step.
- Fixed-work RSC runs put the process in the same multi-gigabyte RSS class with
  this policy, the previous policy at `bd4718a1`, and the pre-PR baseline at
  `51c2ca3a`. The collector change therefore does not explain that footprint.
- Allocation stack logging attributes the dominant retained malloc population
  to MIR JIT compilation and module metadata. Removing generated modules broke
  JIT correctness, while resetting the generator preserved correctness but
  tripled runtime and increased RSS. Both experiments were rejected. MIR code
  and module lifetime, or a compile budget, is separate follow-up work.

## Validation

- PGO-disabled, 6,000-response RSC comparison:
  - adaptive: 19.63 seconds, 159.5 MB committed arena peak;
  - fixed 32K: 22.80 seconds, 160.6 MB;
  - fixed 64K: 19.99 seconds, 163.4 MB.
- PGO-enabled branch build: 6,000/6,000 successful RSC responses at
  337.5 requests/second with a 159.5 MB committed arena peak.
- An exact old-policy PGO build completed the same run at 217.4
  requests/second. A production run without telemetry completed at 301.9
  requests/second; run-to-run and PGO profile variation account for the range.
- Burst/idle/burst RSC runs held 318.8 then 313.5 requests/second.
- Focused JIT barrier, async-generator liveness, comprehensive GC, upvalue,
  array-sort roots, string-concat roots, coroutine, timer, and map tests pass.
- `maid preflight` and `maid knowledge` pass.
- The full specification suite passes: 3,639 tests across 97 files, with no
  failures.
