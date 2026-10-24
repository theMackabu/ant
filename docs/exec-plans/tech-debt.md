# Technical Debt Tracker

Status: active
Last reviewed: 2026-10-24
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

- Area: Reflect.set receiver and success semantics (`src/modules/reflect.c`, property-setting internals)
  - Issue: Reflect.set ignores its optional receiver and returns true after js_setprop unless an exception occurs. js_setprop returns the assigned value even for some rejected writes, so its result cannot be interpreted as a success boolean. Existing own-property read-only checks do not cover inherited restrictions, non-extensible receivers, or falsy proxy set traps.
  - Impact: Accessors can receive the wrong this value, data writes can modify the target instead of the receiver, and callers can observe success when nothing was written.
  - Proposed fix: Address in a separate compatibility PR. Share a receiver-aware internal [[Set]] operation with explicit success/rejection/exception results; retain assignment-expression values and strict-mode throwing behavior at their callers. Avoid duplicating the property-definition and proxy machinery in a Reflect-only implementation. Preserve thrown-value identity and GC roots across callbacks.
  - Validation: Compare with Node for inherited setters and read-only properties, distinct and primitive receivers, receiver-side accessors, frozen/non-extensible objects and arrays, proxy set/defineProperty traps and transparent fallbacks, revocation, SameValue invariants, and exception propagation. Run focused tests and the full spec suite before landing.
  - Owner: theMackabu
  - Status: backlog (2026-09-05); unfinished Reflect-specific implementation and test removed from this PR at user request. The earlier frozen-own-property fix remains. Recoverable local experiment: `/tmp/ant-reflect-deferred.0pgnxD` (not a validated implementation or durable dependency).

- Area: JS path performance (`src/builtins/node/path.cjs`, Silver runtime)
  - Issue: The compatible JS implementation beats installed Ant on three of eight path microbenchmarks, but normalize, join, and relative remain 3.7-7.2x slower. Performance parity is deferred; Node-compatible correctness takes priority over speed. Keep primordial protection and do not trade semantics for benchmark gains.
  - Impact: Current timings are roughly 0.4-1.5 microseconds per call. This is a substantial microbenchmark gap, but does not establish an application-level regression. Profile representative applications before adding runtime complexity; path-heavy bundling, crawling, and module resolution are useful candidates.
  - Proposed fix: Revisit only with measured application impact. Profile the remaining JIT/string costs, retain only repeatable wins, add compatibility regressions, and compare pinned installed/current/Node binaries serially with ABBA ordering. Do not revive the removed hoisting, grouped-initializer, or register-argument experiments without new evidence.
  - Owner: theMackabu
  - Status: backlog (2026-09-05; accuracy first)
  - Evidence: Accepted-only checkpoint, median milliseconds per 100,000 calls, 20,000 warmups, 12 samples per binary; lower is better. Hashes and raw runs are in `/tmp/ant-path-goal.KC4jj3/three-way-results.json`. These measurements predate the reserved-name compatibility fix. Details and experiment history: [path performance plan](active/path-node-performance.md). All 4,136 spec tests and 6,090 Node differential cases passed at this checkpoint; finite coverage is not proof of complete compatibility.

  | Workload | Baseline `$(which ant)` | Current `./build/ant` | Node |
  | --- | ---: | ---: | ---: |
  | posix.normalize | 9.19 | 37.97 | 8.17 |
  | win32.normalize | 10.38 | 74.28 | 14.72 |
  | posix.join | 14.62 | 73.63 | 18.15 |
  | posix.resolve (cwd) | 945.52 | 147.84 | 32.54 |
  | posix.relative (absolute) | 25.30 | 93.40 | 31.88 |
  | win32.resolve (absolute) | 960.83 | 113.88 | 20.72 |
  | win32.relative (absolute) | 27.67 | 142.84 | 36.54 |
  | posix.parse | 23.10 | 19.01 | 2.39 |

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
  - Issue: `JSON.stringify(globalThis)` can throw when it reaches the global `localStorage` property and no `--localstorage-file` or `localStorage.setFile()` path has been configured. Repro: `console.log(JSON.stringify(this));` currently reports `TypeError: localStorage requires a file path: pass --localstorage-file or call localStorage.setFile(path)`
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

