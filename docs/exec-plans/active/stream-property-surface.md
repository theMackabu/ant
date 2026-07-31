# Stream Property Surface

Status: active
Last reviewed: 2026-07-30
Owner: theMackabu

## Goal

Finish the Readable/Writable public property surface so streams report their state the
way Node does, and stop leaking internal bookkeeping as enumerable own properties.

`event-emitter-process-child-streams.md` moved six writable flags onto prototype getters
and `tests/test_stream_writable_lifecycle.cjs` locks that shape in. The readable side was
never given the same treatment, and neither side is complete. Everything below is a
pre-existing gap, verified against a `master` build — none of it is a regression.

## Scope

- Readable and Writable instance properties (also inherited by Duplex/Transform/PassThrough).
- Own-property hygiene: `Object.keys(stream)` should not expose internals.
- The two behavioural divergences the audit surfaced (`highWaterMark` default,
  `Writable.readable`).
- Missing prototype methods, including the Node 17+ async iterator helpers.
- Missing `node:stream` module exports.

Out of scope: web streams (`stream/web`), `stream/promises`, and the `_readableState` /
`_writableState` internal field layout beyond what the getters need.

## Constraints

- `tests/test_stream_writable_lifecycle.cjs` already asserts the six existing writable
  flags are non-enumerable prototype getters. Keep that passing.
- Getters must read from `_readableState` / `_writableState`, never from mirrored own
  properties — the mirroring is what makes the current data props drift.
- `readableFlowing` is tri-state in Node (`null` / `false` / `true`). Ant initialises
  `state.flowing` to `false`, conflating "no consumer yet" with "paused". Fixing the
  getter without fixing the initial value produces a silently wrong `false`.
- Changing the default `highWaterMark` shifts when `write()` returns `false`, so it
  changes real backpressure timing. Treat separately from the cosmetic surface work.

## Current state

Measured with `new Readable({read(){}})` / `new Writable({write(c,e,cb){cb()}})` against
node v26.5.1.

### Readable — 10 missing, 3 wrong kind

| property                | node           | ant               |
| ----------------------- | -------------- | ----------------- |
| `readable`              | getter `true`  | **own data prop** |
| `readableEnded`         | getter `false` | **own data prop** |
| `destroyed`             | getter `false` | **own data prop** |
| `readableFlowing`       | getter `null`  | missing           |
| `readableLength`        | getter `0`     | missing           |
| `readableHighWaterMark` | getter `65536` | missing           |
| `readableObjectMode`    | getter `false` | missing           |
| `readableEncoding`      | getter `null`  | missing           |
| `readableDidRead`       | getter `false` | missing           |
| `readableAborted`       | getter `false` | missing           |
| `readableBuffer`        | getter `[]`    | missing           |
| `errored`               | getter `null`  | missing           |
| `closed`                | getter `false` | missing           |

### Writable — 6 missing, 1 wrong kind, 1 wrong value

| property                                                                               | node           | ant                |
| -------------------------------------------------------------------------------------- | -------------- | ------------------ |
| `writable`, `writableEnded`, `writableFinished`, `writableLength`, `writableNeedDrain` | getters        | getters ✅         |
| `writableHighWaterMark`                                                                | getter `65536` | getter **`16384`** |
| `destroyed`                                                                            | getter `false` | **own data prop**  |
| `writableObjectMode`                                                                   | getter `false` | missing            |
| `writableCorked`                                                                       | getter `0`     | missing            |
| `writableAborted`                                                                      | getter `false` | missing            |
| `writableBuffer`                                                                       | getter `[]`    | missing            |
| `errored`                                                                              | getter `null`  | missing            |
| `closed`                                                                               | getter `false` | missing            |

### Own-property leakage

```text
ant  Readable: ["readable","destroyed","_paused","_pipes","_streamOptions","readableEnded","_readableState","_read"]
node Readable: ["_events","_readableState","_read","_maxListeners"]

ant  Writable: ["readable","destroyed","_paused","_pipes","_streamOptions","_writableState","_write"]
node Writable: ["_events","_writableState","_write","_maxListeners"]
```

`stream_init_base` sets `readable`, `_paused`, `_pipes`, `_streamOptions` on every stream
regardless of direction, so a pure Writable carries a readable side. This affects
`{...stream}`, `JSON.stringify(stream)`, and deep-equal comparisons in test frameworks.

### Behavioural divergences

- **`Writable.readable === false`, node `undefined`.** Breaks `'readable' in stream` and
  `stream.readable !== undefined` duck-typing. Present on `master` too.
- **Default `highWaterMark` 16384 vs node 65536.** Node raised this in v22, so ant matches
  older Node. It was always the internal value; the PR merely made it observable by adding
  the getter. Changing it is a real behaviour change, not a cosmetic one.

### Missing prototype methods

| group                    | missing                                                                                                        |
| ------------------------ | -------------------------------------------------------------------------------------------------------------- |
| lifecycle                | `_destroy`, `_undestroy` (both), `setDefaultEncoding` (writable)                                               |
| readable core            | `unshift`, `wrap`, `iterator`                                                                                  |
| async helpers (node 17+) | `map`, `filter`, `take`, `drop`, `every`, `some`, `find`, `forEach`, `reduce`, `toArray`, `flatMap`, `compose` |

