# Async Error Boundaries

Status: superseded
Last reviewed: 2026-09-17
Owner: theMackabu

This is the early checkpoint restored from stash `4165fdd2`. The work is now
recorded in [the completed plan](../completed/async-error-boundaries.md) and
[the exception completion model](../completed/exception-completion-model.md).
The contracts below describe the earlier design, not current runtime invariants.

## Goal

Handled operational failures and exceptions converted to promise rejections must
not remain pending runtime exceptions. Preserve the callback exception reporting
introduced by `59a2d6b356a3c93f269e5e56e4100fd003624573`.

## Findings and decisions

- `js_mkerr*` creates an internal throw marker and sets the isolate's pending
  exception. It is not an ordinary Error value suitable for callbacks/events.
- Use silent Error construction for ordinary failure values. When a subsystem
  catches a throwing operation and converts it to a rejection or callback error,
  consume that exception before any subsequent call into user code.
- Leave `js_reject_promise` and the event-loop reporter's general contracts
  unchanged. A blanket exception clear could erase an unrelated callback throw.
- Reproduced affected fetch, filesystem, TLS, child-process, RPC, Web Streams,
  Node streams, zlib, promise-cycle, and async-iterator paths. Related validation
  rejections are included where the same ownership mistake is explicit.
- Preserve the checkout's pre-existing upvalue, build, test-manifest, and plan
  edits. This work does not attribute every old API failure to the reporting
  commit; some synchronous failures also reproduce on an older binary.

## Checkpoint

- [x] Trace and reproduce failures in isolated subprocesses.
- [ ] Repair subsystem boundaries and add focused subprocess regressions.
- [ ] Rebuild; run new tests, callback exception tests, and relevant specs.
- [ ] Run preflight and required broader validation; record any limitations.

## Validation

Pending. Regression cases must verify normal Error values, successful process
exit after handled failures, and continued reporting of real callback throws.
