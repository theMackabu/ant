# Technical Debt Tracker

Status: active
Last reviewed: 2026-08-30
Owner: theMackabu

Use this file to record debt that is important enough to preserve but not yet
scheduled.

## Format

- Area:
- Issue:
- Impact:
- Proposed fix:
- Owner:
- Status:

## Open Items

- Area: module-level cached JS values (`g_*` statics)
  - Issue: RESOLVED for the 129 value caches audited on 2026-08-02: module-level `ant_value_t` caches migrated to per-isolate `js->builtins` / `js->mutable_roots` and are marked centrally in `gc_visit_roots` (see completed/module-import-gc-flake.md). The 2026-08-26 isolate audit found a broader remaining surface: a heuristic declaration scan reports 172 candidate mutable file-scope declarations in 58 source files. That raw count includes harmless startup configuration and intentionally shared native services, but it also includes active-resource lists, caches, native handles, root registries, and mutable library state. The global string-intern hash in `src/ant.c` is unguarded during insert/rehash and needs either isolate ownership or synchronization; the cage allocator and native-handle registry already use locks and can remain process-wide if their lifetime contracts survive a focused audit. Descriptors, builtins/mutable roots, normal ESM module state, events, regex, process state, rope mark metadata, and most heap rosters are already isolate-owned.
  - Impact: Same-process workers are now an intended consumer, so the remainder is no longer hypothetical. Any static that stores an isolate pointer, heap value, GC working state, libuv handle, or mutable ownership list can cause cross-isolate marking, callback dispatch through the wrong `ant_t`, teardown of another isolate's resources, or a data race.
  - Proposed fix: Classify the remaining statics instead of mechanically moving all 172 declarations: put JS heap values and isolate-owned resources on `ant_t`; make immutable-after-bootstrap metadata explicitly read-only; protect deliberately process-shared allocators, registries, and services with a documented locking/lifetime contract; eliminate redundant static-root registrations for fields already reached by isolate root visitors. Re-run the inventory after each migration because declarations are only an audit heuristic, not a completion metric.
  - Status: open (value-cache phase complete; non-value classification and migration required for same-process workers)

- Area: `src/gc/` and `src/shapes.c` — process-static collection and shape state
  - Issue: The collector shares mutable scheduling and working state across the process. `gc.c` owns global ticks, timestamps, adaptive thresholds, and EWMAs; `objects.c` owns the mark stack, epochs, minor/major mode, string marker, and profiling state; `strings.c` and `bigints.c` own global mark buffers/epochs. `shapes.c` additionally owns the root-shape graph, allocation pool, GC epoch, byte counters, and IC/object epoch counters; the JIT embeds absolute addresses of those global epoch counters. Concurrent collections race on all of this. Sequential interleaved collection is also unsafe after the 8-bit object epoch wraps because only the triggering isolate clears its mark bytes.
  - Impact: Two isolates cannot collect independently. A race can corrupt collector worklists or shape state, while an epoch collision can make the marker skip a live object and its outgoing edges. Making the counters per-isolate also changes JIT guard lowering, so this is more than a field move.
  - Proposed fix: Add an isolate-owned GC context covering scheduling, mark workspaces, epochs, and profiling/latching state. Give each isolate an independently cleared object/string/bigint mark domain. Make shapes and IC epochs either isolate-owned or a deliberately synchronized shared subsystem, then update JIT epoch loads to address the selected owner. Add a regression that drives two isolates through more than 254 interleaved collections, followed by concurrent collection/IC invalidation stress once the threading contract exists.
  - Status: hard blocker for sequential and concurrent same-process isolates

