# Listener Accept Bursts

Status: completed
Last reviewed: 2026-09-05
Owner: theMackabu

## Goal

Admit queued connections in a single readiness callback, following Bun's
uSockets accept loop, so established HTTP traffic does not delay each new
connection by another event-loop pass.

## Decisions

- Carry the Unix libuv change in `vendor/packagefiles/patches/libuv-drain-accept.patch`
  through `vendor/libuv.wrap`; do not rely on edits to an extracted dependency.
- Drain until accept returns an error, the callback closes the listener, or the
  callback leaves a connection pending instead of consuming it with `uv_accept`.
  Preserve libuv's descriptor-exhaustion handling and deferred acceptance.
- Request backlog 512 for Ant HTTP listeners, like Bun. The OS still caps the
  effective backlog. Windows keeps its existing libuv accept implementation.
- This affects all Unix libuv listeners, including pipes. An uninterrupted
  arrival stream can extend the accept callback; no batching limit is added,
  matching the requested Bun behavior.

## Validation

- The focused C regression queued 32 loopback connections before running the
  event loop. The original library accepted only one and failed the assertion.
- The patched library accepted all 32 in one pass. The regression also checks
  deferred `uv_accept` resumption and listener closure from the accept callback.
- Build the regression with `meson compile -C build test-listener-accept-burst`
  and run `./build/test-listener-accept-burst`. The validation router's suggested
  `./build/ant tests/test_listener_accept_burst.c` is not applicable to a C test.
- Full build, `tests/test_ant_serve.cjs`, and
  `tests/test_server_invalid_request_target.cjs` passed with PGO disabled.
- `./build/ant examples/spec/run.js websocket eventsource`: 20 tests passed.
- `maid preflight` passed, including knowledge and structure checks.
- The existing Nix toolchain required its development shell and a valid
  `SDKROOT`. A PGO link failure from conflicting profile metadata required a
  rebuild without PGO. Restored `-Dpgo=auto`, rebuilt successfully with PGO,
  and reran the C regression, both server tests, and both specs successfully.
  No source build defaults were changed.
- No before/after Elysia throughput claim is made; the deterministic regression
  establishes admission behavior, not the cause of every observed latency tail.