- Area: `src/modules/intl.c`
  - Issue: `Intl` is now present and passes the current compat-table target, but several behaviors are still simplified compatibility implementations rather than fuller ECMA-402 semantics.
  - Impact: `Intl.Collator`, `Intl.NumberFormat`, `Intl.DateTimeFormat`, and `Intl.Segmenter` can still diverge from web or Node behavior for anything beyond the currently covered compat surface.
  - Proposed fix: Continue expanding `Intl` incrementally: replace `strcoll`-only collation, deepen `resolvedOptions()`, make `DateTimeFormat` actually honor stored timezone and locale options, and move `Segmenter` closer to the expected iterable/result object shape.
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

- Area: `src/modules/async_hooks.c`
  - Issue: `node:async_hooks` is still a minimal compatibility layer intended mainly to satisfy framework expectations.
  - Impact: Async context tracking semantics remain shallow, which can break libraries that rely on realistic async IDs, resources, or hook lifecycle behavior.
  - Proposed fix: Replace the placeholder async ID and resource behavior with real runtime-backed tracking, while keeping `AsyncLocalStorage` compatibility stable during the transition.
  - Status: backlog

- Area: Native cfunc ABI / iterator `next()` trampolines
  - Issue: The lightweight native-function ABI (`ant_cfunc_t`, registered via `Ant_Function_Create` with a static `ant_cfunc_meta_t`) has no per-registration data channel — no userdata pointer and no QuickJS-style `magic` int. Any family of native callbacks that share logic but differ by one parameter therefore needs a named trampoline per variant. Current example: the six `*_iter_next` wrappers (array, string, map, set, typed array, headers) that each just call `js_iter_next_result(js, advance_*)`. That helper was introduced by PR #40 (issue #39) to fix unspecified-evaluation-order reads of an uninitialized `ant_value_t` in the old `js_iter_result(js, advance_*(js, &it, &value), value)` pattern, which GCC on Linux miscompiled (stale `value` read before the advance call) while Apple clang happened to evaluate in the safe order.
  - Impact: Source-level boilerplate only. The trampolines are zero runtime cost: `js_iter_next_result` is `static inline` and the advance function is a compile-time constant at each call site, so calls are devirtualized and inlined, and each wrapper keeps a distinct symbol in profiles and stack traces. Alternatives considered and rejected: `js_heavy_mkfun_native()` binding the advance pointer to one generic callback (adds a slot load plus an indirect call on the hot iterator path, allocates a heavier function object, requires a function-pointer-through-`void *` cast, and collapses profiler frames); macro-generated wrappers (pure taste, less greppable).
  - Proposed fix: Only if per-function registration data is wanted in several more places, add a QuickJS-style `magic` int to `ant_cfunc_meta_t` and pass it through to callbacks, letting one generic `iter_next` dispatch through a static advance table with no runtime pointer chasing. This is an engine-wide ABI change — every builtin signature, the call dispatcher, and the JIT native-call convention — so it must not be done just to delete the six iterator trampolines.
  - Owner: theMackabu
  - Status: backlog (deliberately deferred; revisit only alongside a broader cfunc ABI change)

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
  - Issue: Remaining node-differential divergences, all pre-existing on the installed release: (1) exec-result `groups` is an accessor property where node has a plain writable/enumerable/configurable data property (affects `Object.getOwnPropertyDescriptor` shape on `exec`/`d`-flag results); (2) legacy statics `RegExp.leftContext`/`rightContext` are undefined after matches (`$1..$9`/`lastMatch` work); (3) no match can start between the two halves of a surrogate pair, because PCRE2 matches the UTF-8 (WTF-8) bytes and that position has no byte offset: a non-`u` empty-match advance over an astral character, or a user-assigned `lastIndex` pointing at a low surrogate, rounds up to the next code point (`'😀'.matchAll(/(?:)/g)` gives indices `[0,2]`, node `[0,1,2]`; `'😀'.replace(/(?:)/g, '-')` gives `-😀-`). Fixed 2026-10-22 in 929a7f82: `lastIndex`, `index` and `d`-flag `indices` are UTF-16 code units (`tests/test_regexp_utf16_offsets.cjs`). Also fixed 2026-10-22: `$<name>` substitution, the replacer's trailing `groups` argument, and `lastIndex` ToLength coercion (`tests/test_regexp_replace_named_groups.cjs`, `tests/test_regexp_lastindex_coercion.cjs`); `source` normalization, own/subclass `flags`/`exec` dispatch and `lastIndex` accessor redefinition already matched node by then.
  - Impact: Silent wrong values for code relying on legacy statics, `groups` descriptor shape, or non-`u` empty matches and `lastIndex` positions inside astral characters.
  - Proposed fix: `groups`-as-data-property is a result-shape change (canonical shape already exists — add the slot); leftContext/rightContext extend `update_regexp_statics`. (3) needs matching that can start inside a surrogate pair: either a UTF-16 subject for non-`u` patterns that can match a lone surrogate or the empty string, or a PCRE2 16-bit code path; decide only if a real workload hits it.
  - Owner: theMackabu
  - Status: backlog

