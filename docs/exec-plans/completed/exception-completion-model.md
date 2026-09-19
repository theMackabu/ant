# Exception Completion Model

Status: complete
Last reviewed: 2026-09-18
Owner: theMackabu
Base: `34d08d0c`, preserving the existing uncommitted behavioral fixes

## Outcome

Replaced markers whose payload depended on mutable isolate fields with
self-contained exception records. The native/JIT result remains a 64-bit
`ant_value_t`, with unchanged signatures and error tag. Removed the separate
`thrown_exists`, `thrown_value`, and `thrown_stack` fields. The prior iterator,
disposal, stream, callback, Promise, and N-API fixes and regression coverage
remain in place.

## Decisions

- `kTypeError` carries a GC-managed record containing the exact thrown value
  and captured stack. Newly raised completions have distinct identity, even
  for repeated throws of the same Error. Forwarding a record retains identity.
  The existing object union stays 16 bytes; each new record requires an object
  allocation. Local VM catches retain their direct-value path.
- The isolate retains one current handle for scalar/parser helper propagation.
  Catch and delivery use the returned record, clearing current state only when
  the handles match. N-API retains this same record representation.
- Native entry scopes preserve a caller's completion across a handled inner
  failure. A native function returning success with an unconsumed new failure
  returns that exception instead. The VM builtin fast path uses the same
  boundary. Successful calls allocate no record, but pay before/after checks;
  declaring the helper inline does not guarantee free or inlined execution.
  This does not unwind C code or replace operation-specific cleanup checks.
- Ordinary Error construction no longer throws temporarily. Throw capture uses
  own data properties without invoking a stack getter or adding a stack property
  to arbitrary thrown objects. Error constructors still expose their stacks.
- GC traces both record fields. Saved handles are rooted across reentrant work.
  A preallocated OOM record avoids recursive allocation when record creation
  fails; it is the exception to distinct identity. Zero remains a bootstrap or
  no-isolate failure sentinel, and one remains the JIT retry sentinel. Neither
  is traced as a heap pointer.
- Stream callbacks are scheduled as actual microtasks. A handled Promise chain
  would hide their uncaught errors under the new native boundary contract.

This replaces the representation in the [earlier handoff refactor](central-error-handoffs.md).
Removing direct split-state access requires a one-time migration across modules;
this replacement does not reduce the total touched-file count. Current rules
are in [runtime invariants](../../repo/runtime-invariants.md).

## Validation

- Configured macOS ARM64 build passes; all **17 Meson native tests** pass.
- All **30 focused JS regression files** pass. The only changed prior JS test
  strengthens uncaught ReadStream callback timing; the strengthened assertion
  fails on the saved task-entry binary and passes on the replacement.
- Full specs: **4,240 tests across 102 files, zero failures**.
- Expanded native handoff tests verify exact primitive values, independent
  records/stacks, same-value distinct throws, nested native scopes, forgotten
  failure propagation, ordinary Error construction, and 64-bit ABI size.
  A retained record alone keeps its payload and stack alive through minor and
  major GC with conservative C-stack scanning disabled.
- The stack-depth sweep ran 604 runtime files plus specs and JIT examples,
  with no operand-depth rejections or JIT stack overflows. Coverage remains
  incomplete (exit 2) because `test_node_events_once_prototype_spoof.cjs`,
  `test_throw_stack.cjs`, and `test_with_strict.cjs` exit nonzero. All three
  also fail on the saved task-entry binary. The final sweep also failed
  `test_process_stdin_readable_ref.cjs`; six subsequent isolated runs on each
  binary all passed with the same operand-depth diagnostics enabled. That
  fourth failure remains intermittent and unclassified, not a confirmed
  baseline failure. Native tests, focused regressions, full specs, and the
  stack-depth sweep were rerun after fixing the builtin fast-path bypass.
- Both desktop translation units pass syntax checks; full CEF execution and
  allocation-failure injection were not performed.
- Preflight and whitespace checks pass. The PGO profile hash and empty index
  are unchanged from task entry. No subagents, commits, or staging were used.

## Diagnostic migration follow-up

