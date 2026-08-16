# Technical Debt Tracker

Status: active
Last reviewed: 2026-08-14
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
  - Issue: RESOLVED for value caches (2026-08-02): all 129 module-level `ant_value_t` globals (static caches + extern-linked stream/fetch protos) migrated to per-isolate `js->builtins` / `js->mutable_roots` (field lists in include/isolate_values.h, opcode.h-style), marked centrally in `gc_visit_roots` — this was also the root cause of the module-import GC flake (see completed/module-import-gc-flake.md). Remaining: non-value module statics (native handles, caches like `compiled_regex_cache`, `g_active_servers` lists) are still process-global, and `js->sym` fields are still rooted via per-field `gc_register_root` rather than central marking.
  - Impact: Remaining statics only matter if multi-isolate embedding is ever supported.
  - Proposed fix: If multi-isolate lands, sweep the remaining non-value module statics onto `ant_t` and migrate `js->sym` registration to central marking.
  - Status: mostly resolved; remainder parked (pending multi-isolate decision)

- Area: `src/gc/objects.c` — process-static collection state
  - Issue: The GC's mutable working state is shared across the process: `gc_mark_stack` and its indices, `gc_epoch`, the 8-bit `gc_obj_epoch`, `g_minor_gc`, `g_str_mark`, and function-mark profiling state. Concurrent collections from same-process isolates can race on that state. Sequential multi-isolate collection is also unsafe after `gc_obj_epoch` wraps because `gc_obj_epoch_wrapped()` clears mark bytes only in the isolate that triggered the wrap; another isolate can retain a stale byte that later compares equal to the global epoch.
  - Impact: The current `worker_threads` implementation uses `uv_spawn`, so its workers are separate processes and unaffected. Native embedders can create multiple isolates in one process, however: concurrent use can corrupt collection state, while an epoch collision during interleaved sequential collections can make the marker skip an object and its outgoing edges.
  - Proposed fix: Move collection-local mutable state onto `ant_t` or an isolate-owned collection context, including the mark stack, epochs, minor/major mode, string marker, and profiling/latching state. Make epoch access isolate-aware and clear each isolate's mark bytes independently. Document the embedding threading contract and add a C regression that drives two isolates through more than 254 interleaved collections; add a concurrent-collection test only if concurrent isolate use is supported.
  - Status: backlog (pre-existing; required before same-process multi-isolate collection is supported)

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
  - Impact: A native allocation failure during ordinary closure creation causes undefined behavior, normally a NULL-pointer crash, instead of following the existing `T_ERR` OOM path. The external-array bug needs only five captured values; it is OOM-only, not an unreachable closure shape.
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
  - Issue: `node:worker_threads` is still a minimal compatibility implementation, and `Worker.postMessage` remains explicitly unimplemented. The file header points here; keep the two in sync if the scope changes.
  - Impact: Build tools and libraries that rely on real worker thread messaging or broader worker lifecycle behavior still cannot use the native surface directly.
  - Proposed fix: Expand worker thread support incrementally, starting with message passing and the most commonly used worker APIs, while preserving the existing lightweight process-backed architecture where practical.
  - Status: backlog

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

- Area: `src/modules/regex.c` / remaining RegExp divergences from node
  - Issue: A 68-probe node-differential harness (2026-08-06 validation of the regex rework) leaves 15 pre-existing divergences, all reproduced identically on the installed release and on pre-rework binaries — untouched by and out of scope for the rework. Families: (1) exec-result `groups` is an accessor property where node has a plain writable/enumerable/configurable data property (affects `Object.getOwnPropertyDescriptor` shape on `exec`/`d`-flag results); (2) `$<name>` named-group references in string replacements are not substituted (`'abc'.replace(/(?<m>b)/g, '[$<m>]')` emits the literal text) and the function-replacer named-`groups` trailing argument is missing; (3) legacy statics `RegExp.leftContext`/`rightContext` are undefined after matches ( `$1..$9`/`lastMatch` work); (4) subclass/own-property dispatch: an own or subclass `global`/`flags` getter and a subclass `exec` override are bypassed by the built-in `@@match`/`@@replace` fast paths (a swapped `RegExp.prototype.exec` data property IS honored); (5) `lastIndex` string values are not coerced before use, `lastIndex` is defined non-configurable so accessor redefinition throws where node allows it pre-first-exec; (6) `new RegExp('').source` is `""` instead of `"(?:)"`, and `/(?:)/g`-style empty-match iteration over non-BMP text without `u` advances by code point instead of code unit.
  - Impact: Silent wrong values or missing substitutions for code relying on named-group replacement strings, legacy statics, or RegExp subclassing; all are node-observable divergences that a differential test will keep flagging. None affect the perf-critical exec/match/replace fast paths' results for ordinary regexps.
  - Proposed fix: Fix per family, cheapest first: `source` normalization and `lastIndex` ToNumber coercion are one-liners; `$<name>` substitution + replacer `groups` argument extend the existing `repl_template`/replacer marshaling with the already-cached named-groups meta; `groups`-as-data-property is a result-shape change (canonical shape already exists — add the slot); leftContext/rightContext extend `update_regexp_statics`; subclass/own-getter dispatch requires the batch/fast-path guards to also check `global`/`flags`/`exec` own-or-overridden state (guards already exist for `exec` data-property swaps — extend to accessors and subclass prototypes).
  - Owner: theMackabu
  - Status: backlog (pre-existing node-parity gaps; validation harnesses in /tmp/v_regex*.cjs shapes, recreate if wiped)
