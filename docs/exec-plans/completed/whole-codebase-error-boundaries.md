# Whole-codebase Error Boundaries

Status: complete
Owner: theMackabu
Baseline: `87c9299b`

## Outcome

Implemented the 16 approved groups from the whole-codebase error audit without
subagents. Preserved the unrelated macOS PGO profile edit. Seven new regression
files are registered in the harness; the existing caught-stack test now covers
async completion and resume as well.

| Groups | Change | Focused evidence |
| --- | --- | --- |
| F01–F03 | Preserve undefined Promise/disposal errors, track abrupt-completion presence separately, propagate disposer getter failures | `test_disposal_error_values.cjs` |
| F04 | Clear both N-API and runtime pending state, pin the returned handle | `test_napi_clear_exception.cjs` and its native fixture |
| F05–F07 | Stop throwing event listeners; propagate iterable constructor and outer helper failures | `test_native_completion_errors.cjs`, `test_import_iterator_getter_errors.cjs` |
| F08 | Return checked close completions in interpreter and JIT; preserve the original body error | `test_vm_iterator_close_errors.cjs`, existing iterator-close and labeled-jump tests |
| F09 | Settle failed Promise continuation setup through existing owner rejection/cleanup paths | `test_promise_continuation_errors.cjs`, `test_cron_continuation_error.cjs` |
| F10 | Clear saved stacks when async entry/resume or REPL consumes an exception | `test_caught_exception_stack.cjs`; REPL source review |
| F11–F12 | Use proxy-aware import attribute enumeration; propagate hook shortCircuit getter errors | `test_import_iterator_getter_errors.cjs` |
| F13–F14 | Make regexp guards nonobservable; propagate CloseEvent field/getter/conversion failures | `test_native_completion_errors.cjs` |
| F15–F16 | Reject desktop loads with ordinary errors; consume renderer failures before serialization, clear serialization failures | Both edited desktop translation units pass compiler syntax checks |

## Decisions

- `undefined` is a valid exception payload. Disposal carries a separate presence
  flag; async disposal stores it alongside the payload in its private state.
- Synchronous iterator-close opcodes carry a byte indicating that an original
  abrupt completion already exists. Suppressed cleanup errors are consumed;
  normal cleanup failures return a marker checked by interpreter and JIT.
- Failed internal continuation setup queues the existing rejection handler,
  preserving callback timing and retained request ownership. Cron reuses its
  synchronous exception reporting/rescheduling path. Child-process spawn
  failure notification uses a microtask directly.
- Regexp guards inspect own data properties without invoking user accessors.
  Renderer error formatting likewise avoids invoking diagnostic message getters.
- Import attributes need the proxy enumeration path as well as an error check;
  the general own-key helper alone does not invoke proxy traps.

## Validation

- Configured macOS ARM64 build passes. Existing PGO counts produce expected
  changed-control-flow warnings; the profile was not regenerated.
- 22 focused runtime test files pass, including all seven new regressions.
- Full spec suite: **4,240 tests, 102 files, zero failures**.
- `tools/check_stack_depth.sh` swept 602 runtime files plus specs and JIT
  examples: no operand-depth rejections or JIT stack overflows. It exits 2 for
  `test_node_events_once_prototype_spoof.cjs`, `test_throw_stack.cjs`, and
  `test_with_strict.cjs`. All three also fail with the pinned baseline binary;
  the latter two deliberately throw or contain invalid strict syntax.
- Both desktop files pass `cc -fsyntax-only` with the configured runtime include
  paths. Full CEF linking and desktop execution were not performed.
- Preflight knowledge/structure checks and diff whitespace checks pass.
  Reconfiguration is unnecessary: this patch changes no build graph. The
  router's build-configuration recommendation includes the unrelated PGO edit.
- Companion upload, server-body, pipe, and crash-report continuation paths have
  source review and existing suite coverage; no dedicated failure injection
  was performed for each. REPL interaction was not automated.

Audit and command logs are retained locally in `/tmp/ant-whole-error-audit`.
The repository tests are the durable behavioral evidence.

## Resolved follow-up

The separate omission of iterator closing on a function return was subsequently
fixed in [Iterator Cleanup on Return](iterator-return-cleanup.md), including
nested/finally ordering and async iterators.