- Area: roots, symbols, registered-module namespaces, and code arenas — cross-isolate value ownership
  - Issue: `src/gc/roots.c` keeps one process-global static-root array with no isolate owner and no unregister path. Initializers register pointers into `js->builtins` and `js->sym`; `gc_visit_roots(js, ...)` later visits every registered slot using whichever isolate is collecting, and destroying an isolate leaves dangling slots. `src/modules/symbol.c` also overwrites process-global well-known-symbol values on each initialization and stores isolate prototype values in a global iterator fast-path table. `src/esm/library.c` caches a registered library's namespace value globally, so the first isolate's namespace is returned to later isolates. Finally, `src/runtime.c` keeps the code intern table plus code/parse arenas globally, while `js_destroy()` calls `code_arena_reset()` and can free storage still used by another isolate.
  - Impact: A second isolate is unsafe even without concurrent execution: it can mark values from the wrong heap, reuse another isolate's symbols or module namespace, retain dangling root pointers after teardown, or lose compiled/source storage when its peer is destroyed.
  - Proposed fix: Remove isolate fields from the process-global root array and mark `builtins`, `mutable_roots`, and `js->sym` only through isolate-owned visitors; reserve static roots for truly process-lifetime values that cannot point into an isolate heap. Move well-known symbol values, iterator prototype dispatch, registered-module namespace caches, code interns, and code/parse arena ownership onto `ant_t`. Keep only immutable registered-module names and native initializer functions process-global. Add an interleaved C test that creates two isolates, imports builtins, compiles and runs code in both, destroys either one, and then forces GC and continued execution in the survivor.
  - Status: hard blocker for even sequential live isolates

- Area: reactor, libuv loops, async host modules, and teardown
  - Issue: The 2026-08-26 scan finds 104 `uv_default_loop()` call sites across 27 C/header files. `src/reactor.c` runs that process-global loop and asks global no-isolate pending-work queries. `src/modules/timer.c` has one process-static state containing an `ant_t *`, timers, microtasks, next ticks, and immediates; initializing another isolate overwrites the callback owner. Fetch, fs, child process, readline, navigator, net, TLS, server, WebSocket, EventSource, abort, zlib, workers, RPC, LMDB, Wasm, N-API, and buffer code also retain process-global active lists, singleton environments, or isolate values. Several GC marker functions traverse those registries without filtering by owner. Some subsystems are already closer to the required model: events, regex, and process state are isolate-owned; cron/sandbox markers filter by `js`; Atomics wait queues are deliberately shared and locked. `js_destroy()` still invokes global cleanup for RPC, LMDB, buffer, and the global code arena, and it has no general per-isolate close-and-drain phase for outstanding libuv handles.
  - Impact: Isolates cannot run event loops concurrently, initialization can hijack callbacks, one collector can mark another isolate's host values, and terminating one worker can close or free resources belonging to another. Freeing an isolate before all of its handle close callbacks finish also creates teardown use-after-free risk.
  - Proposed fix: Give each worker/isolate its own `uv_loop_t` (or explicit owned loop pointer) and route every handle/request through it; make pending-work accounting isolate-specific. Move isolate-owned queues and active-resource registries into module state on `ant_t`, or retain a synchronized process coordinator only where semantics are intentionally shared (for example Atomics, process signals, inspector, or terminal ownership). Require every global GC marker to select only resources owned by the collecting isolate. Add a teardown phase that stops new callbacks, cancels/closes all isolate handles, drains close callbacks on the owning loop/thread, and only then destroys the VM/heap. Validate with two independently ticking isolates and termination while each major async module has pending work.
  - Status: hard blocker for multiple live async isolates; largest blocker for concurrent worker isolates and full host-module parity

- Area: `src/modules/worker_threads.c` — spawn-failure path
  - Issue: The `new Worker(...)` synchronous failure branch calls `wt_cleanup(wt)` (plain `free`) while `wt` is still linked on `active_workers_head` (so `gc_mark_worker_threads` walks freed memory on every GC) and while `wt_spawn_worker`'s failure branches have `uv_close`d the embedded `stdout_pipe` with a NULL callback (uv still owns a closing handle inside the freed struct).
  - Impact: Use-after-free on GC mark and on uv close completion — but only reachable when uv_spawn fails synchronously (EMFILE-class errors; missing binaries report asynchronously via exit_cb), which also makes a fix hard to test. The 2026-08-02 `ANT_GC_STRESS=3` full-spec crash previously attributed to this path was falsified on 2026-08-06: it was a WebSocket transport lifetime bug triggered before the worker exit path began, and is now fixed (see `completed/fable-perf-fixes-landing.md`). The spawn-failure audit finding remains open independently.
  - Proposed fix: Mirror the success path — `wt_detach` + close handles with `wt_on_handle_closed` and `close_pending` accounting; drop `wt_cleanup` entirely (the success path deliberately leaks the struct; the failure path should match).
  - Status: backlog

