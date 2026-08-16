# REPL Evaluation Completions

Status: active
Last reviewed: 2026-08-15
Owner: theMackabu

## Goal

Make the interactive REPL display the completion value of top-level-await
input after it settles, without automatically awaiting ordinary Promise-valued
expressions or draining unrelated event-loop work.

## Architecture

- Evaluation returns a structured result that distinguishes synchronous
  completion from an asynchronous entry completion.
- The REPL waits only for the promise belonging to that asynchronous entry.
- Promise settlement and rejection handling remain runtime APIs; the REPL does
  not inspect promise object internals.
- The inspector consumes the same structured evaluator, but follows CDP's
  explicit `awaitPromise` policy instead of the REPL's implicit TLA policy.
- Inspector promise responses are deferred through promise reactions so CDP
  handlers never enter a nested libuv run loop.
- The reactor advances libuv until the target settles and uses a wakeable signal
  source so Ctrl+C can interrupt an otherwise dormant await without polling.
- Structured asynchronous evaluation returns an opaque handle retaining its
  suspended Silver coroutine. Interrupting a REPL wait cancels and releases
  that handle, so resolving the abandoned await cannot resume user code. The
  association does not become part of ordinary Promise state.
- Interactive evaluation and `.copy` share the same completion path.

## Task list

1. [x] Preserve top-level-await provenance in the REPL evaluation result.
2. [x] Add runtime promise settlement and handled-rejection APIs.
3. [x] Add a targeted, interruptible reactor wait.
4. [x] Route interactive evaluation and `.copy` through the shared path.
5. [x] Add PTY coverage for completion values, ordinary Promises, shell I/O,
   timers, persistent handles, rejection, clipboard evaluation, and Ctrl+C
   recovery.
6. [x] Complete focused and broad validation.
7. [x] Consolidate the value-only and structured REPL evaluator APIs.
8. [x] Route inspector evaluation and `Runtime.awaitPromise` through deferred
   promise responses.
9. [x] Replace timer polling and thread-dependent signal delivery with a
   wakeable signal bridge.
10. [x] Add explicit asynchronous-entry cancellation and PTY coverage proving
    that interrupted code cannot resume later.
11. [x] Cover Ctrl+C at the prompt after a completed fetch request.
12. [x] Route the inspector side-effect-safe evaluator through the same
    `awaitPromise` completion path.
13. [x] Rename and document the blocking host reactor and zero-upvalue Silver
    entry APIs.

## Decision log

- 2026-08-15: Do not infer asynchronous entry completion from `T_PROMISE`.
  The compiler's `is_tla` provenance is carried through a structured result so
  `Promise.resolve(1)` remains a normal Promise-valued REPL expression.
- 2026-08-15: Do not use `js_run_event_loop()` for REPL evaluation. A targeted
  wait must not be held open by unrelated servers, timers, or other handles.
- 2026-08-15: Keep the synchronous REPL host loop. It already advances libuv
  while waiting for readline, so targeted entry settlement fits the current
  architecture without introducing a second prompt state machine.
- 2026-08-15: Inspector awaiting must not call the synchronous reactor waiter.
  Promise reactions preserve CDP responsiveness and avoid re-entering the
  inspector WebSocket dispatcher from a nested `uv_run()`.
- 2026-08-15: Interrupting a REPL await is cancellation, not merely a stopped
  host wait. The async-entry coroutine must be detached and retired before the
  prompt is restored.
- 2026-08-15: Keep host cancellation out of `ant_promise_state_t`. Only
  structured asynchronous evaluator results receive an opaque coroutine handle;
  ordinary Promise and async-function state retain their existing layout.

## Validation status

- `meson compile -C build`: passed.
- `./build/ant tests/test_inspector_await_promise.cjs`: passed, covering
  `Runtime.evaluate`, `Runtime.runScript`, `Runtime.awaitPromise`, thenables,
  rejection, inspector-wait mode, and concurrent requests.
- `./build/ant tests/test_repl_top_level_await.cjs`: passed.
- Focused TLA, dynamic-import, await, thenable, Promise, global-descriptor, and
  shell tests: passed.
- `./build/ant examples/spec/run.js --all`: 3,920 passed, 0 failed across 100
  spec files.
- `maid preflight`: passed, including repository knowledge and structure checks.
- `tests/test_repl_static_import.cjs` was also attempted, but its existing PTY
  harness timed out before sending input because it searches for an unstyled
  `❯ ` sequence while the REPL inserts an ANSI reset before the space. Static
  import succeeded in the new PTY regression.

## Follow-ups

- General user-facing Promise cancellation remains out of scope. The new
  cancellation primitive is private to host-owned asynchronous entry jobs.
