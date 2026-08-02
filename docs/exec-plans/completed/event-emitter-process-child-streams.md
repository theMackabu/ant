# EventEmitter, Process, and Child Stream Unification

Status: completed
Last reviewed: 2026-07-29
Owner: theMackabu

## Goal

Move process and child-process event handling onto the shared EventEmitter
implementation, restore Node-compatible child stdio backpressure, and close the
remaining compatibility gaps exposed by real packages.

## Scope

- Support string and Symbol event keys on `process`.
- Replace process and stdio EventEmitter method overrides with native
  listener-count hooks at the emitter boundary.
- Keep ordinary EventEmitter dispatch entries cache-dense while retaining
  once, AbortSignal, capture, and in-flight prepend metadata.
- Serialize Writable `_write` calls and complete child stdin writes only after
  libuv reports completion.
- Keep child stdout/stderr alive until their pipes drain.
- Separate WHATWG `URL.parse` from legacy `node:url.parse`.

## Decisions

- Each process/stdio emitter owns one listener-change hook. Signal, stdin data,
  and stdout resize watchers start on the first listener and stop on the last;
  `process.emit` no longer synchronizes watcher state.
- The hot `EventListenerEntry` is 16 bytes. Cold metadata is allocated only for
  once listeners, EventTarget options, and prepends added during dispatch.
- Writable length, high-water-mark, queue, callback, and drain state live in
  `_writableState`. Child stdin passes that completion callback through
  `uv_write`, which both bounds writes and owns the copied buffer until libuv is
  finished.
- Readable `push()` reports remaining high-water-mark capacity, independent of
  flowing mode. Process exit closes stdin but lets stdout/stderr reach EOF.
- Global `URL.parse` remains WHATWG. The `node:url` export returns the legacy
  object shape, including relative request targets such as `/`.
- Ant's direct `exec()` Promise extension retains its historical string
  rejection. The callback and `util.promisify(exec)` forms use Node-shaped
  Errors with status and captured output.
- Signal-terminated exec calls reject instead of resolving. Callback and
  promisified errors expose `code: null`, the signal name, killed state, and
  command; the direct Promise continues to reject with a string.
- Child process exit and close events use Node's signal shape: a null code and
  symbolic signal name, with matching `exitCode` and `signalCode` properties.
- Failed `execFile` callbacks expose the joined command through `.cmd` and use
  the Node-compatible command-and-stderr error message.

## Performance

For 1,000,000 emits with 16 listeners, the five-sample median fell from
262.8 ms with the installed `/Users/themackabu/.ant/bin/ant` to 172.1 ms with
`./build/ant`, a 34.5% improvement. The corresponding Node median was 79.5 ms,
so the remaining marginal-dispatch gap is about 2.17x.

## Validation

- `meson compile -C build`
- Focused EventEmitter, EventTarget, process, signal watcher, stream
  backpressure, child stdio, exec callback, native descriptor, and URL tests
- 128 KiB child stdin/stdout round trip through `cat`
- npm `serve` 14.2.5 request to `/`: `HTTP/1.1 200 OK`
- `./build/ant examples/spec/run.js --all`: 3,682 passed, 0 failed
- `maid preflight`

## Follow-ups

- None.