- Area: primitive receivers + prototype accessors
  - Issue: A getter/setter defined on a type prototype (e.g. `Object.defineProperty(String.prototype, 'x', { get() {...} })`) never fires for primitive receivers — `"s".x` returns undefined where node runs the getter. Pre-existing on all binaries (installed release, pre-port master, current); the primitive lookup paths (`js_try_get_len` boxing path, `sv_prop_get_at` fallback) skip accessor invocation for proto-held accessors on primitives.
  - Impact: Rare pattern (accessors on builtin prototypes), but a silent wrong-value divergence from node. The primitive-IC regression test pins only "warmed site agrees with cold access" for this case; fix the engine gap and the test can assert node's value.
  - Proposed fix: In the primitive branch of `js_try_get_len` (and the field-IC slow path), when the proto-chain lookup lands on an accessor, invoke the getter with the primitive as `this` (`try_accessor_getter` with the unboxed receiver) instead of falling through to paths that load nothing.
  - Status: backlog

- Area: `src/modules/child_process.c` — Windows `spawnSync` option parity
  - Issue: The win32 `spawn_sync_impl` branch ignores `timeout`, `killSignal`, and `maxBuffer` entirely (the POSIX branch implements all three via the select loop and `sync_read_ctl_t`). The `output` array and `error` shape gaps were fixed 2026-08-02, but the three options silently do nothing on Windows.
  - Impact: `execSync`/`spawnSync` with a timeout can hang forever on Windows, and `maxBuffer` provides no protection; `tests/test_child_process_exec_sync_options.cjs` skips on win32 for exactly this reason, so CI would not catch a fix or a regression.
  - Proposed fix: Implement the deadline with `WaitForSingleObject(pi.hProcess, remaining_ms)` + `TerminateProcess` on expiry, and cap the stdout/stderr accumulation loops with the maxBuffer budget (mirror `sync_read_ctl_t` semantics: set the ETIMEDOUT/ENOBUFS `error` via the same `sync_result_error` shape). Needs a Windows machine to validate; un-skip the options test on win32 once done.
  - Status: backlog (blocked on Windows validation)


- Area: `src/sandbox/backends/darwin.c` / macOS HVF VMM interrupts
  - Issue: The Darwin Hypervisor.framework backend currently continues when `hv_gic_config_set_msi_interrupt_range()` fails, and device bringup still includes legacy/polling/manual wake paths instead of a fully interrupt-driven virtio PCI model.
  - Impact: This is acceptable for sandbox bringup, but it leaves the backend less representative of the final VMM contract. Future virtio devices, networking, and lower-latency I/O may rely on proper MSI/MSI-X delivery instead of polling or legacy compatibility paths.
  - Proposed fix: Implement proper GIC MSI/MSI-X wiring for PCI virtio devices on Apple Silicon HVF, make block/net/vsock/9p completions use real device interrupts, then remove the legacy/polling/manual-wake bringup paths and any fallback code that only exists because MSI is missing.
  - Status: backlog

- Area: `src/modules/storage.c` / global `localStorage`
  - Issue: `JSON.stringify(globalThis)` can throw when it reaches the global `localStorage` property and no `--localstorage-file` or `localStorage.setFile()` path has been configured. Repro: `console.log(JSON.stringify(this));` currently reports `TypeError: Warning: --localstorage-file or localStorage.setFile were not provided with valid paths.`
  - Impact: Object inspection and serialization of broad global objects can fail because a host API accessor performs configuration validation too early. This is surprising behavior and unrelated code can trip over localStorage simply by enumerating or stringifying globals.
  - Proposed fix: Make the global `localStorage` property safe to read when unconfigured, either by returning an inert/lazy storage object that throws only when storage operations are used, or by omitting/marking the property so generic stringify/inspection does not invoke a throwing accessor.
  - Status: backlog

