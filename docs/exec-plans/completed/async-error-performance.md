# Async Error Boundary Performance

Status: completed
Last reviewed: 2026-09-17
Owner: theMackabu

## Goal

Keep the handled-error fixes while eliminating measurable successful-operation
overhead. Retain installed `ant` as the user's comparison baseline and use a
matched HEAD build to distinguish source effects from build/profile differences.

## Evidence and decisions

- Original candidate regressed readable sizing by roughly 7-8% versus installed
  Ant. A clean HEAD control built with the same flags, PGO profile, dependency
  objects, and generated headers confirms a readable regression from the patch.
- Changed error control flow invalidates hot readable PGO records: enqueue loses
  up to 27.6 million counts and pull-if-needed loses up to 138 million. Disassembly
  shows changed inlining of repeated internal slot access.
- `js_to_number` returns NaN when coercion throws. Use the existing invalid-size
  branch to consume that exception; successful numeric and object conversions
  need no additional pending-exception check.
- Benchmarks stay outside PGO training. Do not remove correctness coverage or
  weaken exception handling to improve timings.

## Outcome

Successful size conversion uses the original error/type checks and existing
invalid-size test. Pending coercion errors are consumed only after that test
detects NaN. Error values remain rooted across rejection allocation, including
when the size function has already changed the writable stream state.

Refreshed the checked-in Darwin ARM64 profile using
`env -u NO_COLOR ./meson/pgo/build.sh`, unmodified. Training completed without
timeouts or failed workloads. No final profile hash mismatches remain. The new
microbenchmark was not part of training.

Disassembly confirms restored slot-lookup inlining: public readable enqueue had
3 `js_get_slot` callsites in clean HEAD, 9 in the original patch, and 3 after
refresh. These are static callsites, not executed instruction counts.

## Measurements

All comparisons used pinned binaries, equal work, warmup, checked outputs, and
serial ABBA blocks. Other applications remained active on the host.

| Comparison | Readable numeric size | Readable object size |
| --- | ---: | ---: |
| Initial patch vs installed Ant | +6.9% | +8.1% |
| Final candidate vs installed Ant | +1.7% | +1.6% |
| Final candidate vs clean current HEAD | -2.8% | +0.4% |
| Matched modified units without PGO | -0.24% | -0.21% |

Positive means longer elapsed time. The profile-disabled experiment rebuilt the
same 20 changed translation units on both sides without profile use, keeping
all remaining objects identical. All six primary source controls were within
0.5%; a separate `events.once` comparison was 2.46% faster. This removes the
asymmetric effect of rejected profiles when assessing the source changes.

The final installed-binary comparison covers all 12 original workloads. Its
unchanged await control is 2.6% slower, so do not claim literal zero difference
between whole binaries or assign their residual 1-2% difference solely to the
patched paths. Clean HEAD uses its checked-in profile; the final candidate uses
the refreshed one. The matched source experiment and restored inlining support
removal of the original patch-specific slowdown.

Detailed build commands, profiles, source overlays/diffs, binaries, raw samples,
assembly, and all workload results: `/tmp/ant-error-perf-1lsan8qr/README.md`.
Final binary SHA-256:
`d4c5348af1fc509797bcfa7e0f565f68de72a549f591b109b81f2c08fc04bec3`.
Final profile SHA-256:
`dbbcab7bbb369394ecc9d97d82f0630fc141df92adcf42ef5a84b91982e442ac`.

## Validation

- Native build and all 16 Meson tests passed.
- Full spec suite: 4,240 assertions and 102 files passed, zero failures.
- Four handled-error suites, callback exception coverage, and focused fetch and
  writable-stream tests passed.
- Added coercion regressions for `valueOf`/`Symbol.toPrimitive` throwing Error
  objects or primitives, plus nonthrowing NaN/Infinity sizes.
- Preflight and whitespace checks passed.
