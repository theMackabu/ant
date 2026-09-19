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
