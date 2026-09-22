# Iterator Performance Recovery

Status: completed
Last reviewed: 2026-09-21
Owner: theMackabu

## Outcome

Native some early close is 29.2% faster and Set.values for-of is 10.8% faster
than the pre-change build. The other six requested workloads range from 0.1%
slower to 1.3% faster. Required iterator method/result checks and observable
return lookup remain intact.

## Changes and reasoning

- `src/modules/iterator.c`: ordinary next/return lookup now reads shape data and
  walks the prototype chain once. The previous general missing-property lookup
  repeated chain walks; a baseline sample attributed about 27% of native some
  runtime to return lookup. Accessors, exotic objects, other value tags, and
  long chains use the existing general lookup. The helper allocates nothing and
  caches nothing, so callback mutations require no new GC or invalidation rules.
- `src/modules/collections.c`: Set iterator state uses the same direct native
  tag/pointer lookup already used by Map, with the generic fallback preserved.
  The old generic object conversion occurred on every Set iteration step.
- `tests/test_iterator_close_fast_path.cjs`: checks own/inherited methods,
  callback additions/deletions, undefined shadowing, getters, receiver identity,
  prototype rewiring, proxies, invalid methods/results, and original-throw
  preservation.

The two changes were measured separately before combining them: method lookup
alone improved native some by 28.4%; adding the Set state lookup improved Set
iteration by 10.1%. No per-element validation was removed.

## Final measurements

Times are ns/element, except native-some-close, which is ns/invocation. Installed
is the pinned installed binary; Before is the pinned build before these two
changes; Fixed is the final candidate.

| Workload | Installed | Before | Fixed | Fixed vs before | Fixed vs installed |
|---|---:|---:|---:|---:|---:|
| native-some-close | 125.29 | 179.39 | 127.00 | -29.2% | +1.4% |
| set-for-of | 4.46 | 4.74 | 4.23 | -10.8% | -5.1% |
| array-for-of | 1.83 | 1.87 | 1.88 | +0.1% | +2.3% |
| string-for-of | 13.90 | 14.15 | 14.14 | -0.1% | +1.7% |
| generator-for-of | 116.24 | 119.98 | 119.59 | -0.3% | +2.9% |
| typedarray-from-array | 16.46 | 16.67 | 16.45 | -1.3% | -0.0% |
| await-sync-result-control | 7.22 | 7.27 | 7.18 | -1.3% | -0.6% |
| await-readable-stream-control | 419.19 | 431.01 | 426.27 | -1.1% | +1.7% |

The remaining 1.4–2.9% slower rows against Installed remain visible. Installed
predates multiple runtime changes and uses a different profile; these gaps are
not isolated evidence against iterator validation. The manual sync-next and
reader.read controls do not use js_iter_open/js_iter_close. This change does not
claim to eliminate every small gap against that older binary.

## Provenance and method

- Source checkout: `983426318269111b205feb41b220b0107a154573`.
- Installed A: embedded revision 59a2d6b3, SHA-256
  `3d1dc26f314c48964ea0f66b76f0f2fb0b7c025787d9799ba7d6e210dcc05821`.
- Before B: embedded revision 56231004, SHA-256
  `e10bec58200a728ed605a328668806b6dd6b41d79674494ffc1074c159c87200`.
- Fixed C: SHA-256
  `effd0a712fe6f5876ff70efb46ee1523d913e5ca3e341829f37a0c06b257da5c`.
- Fixed PGO file: `meson/pgo/profiles/ant-darwin-aarch64.profdata`, SHA-256
  `d2bce1f2e62f71c57168cbda5caa8eccd79b0d592ec8278829417851bfacb430`.

Before and Fixed use the same compiler flags and profile file. The compiler
reported discarded profile counts only for the changed get_set_iter_state
control flow. The profile was not regenerated or modified.

The original fixtures are unchanged from
`/tmp/ant-iterator-bench-20260921-4ajkh3uk/`. Final batches used three times the
original counts to reduce short-run noise. Each binary ran eight separate
processes per workload, each with three timed rounds and output assertions:
192 processes / 576 timed rounds total. Orders were ABC CBA CBA ABC, then
CBA ABC ABC CBA; every pair follows ABBA/BAAB and the reverse repeat. Final
numbers are medians of process medians. Benchmarks ran serially, without builds
or test suites overlapping. macOS background activity remained present.

Pinned binaries, fixture hashes, exact compiler commands, sample output,
per-process timings, staged comparisons, build logs, and the native sample are
under `/tmp/ant-iterator-recovery-20260921-wwq8h0us/`. `manifest.json` records
provenance and `final/results.json` contains final samples and spreads.

## Validation and limits

- Native Meson build passed; final build hash matches the measured candidate.
- All 4,240 specs across 102 files passed.
- All 14 focused iterator, collection, promise, generator, JIT, and GC regression
  files passed. The new test also passed under Node.
- Read-only correctness, efficiency, and maintainability reviews found no code
  findings. `maid preflight` and `git diff --check` passed.
- Wasm compilation reached linking, then failed on existing missing
  io_print_error_stack, io_print_error_header, and io_print_error_props symbols.
  Recompiling both changed modules from HEAD into temporary objects and replaying
  the same link reproduced all three missing symbols. Repository sources and
  candidate objects were not replaced during that check. Wasm runtime tests and
  package dry-run were not completed because a fresh package could not build.

Unrelated dirty documentation, Temporal results, and Wasm bridge changes were
preserved. The existing Wasm I/O link failure remains outside this native
iterator performance change.