- Area: Silver closure allocation / function-object initialization
  - Issue: `sv_init_closure_function_object()` assigns `closure->func_obj` before proving that the backing function object and its required metadata were initialized successfully. Allocation failures in `mkobj()`, `.length` setup, or prototype setup can therefore leave a closure with an error or partially initialized `func_obj`.
  - Impact: This is an OOM-hardening issue rather than a normal JS-level repro; it likely needs allocator fault injection or a real process memory cap to observe reliably. If it does happen, the interpreter or JIT can expose a callable with invalid/incomplete function-object state.
  - Proposed fix: Make `sv_init_closure_function_object()` return a status value, assign `closure->func_obj` only after `mkobj()` succeeds, propagate failures from length/prototype setup, and have `sv_op_closure()` / `jit_helper_closure()` return the error instead of publishing `func_val`. Avoid fixing this by forcing generic bailout for `OP_CLOSURE`; that would risk large JIT/Newt regressions.
  - Status: backlog

- Area: Silver closure/upvalue allocation OOM handling
  - Issue: `sv_closure_init()` does not check the external upvalue-pointer-array `calloc()` used when a child captures more than `SV_CLOSURE_INLINE_UPVALS` values. It returns a non-NULL closure with `closure->upvalues == NULL`, after which both `sv_op_closure()` and `jit_helper_closure()` immediately write through that pointer. The adjacent `sv_capture_upvalue()`, `jit_capture_upvalue()`, and `jit_make_undef_upvalue()` paths also dereference unchecked `js_upvalue_alloc()` results.
  - Impact: A native allocation failure during ordinary closure creation causes undefined behavior, normally a NULL-pointer crash, instead of following the existing `kTypeError` OOM path. The external-array bug needs only five captured values; it is OOM-only, not an unreachable closure shape.
  - Proposed fix: First check the `calloc()` result in `sv_closure_init()` and return `NULL`; both callers already propagate that result, and the initialized unreachable arena slot is safe for a later young/major sweep. Do not free the slot immediately without also removing its young-roster entry. Then audit upvalue-cell creation as a separate step: make allocation failure propagate through the capture loop without publishing a partially initialized closure or leaving partially linked open cells. Add normal 5+-capture interpreter/JIT/GC coverage, but do not add a production fault-injection hook solely for the OOM branch.
  - Status: backlog

- Area: `src/modules/readline.c`
  - Issue: Rendering assumes readline owns the full visible prompt line, so redraws are anchored to the logical prompt text instead of the terminal position where editing actually begins.
  - Impact: Full redraw paths can clobber externally rendered prefixes, boxed prompts, or other same-line UI written before `question()` or `prompt()` starts editing.
  - Proposed fix: Track an explicit render origin / prompt anchor, separate logical prompt text from the screen position where input begins, and make redraws preserve external prefixes and custom prompt chrome.
  - Status: open

- Area: `src/modules/process.c` / `src/modules/tty.c`
  - Issue: Stdio TTY stream setup still has split ownership. `process.c` creates `process.stdout` / `process.stderr`, installs stdout `rows` / `columns`, and keeps its own terminal sizing helper; `tty.c` later reshapes the same streams, reinstalls `rows` / `columns`, and keeps a separate terminal sizing helper. The stale SIGWINCH setter path has already been removed, but the duplicated ownership that allowed it to drift remains.
  - Impact: Future stdio or TTY changes can diverge between the process bootstrap path and the TTY module path, especially around descriptor shape, `getWindowSize()`, resize behavior, and platform-specific terminal sizing.
  - Proposed fix: Make `tty.c` the single owner of TTY stream shape and terminal sizing for stdout/stderr, leaving `process.c` to create or expose process streams and wire process-specific event-emitter behavior. Share one sizing helper or module-level API so `rows`, `columns`, and `getWindowSize()` all use the same implementation.
  - Status: backlog

