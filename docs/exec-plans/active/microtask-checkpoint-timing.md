# Microtask Checkpoint Timing in Nested Native→JS Calls

Status: active
Last reviewed: 2026-08-01
Owner: theMackabu

## Problem

`sv_vm_call` runs `sv_vm_maybe_checkpoint_microtasks` on the way out of nested
calls, so a promise rejected inside a native-invoked listener (`abort()`,
`emit()`, uv callbacks) can be reported as an unhandled rejection before
control returns to the user code that attaches the handler in the same tick.
Node drains microtasks only when the JS stack empties.

## Repro

```js
const { on, EventEmitter } = require('events');
const em = new EventEmitter();
const ac = new AbortController();
(async () => {
  const it = on(em, 'x', { signal: ac.signal });
  const p = it.next();
  ac.abort(); // rejects p inside the abort dispatch
  try {
    await p;
  } catch (e) {
    console.log('caught:', e.name);
  }
})();
```

- ant: prints `Uncaught (in promise) AbortError: ...` **then** `caught: AbortError`
- node: prints only `caught: AbortError`

Verified 2026-08-01 on a clean build of committed HEAD (`e00454fb`) — this is
long-standing engine behavior, not a regression from the events/stream work
(which does not touch the VM checkpoint code).

Same-tick attach WITHOUT a nested native call is fine
(`Promise.reject()` + immediate `await` produces no spurious warning), so this
is checkpoint **placement**, not the rejection tracker itself.

## Constraints

- Engine-level change in the silver VM; do NOT spot-fix in modules (e.g. by
  pre-attaching dummy handlers in events.c) — that hides the symptom per call
  site and diverges further from node's model.
- The checkpoint exists for a reason (stackless-coroutine-era scheduling);
  removing it outright needs the full async test suite (`tests/harness`
  async group + spec async files) plus server load tests to prove nothing
  relies on the early drain.

## Task list

1. Map every `sv_vm_maybe_checkpoint_microtasks` call site and classify:
   top-of-stack (safe) vs nested-return (the bug).
2. Gate the drain on JS stack depth == 0 (or an equivalent "outermost native
   entry" flag), matching node's semantics.
3. Validate: repro above goes silent; `tests/harness/run.js` async group
   clean; unhandled-rejection reporting still fires for genuinely unhandled
   rejections (spec `promise.js`, `exceptions.js`).

## Validation status

- Repro confirmed on clean HEAD 2026-08-01. No fix attempted yet.
