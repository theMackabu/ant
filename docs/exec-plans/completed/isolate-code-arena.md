# Isolate Code Ownership

Status: completed
Last reviewed: 2026-09-13
Owner: theMackabu

## Outcome

Destroying an isolate now frees only its own persistent code and source interns.
Destroying a sibling preserves compiled functions in the surviving isolate.

## Decisions

- Move persistent code blocks and source interning into `ant_t`. Pass the
  owning isolate explicitly through allocation, accounting, mark, rewind,
  and reset APIs. Keep IC/JIT cleanup before freeing their code storage.
- Parser scratch remains scoped to existing parse marks. Isolate destruction
  must not reset scratch belonging to an enclosing parse operation.

## Validation

Validated on 2026-09-13 against the working tree based on `397f7441`, with the
configured Darwin ARM64 release/LTO/PGO build. This is correctness validation,
not a performance comparison.

- Meson reconfigure, full build, and the native `isolate-code-arena` test passed.
  The regression checks 20 child teardowns, distinct JIT contexts, both teardown
  orders, interning, arena accounting, mark/rewind, enclosing parser scratch,
  and continued execution of closures, classes, eval, and Function code.
- `test_ant_serve.cjs` and `test_js_entry_module_syntax_server.cjs` passed.
- Full spec suite: 4,229 tests across 102 files, zero failures.
- After separating template work, the build, native arena regression, and full
  spec suite passed again with the template changes absent.
- `maid preflight` and `git diff --check` passed. Changes only propagate an
  allocation owner through the compiler/objects handler; opcode stack effects
  and control-flow emitters are unchanged.

## Request Isolate Gate

Request isolates remain deferred. A native probe bootstrapped two runtimes,
queued a native callback through the parent, and drained the child's microtasks.
The callback received the child isolate, confirming that the global queue does
not preserve dispatch ownership. The probe used no cross-heap JS values and
exited without dereferencing freed memory. Its temporary source and build log
are under `/tmp/ant-template-isolate-probe/host.c` and `host-build.log`.

Timer dispatch state, static GC-root registration, and some host-module teardown
also remain process-wide. These need their own ownership changes before
per-request isolation. Parser scratch supports nested sequential scopes, not
simultaneous parsing on multiple threads.

The current ownership rule is documented in
[runtime invariants](../../repo/runtime-invariants.md#code-storage-ownership).
