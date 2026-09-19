# Module Error Boundary Audit

Status: completed
Last reviewed: 2026-09-18
Owner: theMackabu

## Scope and contract

Audit all 90 source/header files under `src/modules/`, including the 2,636
`js_mkerr`, `js_mkerr_typed`, and `js_mkerr_props` calls present at the start
of this audit. Trace helper results to their callers; a direct return can
still reach a promise, callback, event, or fallback that consumes the failure.
Check related handoffs outside the module directory when reached by those
paths. The starting revision is `00bf2f3a` plus the cron CI correction.

`js_mkerr*` creates a pending throw and returns a marker. Keep it for
synchronous throwing APIs. Deliver ordinary Error objects for operational
failures; consume actual throws before converting them to rejection or
callback values. Preserve arbitrary thrown values, new callback exceptions,
and GC rooting after removing pending-exception ownership. Keep changes off
successful paths where an existing error branch provides the boundary.

## Work and validation

- Inventory and review each module file, including files without direct error
  constructors; inspect callback/promise consumers and helper propagation.
- Reproduce normal-trigger omissions before fixing them. Add regressions for
  handled failures, error identity, observers, and callback-thrown controls.
- Build once the independent file groups are ready, run focused regressions,
  then run the full spec suite and preflight.
- Record per-file coverage and distinguish reproduced failures from
  allocation/platform paths checked only in source.

## Findings and changes

- Operational error values: persistent cron, timer cancellation, Blob/body
  conversions, worker termination, sandbox operations, server stop, and utility
  promise/callback APIs now deliver Error objects without leaving pending throws.
- Caught JavaScript errors: crypto callbacks, Web Locks, pipeline registration,
  Readable.from, async iterator continuations, Wasm imports, URL coercions, and
  utility getters preserve the original thrown value, including null/undefined.
- Explicit fallback/report paths consume discarded failures before continuing:
  stdin decoding, worker/sandbox JSON messages, Observable diagnostics, FFI
  callbacks, and HTTP failure responses. Secondary diagnostic getter failures
  cannot poison later event-loop work.
- Receiver helpers no longer store an internal throw marker as `thrown_value`.
  IteratorClose preserves an existing exception across cleanup; normal close
  failures still propagate. JSON tracks error presence independently from an
  undefined thrown value.
- Allocation failures in child-process stream construction now stop construction
  and clean up the private process/pipes; conversion failures are propagated
  before a marker can enter output objects or stream data.
- The new warmup coverage exposed a separate JIT catch bug in
  `jit_helper_catch_value`: it excluded `throw undefined`, unlike the interpreter.
  Removed that exclusion. A module-free regression fails above the 100-call
  tiering threshold before the fix. The condition predates this branch
  (`28162a96`, 2026-08-25).
- Invalid Date inspection now checks the date value before ISO formatting, so
  rendering its fallback text does not construct a discarded RangeError.

The audit retains original Error subclasses and synchronous throwing contracts.
Root consumed errors across allocation, cleanup, and property lookup where the
pending-exception slot previously provided ownership. No blanket exception
clearing was added after user callbacks.

Concurrent changes to `src/modules/assert.c`, the worker-callback layout in
`src/modules/cron.c`, and `tests/test_assert_pending_rejections.cjs` appeared
during the audit. They were preserved; this audit does not claim authorship.

## Validation and limits

Normal-trigger failures were reproduced before their fixes. The new coverage
checks observers, queued work, rejection identity, callback throws, close
precedence, and JIT warmup.

- Final native Meson build passed on macOS ARM64.
- All 23 focused test files passed, including the 12 new audit regressions,
  existing handled-error/callback-exception suites, cron parser/backend/CLI,
  FFI wrappers, Wasm API, and the concurrent pending-assertion test.
- Full spec suite: 4,240 assertions in 102 files, zero failures.
- Preflight knowledge/structure checks and diff whitespace checks passed.
- New tests are registered in `tests/harness/manifest.js`.

Local validation artifacts: `/tmp/ant-module-audit-focused-results.json`,
`/tmp/ant-module-audit-spec-all.log`, and
`/tmp/ant-module-audit-build-final.log`. Detailed partition audit notes are
`/tmp/ant-audit-io.md`, `/tmp/ant-audit-async.md`, and
`/tmp/ant-audit-values.md`; the durable coverage inventory is below.