- Area: Node-style stream duck typing
  - Issue: `src/modules/stream.c` still probes and calls arbitrary stream-like properties such as `.write`, `.end`, `.pause`, `.resume`, `.pipe`, `.read`, `.next`, `_read`, `_write`, `_transform`, and `.getReader`. Related ad-hoc stream/event probes also appear in `src/modules/fs.c` (`.destroy` / `.end`), `src/modules/zlib.c` (`.write` / `.end`), and `src/modules/child_process.c` (`.once`). Lower-confidence event-handler cases in `src/modules/worker_threads.c` (`.onmessage`) and `src/modules/abort.c` (`.onabort`) should be checked for consistent non-callable handling, but should not be treated as bugs without a concrete repro because handler properties are normal platform-style APIs.
  - Impact: Each call site currently decides for itself which missing or non-callable methods are ignored, treated as compatibility no-ops, or allowed to fail later. That makes Node-stream compatibility behavior harder to reason about and can hide inconsistent errors across modules.
  - Proposed fix: Start with `src/modules/stream.c`: add small helper gates such as `stream_get_callable_prop()`, `stream_is_node_writable_like()`, and `stream_is_reader_like()`, then make required-method errors and optional-method no-ops consistent. After that, route the `fs.c`, `zlib.c`, and `child_process.c` stream/event probes through shared helpers. Finally, audit `worker_threads.c` and `abort.c` only for consistent ignore-vs-call behavior on non-callable handler properties.
  - Status: backlog

- Area: Silver compiler
  - Issue: `sv_compiler_t` scratch storage is still allocated per compilation, so repeated compiles in a long-lived process pay allocator churn for locals, bytecode buffers, constants, atoms, upvalue descriptors, loops, srcpos data, and maybe slot-type scratch.
  - Impact: One-shot CLI compiles are fine, but a REPL, watch mode, embedder, or other long-lived process cannot yet recycle compiler scratch space across compiles.
  - Proposed fix: Add a real `compile_pool` scratch allocator after the `compile_ctx` extraction. Pool the resizable arrays for `locals`, `local_lookup_heads`, `code`, `constants`, `atoms`, `upval_descs`, `loops`, `srcpos`, and potentially `slot_types`. Keep `line_table` separate or make it poolable scratch, since it is derived from the current source buffer rather than a semantic cache.
  - Status: backlog

- Area: Shared helper utilities
  - Issue: Small helper logic such as ASCII character classification, casing, and similar utility code is duplicated across multiple runtime and support modules with local one-off implementations.
  - Impact: Repeated copies drift over time, make bug fixes harder to apply consistently, and add noise when adding or reviewing new modules.
  - Proposed fix: Audit duplicated helper patterns across `src/` and `include/`, identify the stable cross-cutting utilities, and centralize them in a small shared header or utility module with repo-wide call sites migrated incrementally.
  - Status: backlog

- Area: `src/modules/intl.c`
  - Issue: `Intl` is now present and passes the current compat-table target, but several behaviors are still simplified compatibility implementations rather than fuller ECMA-402 semantics.
  - Impact: `Intl.Collator`, `Intl.NumberFormat`, `Intl.DateTimeFormat`, and `Intl.Segmenter` can still diverge from web or Node behavior for anything beyond the currently covered compat surface.
  - Proposed fix: Continue expanding `Intl` incrementally: replace `strcoll`-only collation, deepen `resolvedOptions()`, make `DateTimeFormat` actually honor stored timezone and locale options, and move `Segmenter` closer to the expected iterable/result object shape.
  - Status: backlog

- Area: `src/modules/timer.c`
  - Issue: `node:timers/promises setInterval()` is still explicitly unimplemented.
  - Impact: Promise-based timer APIs remain incomplete and can block compatibility with code that expects the Node timers/promises interval surface.
  - Proposed fix: Implement `setInterval()` on top of the existing timer promise scheduling machinery, including cancellation and signal handling behavior consistent with the existing `setTimeout()` and `setImmediate()` support.
  - Status: backlog

- Area: `src/modules/dns.c`
  - Issue: `node:dns` is still a minimal shim centered on `dns.promises.lookup`.
  - Impact: Tooling or apps that expect more of the Node DNS surface still need polyfills or will fail outright.
  - Proposed fix: Expand the module incrementally from the existing lookup path, prioritizing the most commonly used sync, callback, and `promises` APIs needed by current ecosystem packages.
  - Status: backlog

- Area: `src/modules/crypto.c`
  - Issue: `crypto.subtle` is only partially implemented and still marked for extension beyond the current digest-oriented support.
  - Impact: Web Crypto compatibility is incomplete, which blocks packages and runtime features that expect a broader `SubtleCrypto` surface.
  - Proposed fix: Extend `crypto.subtle` method coverage incrementally, starting with the highest-value operations after digest and preserving the existing algorithm parsing entrypoints.
  - Status: backlog