The sandbox error serializer and macOS desktop error formatter still retagged
exception records as Error objects. Both now read the record's value and captured
stack without consuming its current handle. Sandbox record displays fall back to
the message when no stack exists. Ordinary value formatting retains its prior
behavior.

Regression coverage checks stackless errors, captured stacks, primitive throws,
detached records, and records older than the current exception. The macOS test
compiles the actual private formatter from the application entrypoint; its event
loop is stubbed and CEF is not launched. Both regressions fail against their
pre-fix implementations and pass with the migration completed. The configured
macOS build, all 18 Meson tests, the caught-stack and module async error
regressions, and all 4,240 spec assertions pass. Preflight passes. This follow-up
changes two production files; it does not exercise a full sandbox guest or CEF
application launch.

The inspector and child-process listener diagnostics now also use captured
record stacks. Inspector responses retain the completion until formatting ends;
a separate temporary-root scope avoids removing persistent remote-object roots.
Raw Promise rejection values still use their Error object's stack. The inspector
protocol and child-process regressions cover primitive/object stacks, mutation
of an Error's stack during inspection, remote handles after GC, and avoiding a
stack getter on stackless callback errors. The obsolete exception-state alias
and save/restore wrappers were replaced with direct handle operations.

## Sampled normal-operation cost

Six existing fixtures were run in five serial ABBA/BAAB blocks: 120 measured
processes, identical iterations, warmups, and checked outputs. Baseline and
candidate binaries were pinned and their hashes verified. Positive means
longer elapsed time; the result is the median of paired block-mean ratios.

| Workload | Candidate vs task entry |
| --- | ---: |
| Async iterator map | +3.32% |
| Node writable callback | -3.01% |
| Promise await control | +0.01% |
| Buffer from iterator | +0.08% |
| Set construction | +2.65% |
| JSON stringify | +4.38% |

This is a whole-binary sample, not performance equivalence or source-only
attribution. The same PGO file was retained, but the changed control flow emits
profile mismatch warnings; desktop applications remained active. Throw-heavy
performance was not measured. New escaping completions allocate a record.
These final measurements include the builtin fast-path fix; earlier samples
are retained separately under `perf-before-fastpath` in the artifact directory.

The task-entry source snapshot, pinned binaries, checksums, focused results,
and performance samples are retained under the local directory recorded in
`/tmp/ant-exception-model-current`. The reusable performance fixture sources
are in `.cache/module-perf-20260918-111408/fixtures/`.

## Fresh-PGO comparison against installed Ant (2026-09-18)

Compared the installed `59a2d6b3` binary with revision `5903d763` after the user
regenerated PGO. Both executables, the candidate profile, and all workload inputs
were pinned. This measures the combined source and PGO changes; the installed
binary's original profile and complete build configuration are unavailable.

Ran 120 successful processes serially on an Apple M4 Pro with 24 GiB RAM and
macOS 27.0 (26A428). Every workload used ABBA then BAAB order (four samples per
binary); Splay and EarleyBoyer received another ABBA/BAAB block (eight per binary)
because of variation. Values below are medians of per-process results. Desktop
applications remained active. Pre-run audits found no concurrent Ant/build
processes, and no agent-started validation or builds overlapped the timings.

| bench-v8 case | Installed score | Candidate score | Score change |
| --- | ---: | ---: | ---: |
| richards | 6414.37 | 6366.72 | -0.74% |
| deltablue | 6387.00 | 6208.48 | -2.80% |
| crypto | 13529.79 | 13520.35 | -0.07% |
| raytrace | 12228.45 | 11946.04 | -2.31% |
| earley-boyer | 12131.80 | 12329.69 | +1.63% |
| regexp | 7542.34 | 7463.68 | -1.04% |
| splay | 5966.89 | 7145.80 | +19.76% |
| navier-stokes | 24376.39 | 24376.39 | +0.00% |
| Geometric mean | 9872.06 | 10028.93 | +1.59% |

For the following timings, negative change means faster. Iteration keeps the
original best-of-five timing; async iteration keeps the median of nine rounds.
Game of Life uses 500 warmup ticks plus 5000 measured ticks on the seeded 150x40
grid. Microbench retains its full adaptive calibration and reports ns/op.

