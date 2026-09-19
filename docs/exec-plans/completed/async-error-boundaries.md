# Async Error Boundaries

Status: completed
Last reviewed: 2026-09-18
Owner: theMackabu

## Goal and decisions

Handled operational errors and throws converted to rejections must not remain
pending exceptions. Keep the callback exception reporting in `59a2d6b3`.
Use silent Error values for normal failures, or consume a caught throw before
passing it to user code. Do not clear exceptions after callbacks: that could
erase a new callback exception. Leave the general promise rejection contract
and event-loop checks unchanged.

The audit reproduced this across fetch, filesystem, TLS, child-process, RPC,
streams, zlib, promise cycles, async iterators, and promise validation APIs.
Some synchronous failures already occurred in an older pre-reporting binary;
do not attribute all affected cases solely to `59a2d6b3`.

## Outcome and validation

Subsystem boundaries now deliver ordinary Error objects or consumed rejection
reasons. Errors remain rooted across cleanup and user property lookups after
pending exception ownership is removed.

- Native Meson build passed.
- Four new handled-error regression suites and the existing callback-exception
  suite passed on the rebuilt binary.
- Seventeen existing focused filesystem, fetch, stream, RPC, events, network,
  and promise regressions passed.
- Full spec suite: 4,240 assertions, 102 files, zero failures, including
  EventSource and WebSocket.
- Preflight knowledge/structure checks and diff whitespace checks passed.

## Performance scope

Keep normalization in existing failure branches. Successful filesystem stream
callbacks add no marker check; numeric stream-size results add no exception-state
load. New checks after fallible user getters/conversions enforce the boundary.
Benchmarked against the user's installed `/Users/themackabu/.ant/bin/ant` with
pinned binary copies, checksums, fixed work, warmup, and serial ABBA ordering.
All 320 measured processes produced matching checksums. Paired median changes:
readable numeric size +6.9%, readable object size +8.1%; writable sizes and
callback workloads near neutral; iterator workloads 2.4-8.1% faster. The unchanged
promise control was 3.1% slower. Positive means longer elapsed time.

The installed binary identifies as `59a2d6b3`, while the candidate uses
`34d08d0c` plus this patch. That intervening commit regenerated PGO, and the
candidate compiler discarded mismatched profile counts. Other applications were
active. These results compare the requested binaries and do not isolate the
instruction cost of the added checks. The subsequent
[performance investigation](async-error-performance.md) removes the added
size-conversion checks and restores the lost PGO inlining; the initial numbers
above are retained as historical evidence.

Local artifacts, including pinned binaries, source diff, harness, metadata,
and individual samples: `/tmp/ant-error-path-bench-ok1_ij0j/README.md`.
Candidate SHA-256: `909255dc40b89f09a2d19c778523c0d2415c2342f5cee7ea75ccf514c4829704`.
Installed SHA-256: `3d1dc26f314c48964ea0f66b76f0f2fb0b7c025787d9799ba7d6e210dcc05821`.

## Separate existing issues

- Synchronous `Readable.from()` treats a throwing iterator `next()` as EOF.
  Repairing this requires separate iteration error propagation work.
- Some stream jobs can run a later timer before reporting an error-listener
  exception. Reproduced with installed and candidate binaries; the exception
  remains fatal. The fsync/read callback tests retain immediate-exit checks.
- The spec server readiness helper can accept a leftover listener. Its fixture
  ownership check is separate from the fixed fetch rejection that caused the
  original orphan server.

## Review follow-up: synchronous writable throws

Node-style `done(error)` treats null and undefined as successful completion.
Forwarding a consumed `_write()` throw through that callback therefore erased
those failures. The write-start helper now distinguishes pending, completed,
and thrown outcomes. Immediate writes, buffered writes, and `end(chunk)` return
the original pending exception to their JavaScript caller; callback-delivered
operational errors retain their existing behavior.

Regressions verify exact thrown values, including null and undefined, and that
failed writes invoke neither a successful completion callback nor an error event.
Node and the rebuilt Ant both pass. Focused stream/callback/filesystem coverage
and the full 4,240-assertion spec suite pass after the fix. Existing benchmark
artifacts above predate this follow-up; the new comparison is recorded under
`/tmp/ant-nullish-write-9fy2vc1k/`.

## CI follow-up: cron scheduler rejections

Run `35328733749` on `00bf2f3a` failed every platform's cron backend error
tests. Linux's intentional crontab read failure, macOS's failed replacement,
and Windows's trigger-limit failure were caught by JavaScript but still
escaped as pending exceptions. Cron's three promise rejection paths still
used `js_mkerr`, which creates a throw marker and leaves a pending exception.

Use `js_make_error_silent` at the immediate settlement, worker completion,
and queue-submission failure boundaries. Preserve TypeError messages,
synchronous argument validation, and the general promise/event-loop contracts.
Add platform-independent coverage for several queued missing-script failures
and a subsequent request, with and without an uncaught-exception observer.
Missing scripts fail before invoking an operating-system scheduler.

Validation on macOS ARM64:

- Rebuilt `00bf2f3a` sources and reproduced both the existing macOS backend
  failure and the new queued-rejection regression before changing cron.
- Rebuilt with the fix; cron parsing, macOS registration/rollback/removal,
  scheduled CLI/timer behavior, handled async errors, and uncaught callback
  exception tests all passed.
- Cron, promise, and async specs passed; preflight and whitespace checks passed.

Native Linux and Windows backend reruns remain for CI. Executable-path lookup
and work-queue submission failures were inspected but not fault-injected.

The subsequent [module-wide audit](module-error-boundaries.md) covers all 90
module files and records the remaining fixes and their validation.