- Area: compiled-code lifetime — permanent strings, BigInt literals and pinned template objects (`src/runtime.c` code arena, `js_mkstr_permanent`, `js_mkbigint_permanent`, `gc_pin_permanent`)
  - Issue: Compiled functions (`sv_func_t`, bytecode, constants) live in the code arena and are never freed individually. Every compile-time string literal, including tagged-template cooked and raw strings, is allocated by `js_mkstr_permanent` in `js->pool.permanent`, which is only released at isolate teardown. BigInt literals are the same: the compiler builds them with `js_mkbigint_permanent` (`src/silver/compiler.c`), whose payload comes from `js_permanent_alloc` in the same pool, because the collector does not trace constant tables and a collected payload would be swept while the function still refers to it. Runtime BigInts (arithmetic results, `BigInt()`, conversions) stay in the collected BigInt pool; only the `permanent` flag threaded through `bigint_alloc_payload_in`, `js_mkbigint_limbs_in` and `bigint_from_string_digits` separates the two. Tagged-template objects (`OP_PUT_CONST`) and JIT object-literal templates (`jit_helper_object_template`) are kept alive with `gc_pin_permanent`, which is sound only because the code that owns them is immortal. Code compiled repeatedly at runtime (`eval`, `new Function`, VM scripts, template engines that compile on the fly, REPL input, hot module reload) therefore grows memory without bound, including pinned objects that every major collection traces. V8 instead keeps template objects alive through the code that owns their site and collects both when that code dies.
  - Impact: Unbounded memory growth for programs that compile code repeatedly. The pins also add major-collection tracing proportional to their count: on a GC-heavy churn benchmark, 16,000 pinned template objects cost about 4-5%, which matches keeping the same objects alive through a plain array. The string part of that cost is removed by the permanent-string marking fast path (`STR_META_PERMANENT`).
  - Proposed fix: Give each compiled unit (script, module, or function tree) an owner the collector can reclaim once no closure, frame, or JIT code references it. Allocate the unit's literal strings and BigInt literal payloads in that unit's own arena instead of the global permanent pool, and replace `gc_pin_permanent` for template objects with ownership by the unit: traced from it and freed with it. Keep `STR_META_PERMANENT` meaning "memory owned outside the collector"; it stays valid because the collector still never frees those strings individually, their unit does. The permanent-root vector then only holds truly isolate-lifetime objects.
  - Validation: A loop compiling `new Function`/`eval` sources that contain tagged templates, object literals and BigInt literals must reach steady-state RSS. Template identity per call site must hold for as long as the code is alive, and a GC torture pass must cover reclaiming a unit while one of its closures is still on the stack.
  - Owner: theMackabu
  - Status: in progress (2026-09-23). Code from `eval` and the Function constructors is now reclaimed through code units ([Reclaim Dynamic Code](active/reclaim-dynamic-code.md)); scripts, modules, the REPL, the atom intern table and JIT machine code are still immortal. Related: the cross-isolate code-arena entry above.