### Missing module exports

`addAbortSignal`, `compose`, `destroy`, `duplexPair`, `isWritable`,
`Readable.wrap`, `Writable.fromWeb`, `Writable.toWeb`, plus the internal
`_isArrayBufferView` / `_isUint8Array` / `_uint8ArrayToBuffer` helpers and the
`ReadableState` / `WritableState` constructors.

## Task list

Ordered so each step is independently landable and testable.

1. **Readable getters.** Add the 10 missing readable getters via `js_set_getter_desc`,
   reading from `_readableState`. Mirrors `js_writable_*_getter`.
2. **Convert the data props.** Move `readable`, `readableEnded` and `destroyed` (both
   sides) to prototype getters and delete the mirrored own properties.
3. **`flowing` tri-state.** Initialise `state.flowing` to null instead of `false`, then
   audit the ~9 `js_truthy(js_get(state,"flowing"))` sites — all want "not flowing" for
   both null and false, so they should be unaffected, but confirm each.
4. **Own-property hygiene.** Stop `stream_init_base` publishing `readable`/`_paused`/
   `_pipes`/`_streamOptions` as enumerable own properties; move them to slots or
   non-enumerable descriptors.

   This **necessarily** changes `Writable.readable` from `false` to `undefined`, matching
   node -- there is no way to stop publishing the own property and keep the old value. So
   the behaviour change lands with this step rather than being deferred: `'readable' in
   stream` and `stream.readable !== undefined` duck-typing both flip. Land it as its own
   commit with a test asserting the node-matching shape, so it can be reverted
   independently of the cosmetic getter work.
5. **`errored` / `closed`.** Both sides. `errored` is already tracked via
   `stream_set_errored`; `closed` needs wiring to the `close` emit.
6. **Writable remainder.** `writableObjectMode`, `writableCorked`, `writableAborted`,
   `writableBuffer`, `setDefaultEncoding`.
7. **Test.** Extend `test_stream_writable_lifecycle.cjs` or add a sibling asserting, for
   both directions: every flag is a non-enumerable prototype getter, and no flag appears
   as an own key.

   Do **not** assert `Object.keys()` equals node's set. The table above shows node also
   exposes `_events` and `_maxListeners`, which come from its EventEmitter and are out of
   scope here -- that assertion cannot pass however good the stream work is. Assert the
   absence of the stream-owned keys instead, against an explicit list:

   ```js
   const LEAKED = ['readable', 'destroyed', '_paused', '_pipes', '_streamOptions',
                   'readableEnded'];
   for (const k of LEAKED) assert(!Object.hasOwn(stream, k), `${k} is an own key`);
   ```

   Exact `Object.keys()` parity needs the EventEmitter internals to match too. If that is
   wanted, add it to the Scope and Task list of this plan or write a separate one; do not
   smuggle it in through a test assertion.
8. **`highWaterMark` default** — separate commit, separate decision. Benchmark pipe
   throughput before/after; 4× the buffer changes drain frequency.
9. **Methods** — separate plan. `unshift`/`wrap`/`_destroy`/`_undestroy` are small;
   the 12 async helpers are their own body of work and want `iterator` first.

## Decision log

- Getters read state directly rather than mirroring into own properties. The current
  own-data-prop approach is exactly what lets `readableEnded` drift from
  `_readableState.ended`.
- `readableFlowing` is not landable as a standalone getter — without the tri-state fix it
  reports `false` where node reports `null`, which is worse than `undefined` because it
  looks correct.
- The `highWaterMark` default is held back from the surface work because it changes
  observable backpressure timing, not just the description of it.
- `Writable.readable` is **not** held back, despite also being a behaviour change. It cannot
  be: step 4 removes the own property that produces the wrong value, so the two are the same
  edit. It ships with step 4, in its own commit, with a test.

## Validation status

- Property tables above measured 2026-07-30 against node v26.5.1 and a `master` worktree
  build; the divergences are pre-existing, not introduced by
  `event-emitter-process-child-streams`.
- Prototype chains verified identical to node:
  `Transform -> Duplex -> Readable -> Stream -> EventEmitter`.
- Baseline suite at time of writing: 439 pass / 16 fail, all 16 pre-existing and unrelated.

## Follow-ups

- `child.stdin.destroy()` reports success for its queued chunks where node reports
  `write ECANCELED`. Ant hands one chunk at a time to `uv_write`
  (`stream_writable_write_impl` enqueues while `priv->writing`), so the rest sit in
  `state.buffer` and never reach libuv to be cancelled. Node batches via `_writev`, so all
  of them are in libuv. Closing this needs `_writev`/cork support.
- The `events.on()` steady-state growth traced through to a general engine issue rather
  than anything stream- or events-specific. Fixed by
  [Property Reference Table Removal](../completed/property-reference-table-removal.md);
  the workload is now flat (98.1 MB -> 24.5 MB at 400k events).