- Area: `src/modules/worker_threads.c`
  - Issue: `node:worker_threads` is currently a process-backed compatibility stub: `Worker` uses `uv_spawn`, worker-to-parent messages are JSON frames on stdout, and `Worker.postMessage` is explicitly unimplemented. In-process `MessagePort` queues store raw `ant_value_t` values and deliver synchronously within one isolate; the current `structuredClone` clones into the same `ant_t`, so neither mechanism can transfer an object graph into a destination isolate. The implementation also keeps one global active-worker list and its GC marker does not partition entries by isolate.
  - Impact: Replacing `uv_spawn` with a native thread before the isolate, loop, GC, and teardown blockers above are fixed would convert process isolation into races and cross-heap use-after-free. Even after those prerequisites, raw `ant_value_t` queues cannot cross heap boundaries, and termination must join the thread without freeing an isolate while callbacks or messages can still reach it.
  - Proposed fix: After sequential isolation and per-isolate loops/teardown are proven, create each worker as one native thread owning one `ant_t` and one `uv_loop_t`. Use parent/worker control blocks plus thread-safe queues and async wakeups. Implement a destination-isolate structured-clone codec that preserves cycles and supported built-in types, rejects unsupported values, and performs explicit transfer/detach for `ArrayBuffer` and `MessagePort`; never share raw heap values. Add bootstrap, module-loading, error/exit propagation, `ref`/`unref`, cooperative termination, loop shutdown, and join semantics, then remove the stdout framing and process spawn path. Keep process workers as a separate mode only if OS-level fault/security isolation is still desired; isolate-backed workers are threads, not process sandboxes.
  - Status: blocked on core isolate ownership and per-isolate reactor work; messaging/lifecycle remains a separate milestone

- Area: `src/modules/async_hooks.c`
  - Issue: `node:async_hooks` is still a minimal compatibility layer intended mainly to satisfy framework expectations.
  - Impact: Async context tracking semantics remain shallow, which can break libraries that rely on realistic async IDs, resources, or hook lifecycle behavior.
  - Proposed fix: Replace the placeholder async ID and resource behavior with real runtime-backed tracking, while keeping `AsyncLocalStorage` compatibility stable during the transition.
  - Status: backlog

- Area: Native cfunc ABI / iterator `next()` trampolines
  - Issue: The lightweight native-function ABI (`ant_cfunc_t`, registered via `js_mkfun` with a static `ant_cfunc_meta_t`) has no per-registration data channel — no userdata pointer and no QuickJS-style `magic` int. Any family of native callbacks that share logic but differ by one parameter therefore needs a named trampoline per variant. Current example: the six `*_iter_next` wrappers (array, string, map, set, typed array, headers) that each just call `js_iter_next_result(js, advance_*)`. That helper was introduced by PR #40 (issue #39) to fix unspecified-evaluation-order reads of an uninitialized `ant_value_t` in the old `js_iter_result(js, advance_*(js, &it, &value), value)` pattern, which GCC on Linux miscompiled (stale `value` read before the advance call) while Apple clang happened to evaluate in the safe order.
  - Impact: Source-level boilerplate only. The trampolines are zero runtime cost: `js_iter_next_result` is `static inline` and the advance function is a compile-time constant at each call site, so calls are devirtualized and inlined, and each wrapper keeps a distinct symbol in profiles and stack traces. Alternatives considered and rejected: `js_heavy_mkfun_native()` binding the advance pointer to one generic callback (adds a slot load plus an indirect call on the hot iterator path, allocates a heavier function object, requires a function-pointer-through-`void *` cast, and collapses profiler frames); macro-generated wrappers (pure taste, less greppable).
  - Proposed fix: Only if per-function registration data is wanted in several more places, add a QuickJS-style `magic` int to `ant_cfunc_meta_t` and pass it through to callbacks, letting one generic `iter_next` dispatch through a static advance table with no runtime pointer chasing. This is an engine-wide ABI change — every builtin signature, the call dispatcher, and the JIT native-call convention — so it must not be done just to delete the six iterator trampolines.
  - Owner: theMackabu
  - Status: backlog (deliberately deferred; revisit only alongside a broader cfunc ABI change)