- Area: GC native-stack scan — compiled (MIR) frames are still scanned conservatively (`src/gc/objects.c` `gc_scan_current_stack`/`gc_scan_segmented`, `include/gc/objects.h` `gc_vm_seg_t`, `src/jit/entry_stub.c`)
  - Issue: Interpreter activations are skipped by the stack scan and the event loop keeps libuv's poll buffer free of JS values, but compiled frames are still scanned word by word. A stale word in a compiled frame (a dead spill slot, or an interpreter register saved by a JIT prologue) counts as a root. Retention only grows when such a word stays at the same address across several majors: one stale pointer to a removed node keeps every node removed after it alive, and the chain grows each major. Compiled frames rarely qualify because they are short-lived and the next call at the same depth rewrites them, so making them precise was deferred. Per-word attribution on canonical Splay (2026-09-22) found every retaining word in the interpreter's or `uv_run`'s frame and none in the MIR frames active at every major; with those two fixed, post-major live stays flat at about 768.7k objects (it used to climb to 2.86M). The one known compiled-frame path, stale interpreter callee-saved registers saved by a JIT prologue, is handled by `ant_jit_enter_clean` zeroing them on entry from non-compiled code.
  - Impact: Not observed. The exposure is a long-running compiled frame that never returns and keeps a dead pointer in a slot it stops writing, for example an OSR'd outer loop whose spill slot held a node that was later removed. The symptom would be post-major live objects climbing on a workload whose real live set is constant.
  - Proposed fix: Make compiled frames precise. Record GC stack maps in MIR at each safepoint (helper calls and allocation sites): which spill slots and callee-saved registers hold tagged values. Keep a code-range → function registry so the scan can map a return address to its map. At GC time, walk the compiled frames by frame pointer (MIR keeps a frame chain on arm64, with the frame record at the bottom of the frame and the saves at the top), mark only the slots the map lists, and scan any frame without a map conservatively. Once every compiled frame has a map, `ant_jit_enter_clean` and the `vm_top_kind` byte can go, since the saved stale registers would no longer be read. The MIR changes (safepoint maps, the saved-area layout, per-function code ranges) are the bulk of the work.
  - Diagnosis: To check whether a retention report is this issue, instrument a major temporarily. Drain precise roots before `gc_scan_current_stack`, then count the objects the stack scan greys. Scan the stack one word at a time, draining after each, and log any word that greys more than a few hundred objects. Walk the frame-pointer chain with `dladdr` on each saved return address to name the frame each word sits in. Words in `sv_execute_frame` or `uv_run` point at the existing mechanisms. Words in unnamed frames are MIR code, and point here.
  - Validation: A benchmark with a constant live set (canonical Splay; the retention microbenches in the 2026-09-22 session) keeps flat post-major live counts with the conservative JIT-frame scan replaced. `ANT_GC_STRESS` sweeps pass on the verify build with and without `--jitless`. The boundary microbench (`tests/bench_vm_boundaries.cjs`) shows no slowdown on the compiled-code crossings.
  - Owner: theMackabu
  - Status: backlog (2026-09-22); deferred because no workload shows retention through compiled frames.
  - Shared infrastructure (2026-10-09): the code-range → function registry and per-call-site records keyed by return offset now exist for stack traces (`src/jit/unwind.c`, MIR patch `mir-call-site-info.patch` tags call insns and reports their return offsets). The stack maps should extend those records (one per safepoint: tagged slots plus bytecode offset), as V8's safepoint table and deoptimization data serve both GC and stack traces, rather than add a second table.

