# tlsuv WebSocket Buffered Read

Status: completed
Last reviewed: 2026-06-19
Owner: theMackabu

## Goal

Make Ant's outbound `WebSocket` client preserve message boundaries when a
backend sends multiple WebSocket frames in one TCP read, or splits one frame
across TCP reads.

## Scope

- Keep the fix in a Meson wrap patch for vendored tlsuv.
- Avoid changing Ant's Hono adapter or server-side WebSocket frame parser.
- Keep the common complete-frame path parsing directly from the current read
  buffer, and allocate only for incomplete trailing bytes.

## Decisions

- tlsuv's prior parser handled only one frame per transport read and had no
  pending-byte buffer. That loses data when the kernel coalesces separate
  WebSocket frames, which matches the observed proxy truncation.
- The patch adds a small per-socket pending buffer for incomplete tails, loops
  over complete frames, and stops dispatching after a close frame.

## Validation

- Reproduced the truncation with `/tmp/t.js`, real `wscat`, and an `ffprobe`
  command through the proxy against the installed Ant release.
- Verified the same `/tmp/t.js`, `wscat`, and `ffprobe` command returns the full
  JSON payload with the patched local build.
- Added `tests/test_websocket_client_buffered_frames.cjs`.
- Ran `meson compile -C build`.
- Ran `./build/ant tests/test_websocket_client_buffered_frames.cjs`.
- Ran `./build/ant examples/spec/run.js websocket`.
- Ran `maid preflight`.
