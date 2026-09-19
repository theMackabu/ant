# Native Call Fast Path

Status: complete
Last reviewed: 2026-09-18
Owner: theMackabu

## Outcome

Separated ordinary native calls from exception-scope management while keeping
the exception completion model, forgotten-failure propagation, and 64-bit
native/JIT ABI. Production changes are limited to `include/silver/call.h` and
`src/silver/glue.c`; existing module fixes and handoffs remain untouched.

## Design

The inline common path tests for an existing exception or heap `new_target`,
calls the native function directly otherwise, and checks its completion.
An out-of-line helper owns saving/rooting/restoring existing exceptions and
native constructor frames. It is noinline, but not forced into size optimization:
constructors also use it. Explicit returned records and forgotten pending
failures retain precedence over the caller's saved exception.

The common wrapper is small enough to inline in the measured release build.
It still checks pending state before the call and checks both explicit and
pending failures afterward. It is not an unchecked native call.

Confirmed handoff duplication includes `fetch_reject(req,
fetch_rejection_reason(js, value))`, since `fetch_reject` normalizes its input,
and immediate `js_take_thrown`/`js_reject_promise` sequences in Promise handler
processing and capability creation. Those are local simplification candidates,
not grounds to remove conversions before reentrant cleanup or user callbacks.
They remain unchanged so the performance comparison isolates native dispatch.

## Validation

- Configured macOS ARM64 build passes. All 17 native tests pass, including
  exception-scope preservation, forgotten failures, returned records with their
  pending handle cleared, constructor target roots, and old exception roots.
  Root tests disable conservative C-stack scanning and run major GC.
- All 30 retained exception regressions and both `new_target` suites pass.
  Full specs pass: 4,240 tests across 102 files, zero failures.
- Final ARM64 assembly has a direct indirect-native `blr` in the ordinary
  builtin branch, without exception save/restore or native root-frame setup.
  The 11 prior out-of-line `sv_invoke_native` copies disappear; there is one
  shared `Ant_Silver_InvokeNativeScoped` implementation. `jit_helper_call`
  decreases from 585 to 530 instructions. Its complete general-call stack
  frame remains 224 bytes; these are static code counts, not execution counts.
- Preflight and whitespace checks pass. The PGO profile is unchanged. No
  index operations were performed; the user staged additional changes during
  this task. No subagents were used.
- The full stack-depth sweep was not repeated: this change does not alter
  opcodes, interpreter handlers, or compiler stack effects. Existing broad
  sweep limitations remain recorded in the
  [exception model plan](exception-completion-model.md).

## Isolated patch timings

Baseline is the completed exception-record implementation, including its
native builtin-path guard. Candidate adds only this native-dispatch split to
production code. Both are pinned builds from the same configured tree and PGO
file. Five serial ABBA/BAAB blocks per workload produced 140 measured processes
with identical checked work/output and untimed warmup. Positive means slower;
deltas are medians of paired block-mean ratios.

| Workload | Candidate vs baseline |
| --- | ---: |
| Native dispatch through Number methods | -7.50% |
| Async iterator map | -0.74% |
| Node writable callback | -0.56% |
| Promise await control | -0.97% |
| Buffer from iterator | -0.42% |
| Set construction | -1.31% |
| JSON stringify | -0.03% |

Three longer ABBA/BAAB blocks at four times the work add 24 measured processes:
native dispatch is -7.15% (block range -10.71% to -6.51%), and Set construction
is -1.86% (range -3.04% to -1.43%). Native dispatch consistently improves in
this synthetic workload; the small broader-workload changes are not strong
evidence of general application speedups. Profile mismatch warnings remain,
and desktop applications were active. These measurements do not establish that
all cost of the larger exception-model migration has been recovered.

Task-entry source/index snapshots, pinned binaries, profile/build provenance,
raw measurements, fixture, runner, and before/after assembly are retained under
the directory recorded in `/tmp/ant-native-call-fastpath-current`.