- Area: error stack text — built eagerly on every throw and every `new Error()` (`src/errors.c` `js_throw`, `js_build_stack_text`, `js_capture_stack`; `Ant_Exception_Raise`)
  - Issue: `js_throw` renders the complete stack trace for any thrown value that has no string `stack` (including primitives like `throw 5` and `generator.throw(x)`) and hands it to `Ant_Exception_Raise` in case the exception goes uncaught. Error construction does the same through `js_capture_stack` (`JS_STACK_TEXT_FROM_ERROR_OBJECT`). Rendering walks every VM frame and formats colored, syntax-highlighted text with a source snippet, and checks the terminal (`ioctl`). The text is discarded whenever the exception is caught, which is the common case. A per-frame `getcwd` used to dominate this; it is now cached per thread against `process_cwd_epoch` (2026-09-22), which cut a caught throw from ~18 µs to ~5.2 µs (100k caught `generator.throw` calls: 1.8 s → 0.52 s). What remains is the formatting itself: colored `crsprintf` (~15% of the original profile), highlighting (`hl_iter_next`, ~5%), terminal checks (~2%), and string building.
  - Impact: About 5 µs per throw and per `new Error()`, paid even when nobody reads the trace. It matters for code that uses exceptions for control flow: iterator and generator protocols (`throw`/`return` into generators), validation libraries that throw and catch per item, retry loops. A caught throw should cost well under a microsecond.
  - Proposed fix: Capture, render later. At throw or construction time, record only the raw frames: `sv_func_t *` plus bytecode offset per VM frame, plus native frame names. That is cheap and safe to keep, because compiled functions live in the code arena and are never freed individually (see the compiled-code lifetime entry). Render the text only when it is needed: the uncaught-exception reporter, the unhandled-rejection printer, and the first read of an Error's `stack` property (an accessor, or a lazily materialized data property, cached after the first read). Keep `JS_ERR_NO_STACK` and user-assigned `stack` strings working as today. V8 works the same way: structured frames at capture, formatting on first `stack` access.
  - Validation: Byte-for-byte identical output against the current build for uncaught errors, unhandled rejections and `console.log(e.stack)`, colored and uncolored, before and after `process.chdir` (the 2026-09-22 check compared `FORCE_COLOR=1` traces from a nested-call script). `Error.captureStackTrace`, `prepareStackTrace`/CallSite behaviour and the error-output harness tests unchanged. Timing: 100k caught `generator.throw` calls and 100k `new Error()` in a loop, before and after, against master.
  - Owner: theMackabu
  - Status: mostly done (2026-10-09). Errors now capture a frame record and render lazily (see [per-error-capture-lazy-stack.md](active/per-error-capture-lazy-stack.md)): `new Error` went from about 11 µs to 0.24 µs. Primitive throws (`throw 5`, `generator.throw(x)`) still render eagerly in `js_throw`. The cwd cache landed first (2026-09-22).