- Area: Generational GC / open upvalues into suspended coroutine stacks
  - Issue: The closed-upvalue write barrier (`gc_upvalue_write_barrier`) covers closed cells only. An OPEN upvalue whose `location` was relocated into a materialized coroutine's heap-allocated VM stack (`sv_async_move_open_upvalues`) is not covered: if the owning generator/async object is promoted to old, minor GC scans neither the object (old objects are not traversed) nor the suspended VM stack (only `pending_coroutines` and mco stacks are scanned), so a young value written through such a cell between suspension and resumption is invisible to minor GC and can be freed while reachable.
  - Impact: Use-after-free requiring a specific interleaving: closure escapes a generator, generator suspends and is promoted, escaped closure writes a fresh heap value through the still-open relocated cell, minor GC runs before resumption. Pre-existing (predates the closed-cell barrier); no known in-the-wild repro.
  - Proposed fix: A per-isolate registry of live materialized VMs (`sv_vm_create(SV_VM_ASYNC)` / `sv_vm_destroy`), with minor GC scanning suspended VM stacks the way it scans `pending_coroutines`. Barrier-side alternatives are unsafe: remembering open cells and marking `*location` reads freed memory if the target VM is destroyed before the next GC (`sv_vm_destroy` does not close upvalues pointing into its stack).
  - Owner: theMackabu
  - Status: backlog

- Area: `src/streams/readable.c`
  - Issue: `ReadableStreamBYOBReader` is still explicitly unimplemented, and byte-source support is still called out as incomplete.
  - Impact: Web Streams byte-oriented consumers cannot rely on BYOB reader semantics, leaving an important platform feature gap for stream-heavy or browser-compatible code.
  - Proposed fix: Add real byte-source plumbing and implement `ReadableStreamBYOBReader` on top of it instead of routing byte sources through the default reader path.
  - Status: backlog

- Area: sloppy-mode `this` boxing / per-call primitive wrapper allocation
  - Issue: The 2026-08-06 parity fix made sloppy non-arrow functions that observe `this` (bytecode contains `OP_THIS`, `OP_CLOSURE`, or `OP_EVAL`) box primitive receivers per spec (`js_normalize_sloppy_this` + `jit_helper_normalize_sloppy_this`). Each such call allocates a fresh wrapper object: 3M warmed calls measure ~260ms vs node's ~20.6ms (node caches/inline-allocates its wrappers). Strict functions and sloppy functions that never touch `this` are proven flat (interleaved A/B, 20M-call micro).
  - Impact: Correct semantics (the old fast path returned `typeof this === "number"` where spec says `"object"`), and the cost is confined to a rare pattern — sloppy functions invoked with primitive receivers that actually read `this`. Only matters if it ever shows up in a real profile.
  - Proposed fix: A per-primitive-value wrapper cache (or a small per-isolate cache keyed on the primitive value/type) so repeated calls with the same receiver reuse one wrapper. Mind identity semantics: each sloppy call must still observe a spec-fresh wrapper if the program can detect identity (e.g. `this.x = 1` then re-entry) — so a cache is only sound if invalidated on any property write, or scoped to reads-only paths; otherwise consider a nursery-friendly fast allocation path instead.
  - Owner: theMackabu
  - Status: backlog (perf only; do not regress the 2026-08-06 semantics)