| Workload / metric | Installed | Candidate | Time change |
| --- | ---: | ---: | ---: |
| iteration: for..of array ms | 54.000 | 53.000 | -1.85% |
| iteration: for..of map.values() ms | 73.500 | 76.500 | +4.08% |
| iteration: for..of, no prop read ms | 40.500 | 38.000 | -6.17% |
| iteration: indexed for ms | 57.500 | 56.500 | -1.74% |
| life-fixed: render_ms | 0.446 | 0.447 | +0.27% |
| life-fixed: tick_ms | 0.332 | 0.328 | -1.13% |
| async-iteration: async-next ms | 36.817 | 36.696 | -0.33% |
| async-iteration: async-next-control ms | 39.761 | 41.106 | +3.38% |
| async-iteration: fulfilled-promise ms | 17.750 | 17.916 | +0.93% |
| async-iteration: fulfilled-promise-control ms | 19.344 | 20.229 | +4.57% |
| async-iteration: readable-stream ms | 105.517 | 103.061 | -2.33% |
| async-iteration: readable-stream-control ms | 105.094 | 105.842 | +0.71% |
| async-iteration: sync-result ms | 5.687 | 5.796 | +1.92% |
| async-iteration: sync-result-control ms | 1.827 | 1.802 | -1.37% |

The original seeded Game of Life player was also run through tick 5000 with its
per-tick logging: simulation averaged 1.35% less time and rendering 0.90% more.
All fixed-work state hashes match Node; all original-player final states match.
Iteration returned exact expected counts in separate Node/A/B validation runs,
and async iteration and bench-v8 passed their built-in output checks.

All 43 microbench rows completed. Their equal-weight geometric mean normalized
time increased 1.58%. Larger repeated slowdowns were:

| Microbench | Installed ns/op | Candidate ns/op | Time change |
| --- | ---: | ---: | ---: |
| regexp_ascii | 88.375 | 108.180 | +22.41% |
| regexp_utf16 | 96.355 | 114.480 | +18.81% |
| regexp_replace | 133.520 | 164.115 | +22.91% |
| array_push | 10.395 | 11.265 | +8.37% |
| array_pop | 18.670 | 20.625 | +10.47% |
| math_min | 7.025 | 7.720 | +9.89% |
| bigint256_arith | 67.225 | 73.445 | +9.25% |

The positive bench-v8 aggregate does not establish performance equivalence.
The regex, array push/pop, and Math.min microbench slowdowns remain unresolved;
no cause is assigned to the exception machinery without separating PGO effects.
Splay's block gains ranged from 17.74% to 66.20%, so its combined 19.76% median
gain is not a stable floor. Median Splay peak RSS was 1.147 GiB installed versus
1.954 GiB candidate. This time-based workload does unequal work at different
speeds; the memory result does not establish a leak or equal-work overhead.
`bigint_shift_floor` was also variable; its apparent median gain should not be
interpreted as a stable improvement. Other small deltas remain subject to noise.

The pinned candidate passed all 4240 specs across 102 files plus the focused
caught-exception-stack and inspector regressions. Preflight passed. Native
long-error-message tests passed before PGO training and were not rebuilt here.
No runtime code or checked-in benchmark sources were changed during measurement.

- Baseline SHA256: `3d1dc26f314c48964ea0f66b76f0f2fb0b7c025787d9799ba7d6e210dcc05821`.
- Candidate SHA256: `ae5d051b5f51da11074cdedcc3533e772ea388443bac91ac8b7a6a32dbe84854`.
- Candidate profile SHA256: `0f17b3a7a3b79e273cabf96342ca83630101f4d4881dc9ecdabfd3ffab300c1e`.
- Local evidence: `.cache/pgo-comparison-pdjc_4wa/`, including `report.md`,
  `results.csv`, `summary.json`, `runs.json`, frozen fixtures, runner scripts,
  input hashes, per-run stdout/stderr, process audits, and time/RSS statistics.

The subsequent [performance and nested-finally follow-up](exception-performance-and-finally.md)
records the regression attribution, restored upvalue cleanup, completion-lifetime
fix, and fresh-profile results. The measurements above describe the earlier
pinned `5903d763` build.