Allocation failure branches were reviewed without fault injection. Native Linux
and Windows scheduler backends, worker kill syscall failures, and actual sandbox
VM backend error transport were not executed on this macOS host. The sandbox
decoder and these platform paths were traced in source. Existing PGO produces
expected changed-function profile mismatch warnings; no performance claims are
made and profile artifacts were not regenerated.

## Per-file audit coverage

Counts below are lexical `js_mkerr*` calls at the start of this audit (after
the three cron rejection replacements), not counts of distinct bugs. All 90
files were inspected; the total is 2,636. A zero includes files with indirect
error helpers, declarations, or native-only transport. Constructors deliberately
retained for synchronous throws are not omissions.

| File under `src/modules/` | Calls | Disposition |
| --- | ---: | --- |
| `abort.c` | 6 | Synchronous validation; abort reasons are values; callback throws retained. |
| `assert.c` | 10 | Rejections already normalized; concurrent pending-promise changes preserved. |
| `async_hooks.c` | 7 | Synchronous callback wrapper returns thrown completion. |
| `atomics.c` | 53 | Synchronous checks; waitAsync settles status strings. |
| `bigint.c` | 43 | Synchronous arithmetic/conversion helpers propagate markers. |
| `blob.c` | 18 | Fixed promise backing-store and typed-array allocation rejection. |
| `buffer.c` | 274 | Synchronous constructors; fixed TypedArray.from close/open propagation; async consumers checked separately. |
| `builtin.c` | 18 | Synchronous validation/allocation; delegates async serve to server. |
| `child_process.c` | 65 | Fixed failed stream construction, output conversion, and async chunk handoffs. |
| `cjit.c` | 39 | Compilation, signature, and invocation helpers return synchronous throws. |
| `collections.c` | 126 | Synchronous throws; shared close now preserves original error. |
| `cron.c` | 39 | Fixed three rejection boundaries and rooted caught callback reason; concurrent refactor preserved. |
| `crypto.c` | 134 | Fixed pbkdf2/scrypt callback error values and option getter propagation. |
| `date.c` | 15 | Synchronous validation; fixed Invalid Date display fallback in src/ant.c. |
| `dns.c` | 8 | Fixed promise lookup conversion of synchronous resolver errors. |
| `domexception.c` | 0 | No constructors; creates ordinary exception objects. |
| `events.c` | 37 | Existing callback/rejection normalization traced; no further changes. |
| `eventsource.c` | 6 | Synchronous constructor checks; async errors are event objects. |
| `fetch.c` | 5 | Normalized and rooted upload failures before cancellation/reentry. |
| `ffi.c` | 74 | Fixed callback conversion failure fallback; synchronous FFI helpers retain throwing contract. |
| `formdata.c` | 17 | Synchronous methods; multipart body callers consume helper throws. |
| `fs.c` | 305 | Fixed error-producing output/stream helpers at callback/rejection handoffs. |
| `generator.c` | 15 | Standardized caught throw consumption, retaining roots across coroutine cleanup. |
| `globals.c` | 1 | reportError validation throws; reported values are ordinary values. |
| `headers.c` | 48 | Synchronous validation/iteration helpers propagate; close uses shared helper. |
| `http.c` | 0 | No constructors; C transport status passed to owning modules. |
| `http_metadata.c` | 0 | No constructors; metadata transport only. |
| `http_parser.c` | 4 | Synchronous parser validation and decorated SyntaxError return. |
| `http_writer.c` | 11 | Synchronous payload/header helpers propagate out-parameter errors. |
| `intl.c` | 3 | Synchronous locale validation helpers return throws. |
| `io.c` | 2 | Synchronous formatting allocation errors; display fallback traced to strdate. |
| `iterator.c` | 33 | Fixed early/terminal/flatMap propagation and promise continuation boundaries. |
| `json.c` | 11 | Fixed undefined error sentinel and replacer getter propagation. |
| `lmdb.c` | 91 | Synchronous environment/transaction/database operations; buffer helper returns propagate. |
| `localstorage.c` | 7 | Synchronous argument/storage path errors. |
| `math.c` | 1 | Synchronous secure-random failure. |
| `module.c` | 7 | Synchronous module/require/hook validation; loader handoffs inspected. |
| `multipart.c` | 8 | Parsing/append helpers return markers; Request/Response consume before rejection. |
| `navigator.c` | 3 | Fixed callback/continuation rejections and lock ownership on failure. |
| `net.c` | 19 | Fixed receiver marker corruption and buffer conversion event errors. |
| `observable.c` | 23 | Fixed caught observer/cleanup/reporting ownership and getter propagation. |
| `os.c` | 5 | Synchronous priority API failures. |
| `performance.c` | 0 | No constructors or error handoffs. |
| `process.c` | 46 | Fixed stdin construction and decoder fallback error ownership. |
| `process_plan.c` | 5 | Existing rejected-result adapter consumes/roots helper errors. |
| `process_plan.h` | 0 | Existing rejected-result adapter consumes/roots helper errors. |
| `process_stage.c` | 0 | Native process status transport; no JS error constructors. |
| `process_stage.h` | 0 | Native process status transport; no JS error constructors. |
| `readline.c` | 18 | Synchronous validation; promise question/iterator completion resolves values. |
| `reflect.c` | 12 | Synchronous validation and delegated call/constructor propagation. |
| `regex.c` | 59 | Synchronous validation/allocation/matching helpers return markers. |
| `request.c` | 47 | Fixed promise body allocation/conversion rejection. |
| `response.c` | 48 | Fixed promise body allocation/conversion rejection. |
| `rpc.c` | 39 | Fixed receiver values and centralized consume/root rejection ownership. |
| `sandbox.c` | 81 | Fixed rejection adapters, backend errors, and ignored message parse failures. |
| `sandbox_stub.c` | 0 | GC stub only. |
| `server.c` | 40 | Fixed stop rejection and errors consumed by HTTP failure fallback. |
| `sessionstorage.c` | 4 | Synchronous argument errors. |
| `shell.c` | 36 | Synchronous builders propagate; process-plan rejected-result adapter consumes markers. |
| `stream.c` | 11 | Fixed pipeline registration/pipe failures and Readable.from error propagation. |
| `string_decoder.c` | 11 | Synchronous helpers; process stdin fallback now consumes their errors. |
| `structured-clone.c` | 9 | Recursive synchronous clone helpers propagate errors. |
| `symbol.c` | 7 | Fixed original exception preservation across IteratorClose. |
| `syntax.c` | 8 | Synchronous parse/strip validation and decorated errors propagate. |
| `temporal/core.c` | 59 | Synchronous Temporal conversion/options helpers propagate out-parameter errors. |
| `temporal/duration.c` | 16 | Synchronous Temporal validation/conversion errors. |
| `temporal/instant.c` | 13 | Synchronous Temporal validation/conversion errors. |
| `temporal/now.c` | 0 | Temporal provider errors return synchronously; no direct constructors. |
| `temporal/plain_date.c` | 12 | Synchronous Temporal validation/conversion errors. |
| `temporal/plain_datetime.c` | 14 | Synchronous Temporal validation/conversion errors. |
| `temporal/plain_monthday.c` | 8 | Synchronous Temporal validation/conversion errors. |
| `temporal/plain_time.c` | 10 | Synchronous Temporal validation/conversion errors. |
| `temporal/plain_yearmonth.c` | 15 | Synchronous Temporal validation/conversion errors. |
| `temporal/temporal_internal.h` | 0 | Declarations and synchronous Temporal helper contracts. |
| `temporal/zoned_datetime.c` | 23 | Synchronous Temporal validation/conversion errors. |
| `textcodec.c` | 11 | Synchronous encode/decode helpers; stream consumers own caught failures. |
| `timer.c` | 17 | Fixed cancellation error values and timer creation rejection; reason rooted across cleanup. |
| `tls.c` | 25 | Fixed receiver marker corruption; transport errors already silent. |
| `tty.c` | 21 | Fixed error values for callbacks/events and callback-thrown propagation. |
| `uri.c` | 10 | Synchronous encoding/decoding validation and allocation errors. |
| `url.c` | 54 | Fixed search-param and formatting coercion/getter propagation. |
| `url/legacy.c` | 5 | Synchronous parser/decoder errors propagate. |
| `url/url_internal.h` | 0 | Declarations and C URL data only. |
| `util.c` | 24 | Fixed promise/callback values, post-settlement throws, getter propagation, and aborted rejection. |
| `v8.c` | 6 | Recursive synchronous serialization/deserialization errors propagate. |
| `wasi.c` | 7 | Synchronous host/module failures propagate to Wasm construction/rejection adapter. |
| `wasm.c` | 90 | Existing rejection adapter consumes markers; fixed import getters and error rooting. |
| `websocket.c` | 21 | Synchronous validation/transport setup throws; async events use ordinary values. |
| `worker_threads.c` | 47 | Fixed termination rejection and deliberately ignored JSON parse errors. |
| `zlib.c` | 56 | Fixed output conversion failures before callback/event/stream delivery. |
