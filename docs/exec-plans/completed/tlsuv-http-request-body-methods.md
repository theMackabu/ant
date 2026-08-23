# tlsuv HTTP Request Body Methods

Status: completed
Last reviewed: 2026-08-23
Owner: theMackabu

## Goal

Allow Ant `fetch()` requests to send fixed and streamed bodies with HTTP
methods such as `PATCH` and RFC 10008 `QUERY` without changing the method on
the wire.

## Scope

- Keep the transport compatibility fix in a Meson wrap patch for vendored
  tlsuv.
- Preserve tlsuv's existing zero-length `Content-Length` behavior for `POST`,
  `PUT`, and `PATCH`.
- Add focused local-server coverage for `PUT`, `PATCH`, and `QUERY`.

## Decisions

- tlsuv 0.40.13 rejected upload data unless the method was `POST` or `PUT`, so
  `PATCH` and `QUERY` failed before transmission.
- HTTP message framing is independent of method semantics. The patch permits
  queued request data for any method and derives `Content-Length` whenever a
  fixed body is present.
- Ant's Request implementation continues to enforce its own method and body
  rules before the transport is invoked.

## Validation

- Reverse-checked the patch against the configured tlsuv subproject.
- Applied the patch cleanly to a pristine tlsuv v0.40.13 source snapshot.
- Ran `meson compile -C build`.
- Ran `./build/ant tests/test_fetch_request_methods.cjs`.
- Ran `./build/ant tests/test_fetch_redirect.cjs`.
- Ran `./build/ant examples/spec/run.js fetch request`.
- Ran `maid preflight` and `maid validate_changes`.