- Area: `src/modules/buffer.c` / resizable `ArrayBuffer` (ES2024) and length-tracking views
  - Issue: `new ArrayBuffer(len, { maxByteLength })` ignores its options; `resizable`, `maxByteLength`, `resize()` and `transferToFixedLength()` are missing, and views cannot track a buffer's size. Related gap with the same fix mechanism: DataView `byteLength`/`byteOffset` and `get*`/`set*` keep working on a detached buffer (stale offset/length, reads succeed) where node throws `TypeError` (checked 2026-10-13), because the JIT-inlined DataView getters use `ANT_NATIVE_DETACH_NONE`.
  - Impact: Code that feature-detects or relies on resizable buffers (streams, Wasm-style growable memory, protocol parsers) gets fixed-size buffers silently, and `resize` is `undefined`.
  - Proposed fix: Reserve `maxByteLength` inline at construction so the data pointer never moves across a resize (untouched pages are lazily zeroed; GC accounting counts the current length, not the reservation, and `resize` adjusts `js->buffers.bytes`). Keep `TypedArrayData`/`DataViewData` `length`/`byte_length`/`byte_offset` as the *effective* values and recompute them eagerly: a resizable buffer keeps a list of its views (allocated only for resizable buffers, views unregister in their finalizers), and `resize` updates every view (length-tracking views get the new length, out-of-bounds fixed views become 0/0/0, each view keeps its declared offset/length to recover). This leaves the ~175 direct readers and the JIT's inlined size getters correct unchanged and costs nothing for fixed buffers. Add a per-view `invalid` byte to `DataViewData` and point the DataView native getter descriptors at it (`ANT_NATIVE_DETACH_SELF`) so inlined reads fall back to the throwing C getter when out of bounds or detached. Then: zero newly exposed bytes on grow, `transfer` preserves resizability, `slice`/`transferToFixedLength` drop it, structured clone preserves `maxByteLength`, and audit the ~40 typed-array methods to treat out-of-bounds like detached. Growable `SharedArrayBuffer` (`grow`/`growable`) is a follow-up on the same view mechanism.
  - Validation: test262 `built-ins/ArrayBuffer/prototype/resize`, `.../TypedArray*/**/resizable*`, `.../DataView/**/resizable*` and length-tracking cases where the harness allows, plus node differentials for shrink/regrow of fixed and tracking views, detach after resize, and JIT-warmed `length`/`byteLength` reads across a resize. Write an exec plan under `docs/exec-plans/active/` when work starts (multi-step runtime invariant change).
  - Owner: theMackabu
  - Status: backlog (2026-10-13; scoped, estimated ~400-600 lines mostly in `buffer.c`)

- Area: `src/modules/assert.c` / object `expected` in `assert.throws` and `assert.rejects`
  - Issue: `assert_exception_matches` checks a constructor, a validator function or a RegExp, and accepts anything else without checking. Node treats an object (or an Error instance) as a property-by-property match: every own key of `expected`, plus `name` and `message` when present, must be deep-strictly equal on the thrown value, and a RegExp value is tested against a string property. Node also rejects an empty object with `ERR_INVALID_ARG_VALUE`. Example: `assert.throws(fn, { code: 'ERR_X' })` passes in Ant whatever `fn` throws.
  - Impact: Tests that use the object form verify nothing, so wrong error shapes go unnoticed. `tests/test_worker_post_message_to_thread.cjs` relies on `assert.rejects(..., { name: 'DataCloneError' })`. The RegExp and validator gaps hid two real bugs (a wrong `test_ant_c.cjs` expectation and a missing `ERR_INVALID_ARG_TYPE` code in `wire.c`), so expect this one to surface similar failures once it is fixed.
  - Proposed fix: Add the object branch to `assert_exception_matches`, after the RegExp check and before the callable check. Fail when the thrown value is not an object, then compare each key of `expected` against the thrown value with the existing `js_deep_equal(..., true)`, testing a RegExp value against a string actual with `assert_regexp`. Report failures as an `AssertionError` with a `+ actual - expected` diff of the compared keys, using the operator passed in (`throws` or `rejects`). Throw `ERR_INVALID_ARG_VALUE` for an empty object.
  - Validation: Node differential for matching and mismatching keys, nested objects, RegExp values, Error instances as `expected`, non-object thrown values, the empty object, and the custom-message argument, for both `throws` and `rejects`. Run every test that passes an object as `expected` and fix any expectation that turns out to be wrong instead of loosening the check.
  - Owner: theMackabu
  - Status: backlog (2026-10-21; RegExp and `rejects` matching landed first)

