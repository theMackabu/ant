# Central Error Handoffs

Status: complete
Owner: theMackabu
Base: `34d08d0c`, retaining the previously completed fixes in the working tree

The marker representation described here was subsequently replaced by the
[exception completion model](exception-completion-model.md). This plan records
the intermediate handoff consolidation and its validation, not the current
payload or pending-state contract.

## Outcome

Centralized native exception delivery while preserving all prior behavioral
regressions and the independent iterator, getter, disposal, native-callback,
N-API, and stream-state fixes. Strict uncaught-exception reporting remains.

- `js_reject_promise` consumes internal markers via `Ant_Error_ConsumeMarker`
  before its settlement guard. An already-settled Promise does not leak a
  newly delivered exception. Ordinary values leave pending state alone.
- Payload-bearing markers consume only a matching active exception. Zero-payload
  markers identify the active completion, including a thrown undefined/null.
  An old marker does not erase a newer unrelated throw.
- `Ant_Error_CallCallback` normalizes the first argument before invoking an
  error-first callback. The caller's argument array is unchanged; a callback's
  new throw remains pending even when it throws the same value again.
- `Ant_Promise_Observe` shares internal continuation setup and queues the
  owner's rejection callback on setup failure. Public `.then()` behavior is
  unchanged. Cron and other paths with different settlement contracts retain
  their explicit owner-specific handling.

Removed redundant rejection consumption/constructor replacements, the cron
OS-request settlement refactor, Request/Response rejection adapters, and
repeated continuation-failure branches. Preserved explicit consumption before
reentrant cleanup and ordinary error construction for events. Existing
`js_mkerr` construction/throw semantics and synchronous error returns remain.

## Scope reduction

Against the user's reset base, the production diff (`src`, `include`, and
`packages`) decreased from 1,504 insertions / 837 deletions to 1,431 insertions /
808 deletions: **2,341 to 2,239 changed lines**. Tests and documentation are
excluded from these counts. Production file count increased from 62 to 65
because the shared APIs add changes to `src/errors.c`, `include/errors.h`, and
`include/ant.h`. Preserving the independent fixes prevents a large file-count
reduction; the refactor removes repeated ownership logic, not those fixes.

## Validation

- All 30 retained JS regression files pass on both the saved task-entry binary
  and the candidate, including uncaught callback reporting. None of those test
  files were changed by this refactor.
- New native `error-handoffs` Meson test passes. It covers exact thrown values,
  ordinary-value rejection with a pending exception, stale payload markers,
  rejection of fulfilled/rejected Promises, callback argument ownership, new
  callback throws, and deferred continuation setup failure handling.
- Configured macOS ARM64 build and reconfiguration pass. Full specs pass:
  **4,240 tests across 102 files, zero failures**.
- Stack-depth sweep covers 604 runtime files plus specs/JIT examples, with no
  operand-depth rejections or JIT stack overflows. Exit 2 remains due to the
  known `test_node_events_once_prototype_spoof.cjs`, `test_throw_stack.cjs`, and
  `test_with_strict.cjs` baseline failures.
- Both desktop translation units pass syntax checks. Full CEF execution was
  not performed. No allocation-failure injection or performance comparison
  was performed, so these results are behavioral validation, not universal
  equivalence or a performance claim.
- Preflight and diff whitespace checks pass. The PGO profile's SHA-256 and the
  empty index are unchanged. No commits, staging, or subagents were used.

A complete task-entry source snapshot, patch, binary, comparison results, and
metrics are retained at the local path recorded in `/tmp/ant-central-errors-current`.