- Area: `src/modules/regex.c` / exact ECMAScript Unicode properties of strings
  - Issue: PCRE2 supports Unicode properties of individual code points but not ECMAScript's finite multi-code-point properties of strings. Ant therefore lowers properties such as `RGI_Emoji` into hand-written structural patterns in the current textual `/v` translator. Those approximations can admit malformed sequences, miss valid sequences, and cannot compose exactly with `/v` union, intersection, difference, nested sets, or `\q{...}` string members. Ant currently vendors PCRE2 10.47 with Unicode 16.0 tables, which also prevents exact Unicode 17 behavior even if the string-property expansion itself is updated.
  - Impact: `/v` expressions using `Basic_Emoji`, `Emoji_Keycap_Sequence`, `RGI_Emoji_Modifier_Sequence`, `RGI_Emoji_Flag_Sequence`, `RGI_Emoji_Tag_Sequence`, `RGI_Emoji_ZWJ_Sequence`, or `RGI_Emoji` can produce false positives or false negatives. Ad hoc structural fixes only move the boundary, while inconsistent code-point and string-property Unicode versions can make set operations internally contradictory.
  - Proposed fix: Keep PCRE2 as the final matcher and JIT, but replace textual `/v` rewriting with a typed set AST whose values contain code-point ranges plus finite string tries. At build time, pin one `ANT_UNICODE_VERSION`, consume the authoritative Unicode data for all seven properties, and generate checked-in compact trie/minimized-DAG tables with generator provenance. At pattern compile time, evaluate union, intersection, and difference exactly; emit code-point ranges as PCRE2 classes and emit string tries as compact longest-first, non-atomic alternations so normal ECMAScript backtracking to shorter members remains possible. Enforce the specification's early errors for complements of string properties and negated classes containing them. Keep PCRE2's code-point tables on the same pinned Unicode version, preferably by updating/regenerating its UCD data rather than maintaining a divergent overlay. A first milestone may lower direct uses of the seven properties exactly, but the typed AST is required for complete `/v` set algebra.
  - Rejected alternatives: Do not post-filter PCRE2 matches because that breaks captures, backtracking, lookarounds, quantifiers, replacements, and match indices. Do not depend on zero-width callouts to consume variable-length strings, fork PCRE2 with an Ant-only opcode, replace the regex engine, or continue growing structural emoji approximations; each has a larger correctness or maintenance cost than generated exact lowering.
  - Owner: theMackabu
  - Status: backlog (targeted `RGI_Emoji` false-positive fix exists; complete property-of-strings and Unicode-version conformance remain open)

- Area: `src/modules/regex.c` / remaining RegExp divergences from node
  - Issue: A 68-probe node-differential harness (2026-08-06 validation of the regex rework) leaves 15 pre-existing divergences, all reproduced identically on the installed release and on pre-rework binaries — untouched by and out of scope for the rework. Families: (1) exec-result `groups` is an accessor property where node has a plain writable/enumerable/configurable data property (affects `Object.getOwnPropertyDescriptor` shape on `exec`/`d`-flag results); (2) `$<name>` named-group references in string replacements are not substituted (`'abc'.replace(/(?<m>b)/g, '[$<m>]')` emits the literal text) and the function-replacer named-`groups` trailing argument is missing; (3) legacy statics `RegExp.leftContext`/`rightContext` are undefined after matches ( `$1..$9`/`lastMatch` work); (4) subclass/own-property dispatch: an own or subclass `global`/`flags` getter and a subclass `exec` override are bypassed by the built-in `@@match`/`@@replace` fast paths (a swapped `RegExp.prototype.exec` data property IS honored); (5) `lastIndex` string values are not coerced before use, `lastIndex` is defined non-configurable so accessor redefinition throws where node allows it pre-first-exec; (6) `new RegExp('').source` is `""` instead of `"(?:)"`, and `/(?:)/g`-style empty-match iteration over non-BMP text without `u` advances by code point instead of code unit.
  - Impact: Silent wrong values or missing substitutions for code relying on named-group replacement strings, legacy statics, or RegExp subclassing; all are node-observable divergences that a differential test will keep flagging. None affect the perf-critical exec/match/replace fast paths' results for ordinary regexps.
  - Proposed fix: Fix per family, cheapest first: `source` normalization and `lastIndex` ToNumber coercion are one-liners; `$<name>` substitution + replacer `groups` argument extend the existing `repl_template`/replacer marshaling with the already-cached named-groups meta; `groups`-as-data-property is a result-shape change (canonical shape already exists — add the slot); leftContext/rightContext extend `update_regexp_statics`; subclass/own-getter dispatch requires the batch/fast-path guards to also check `global`/`flags`/`exec` own-or-overridden state (guards already exist for `exec` data-property swaps — extend to accessors and subclass prototypes).
  - Owner: theMackabu
  - Status: backlog (pre-existing node-parity gaps; validation harnesses in /tmp/v_regex*.cjs shapes, recreate if wiped)