- Area: ESM loader linking and evaluation (`src/esm/`, compiler and VM module ops, `esm/hooks.c`)
  - Issue: Linking and evaluation do not follow ECMA-262 §16.2.1.5-6. Re-exports (`export { v } from`, `export *`, diamonds) copy values instead of forwarding live bindings; `export default <expr>` is not a binding (reading it before the body runs gives TypeError instead of ReferenceError, anonymous defaults get `.name === ""`); a hoisted function exported from a module in a cycle is not callable before its body runs; importing a missing or ambiguous (two `export *` origins) name is accepted and module bodies run anyway; `export *` re-exports `default`; namespace properties report `configurable: true`; namespace keys are not sorted. Link failures are not cached, and top-level-await ordering, cycle promises and `import()` of a module on the evaluation stack are not spec-ordered.
  - Impact: Code depending on live bindings through barrels or re-exports sees stale values; cyclic module graphs crash with "undefined is not a function"; typos in import names surface as `undefined` at use sites instead of a SyntaxError before anything runs.
  - Proposed fix: Implement Link (instantiate every module's environment, install hoisted functions, resolve and validate every import, all before any body runs) and Evaluate (DFS post-order, strongly connected components as one unit, async-dependency counting for TLA) per the spec, with resolved bindings forwarding to the origin module's slot. Keep link cost linear in modules plus exports. May land as a series of commits, but as one fix. Do not port code from the `solsahar:ant` `@solsahar/runtime-fixes` branch, where these bugs were first spotted.
  - Repros: `tests/fixtures/esm-linking/`, run the named entry with `./build/ant`. Current → expected output: `live/main.mjs` `before` → `after`; `deftdz/a.mjs` `TypeError` → `ReferenceError`; `cycle/a.mjs` `TypeError: undefined is not a function` → `/summer/camp`; `missing/main.mjs` `body ran, ran=1` → SyntaxError with no module body run; `ambig/main.mjs` `k=2` → SyntaxError; `failed/main.mjs` already prints `boom` for both imports (keep it that way, and add the link-failure equivalent).
  - Validation: Turn the repros into a regression test, and add cases for diamond re-exports, namespaces read before and after evaluation, sibling modules not running on link failure, concurrent dynamic imports of a broken graph, the `esm/hooks.c` path, namespace descriptors and key order; full `examples/spec` and every `.mjs` test. Benchmark a synthetic 5,000-module, 20-export graph before and after.
  - Owner: theMackabu
  - Status: backlog (2026-10-22; repros verified on 929a7f82)

- Area: `for...in` enumeration of keys deleted during the loop (`src/silver/compiler.c` `compile_for_in`, `OP_FOR_IN`, JIT)
  - Issue: `for...in` compiles to a snapshot of keys plus an index loop, so a key deleted before the loop reaches it is still visited. Affects own properties (`delete o.b` while visiting `a` still yields `b`), array indices past a shrunk `length`, typed-array indices after the buffer is detached, and inherited keys deleted from the prototype. ECMA-262 §14.7.5.9 requires deleted, not-yet-visited properties to be skipped.
  - Impact: Loops that prune an object while iterating it process stale keys, and reads of those keys return `undefined`.
  - Proposed fix: A for-in step operation that skips keys no longer present, with a cheap "object unchanged since the snapshot" check so unmodified plain objects and arrays don't pay a lookup per key; supported in the interpreter and the JIT. Proxies keep their current trap order.
  - Repro: `./build/ant tests/fixtures/for_in_deleted_keys.js` prints `["a","b","c"]`, `["0","1","2"]`, `["0","1","2"]`, `["own","inherited"]`; node prints `["a","c"]`, `["0"]`, `["0"]`, `["own"]`.
  - Validation: Node differential for own, array, typed-array (detach mid-loop) and inherited cases in interpreter and JIT-compiled code; benchmark `tests/bench_forin_object_keys.js`, a 100-element array and a 20-property object loop before and after. When it lands, add the detach-during-`for...in` case back to `tests/test_typedarray_index_properties.cjs`.
  - Owner: theMackabu
  - Status: backlog (2026-10-22; found while fixing typed-array own properties, deferred)

- Area: `src/modules/regex.c` / `d`-flag `indices` cost on non-ASCII subjects
  - Issue: Every capture's start and end is converted from a byte offset to UTF-16 units when building `indices`. Captures before the match start (lookbehind) fall back to `byte_offset_to_utf16`, which goes through the thread-local scan cursor.
  - Impact: `/.../dg` exec loops on non-ASCII text cost +3.3% instructions vs 16f2cca5 (which reported wrong byte offsets). ASCII subjects are unaffected (-1.1%).
  - Proposed fix: Convert all capture offsets in one pass: sort the 2×ovcount offsets, walk forward once from the match-start anchor counting UTF-16 units, and scatter the results back; or count lazily only for captures that participated.
  - Validation: Instructions-retired A/B against 16f2cca5 built in the same build dir, for a `/w(or)d/dg` exec loop over 1 MB ASCII and non-ASCII strings, plus `tests/test_regexp_utf16_offsets.cjs` and a node differential with lookbehind captures over mixed-width text.
  - Owner: theMackabu
  - Status: backlog (2026-10-22)

- Area: startup instruction count after the 2026-10-22 runtime-fix and perf commits
  - Issue: `console.log(1)` retires about 0.15M more instructions (+0.3%) than 16f2cca5, consistently across same-build-dir runs. Not yet attributed to a change.
  - Impact: Small fixed cost per process start; matters only for short-lived CLI invocations.
  - Proposed fix: Bisect the four edited files in the baseline worktree (apply each file's diff alone, rebuild, compare `instructions retired` for a hello-world, min of 5 runs), then look at what that file runs at init.
  - Validation: hello-world instruction count back within noise (±0.1M) of 16f2cca5.
  - Owner: theMackabu
  - Status: backlog (2026-10-22)

- Area: build with npm 12 (`src/tools/`, `meson/npm_ci.py`)
  - Issue: `src/tools/` ships only `npm-shrinkwrap.json`, and npm 12.2.1 refuses `npm ci` without a `package-lock.json` (`EUSAGE`), so `meson/tools_npm_ci.stamp` fails.
  - Impact: A fresh build fails for anyone on npm 12.
  - Proposed fix: Ship a `package-lock.json` for `src/tools/`, or have `npm_ci.py` fall back when only a shrinkwrap exists. Local workaround: copy the shrinkwrap to the gitignored `src/tools/package-lock.json`.
  - Owner: pgray
  - Status: backlog (2026-10-24)

- Area: build with GCC 16 (`include/native_tag.h`)
  - Issue: GCC 16.2.1 rejects `Ant_NativeTag("ABUF")` as a `constexpr` initializer (`include/native_tag.h:6`, from `include/modules/buffer.h:71`), although BUILDING.md lists GCC 14+ as supported. Clang builds it.
  - Impact: The tree does not build with GCC 16.
  - Proposed fix: Declare the tags `static const` or as enumerators instead of `constexpr`, or give `Ant_NativeTag` four character literals instead of indexing a string literal. Otherwise narrow BUILDING.md's GCC range.
  - Owner: pgray
  - Status: backlog (2026-10-24)

- Area: `ant install` with a `bun.lock` present
  - Issue: `ant install` silently never writes `ant.lock`.
  - Impact: Projects moving from bun keep no Ant lockfile and get no warning.
  - Proposed fix: Write `ant.lock` regardless, or say why it was skipped.
  - Owner: pgray
  - Status: backlog (2026-10-24; found moving pgray.dev from bun to ant)

- Area: `ant trust`
  - Issue: Rewrites `package.json` with 2-space indentation.
  - Impact: Noisy diffs in projects that use other indentation.
  - Proposed fix: Preserve the file's detected indentation when rewriting it.
  - Owner: pgray
  - Status: backlog (2026-10-24; found moving pgray.dev from bun to ant)

- Area: `ant run` child processes
  - Issue: `workerd` children keep running after `ant run preview` is killed.
  - Impact: Orphaned `workerd` processes keep running.
  - Proposed fix: Not investigated yet.
  - Owner: pgray
  - Status: backlog (2026-10-24; found moving pgray.dev from bun to ant)

- Area: `ant.lock`
  - Issue: Does not record which registry each package was resolved from.
  - Impact: A lockfile can't show or reproduce a registry switch.
  - Proposed fix: Record the resolved registry per package.
  - Owner: pgray
  - Status: backlog (2026-10-24; found moving pgray.dev from bun to ant)
