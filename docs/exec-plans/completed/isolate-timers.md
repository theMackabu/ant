# Isolate Timer and Job Ownership

Status: completed
Last reviewed: 2026-09-13
Owner: theMackabu

## Outcome

Timers, microtasks, next ticks, and immediates retain their owning isolate
through scheduling, pending-work queries, GC traversal, dispatch, and teardown.
The template feature remains stashed and the committed code-arena fix is intact.

## Decisions

- Embed queue state at the end of `ant_t`, available before module bootstrap.
  Keep timer handles' owner explicit and make ID lookup isolate-specific.
- Keep the current libuv default loop for this slice. A timer callback selects
  its own isolate even when the shared loop is pumped by another runtime.
- Detach timers from their owner before closing. Close callbacks free native
  storage only, so they can finish after the isolate is destroyed without
  running a shared event loop during heap teardown.
- Discard queued work before destroying the VM. Release queued coroutine
  references and await holds, and block new timer/job work during teardown.
- Remove timer prototype registrations from the global root array; their
  existing `isolate_values.h` entries already provide isolate-owned roots.
- Preserve existing job ordering, refresh, ref/unref, and argument semantics.
- Store the immediate ID in the internal handle slot used by cancellation.
  Previously `clearImmediate(handle)` could not find the scheduled entry.

## Validation

Validated on the working tree based on `bcfd34c9` with the configured Darwin
ARM64 release/LTO/PGO build:

- Meson reconfigure and builds passed. PGO control-flow mismatch warnings are
  expected for changed code; these runs do not establish performance results.
- `isolate-timers` and `isolate-code-arena` native regressions passed. The timer
  test covers job and timer dispatch, nested queue draining, GC visitors,
  promise/thenable and primitive-await jobs, ID collisions, ref/unref,
  immediate cancellation, surviving-isolate GC, both teardown orders, native
  close callbacks after heap destruction, and releasing queued await holds.
- Ten focused tests passed: timer refresh, unref exit, fired-timeout GC, timer
  mutation, next-tick ordering and bound receivers, async-await microtasks,
  abort-listener ordering, concurrent immediate promises, and basic timers.
- Full spec suite: 4,229 tests across 102 files, zero failures.
- Preflight and changed-file whitespace checks passed.

## Limits

This does not make the complete runtime safe for concurrent isolates. Shared
GC state, static roots outside the timer module, symbols, native namespace
caches, other host resources, and loop ownership remain separate work.
The native regression bootstraps only timers to test this boundary independently
of other modules' global symbol/GC-root registrations. Full CLI bootstrap is
covered by the focused JS tests and spec suite in a single isolate.

Detached native timer storage is reclaimed when libuv processes its close
callbacks; destruction does not itself pump the shared loop. Applications must
still drain close callbacks before destroying an owned loop in a future loop
ownership implementation. The complete host-runtime gate remains in
[the debt tracker](../tech-debt.md).
