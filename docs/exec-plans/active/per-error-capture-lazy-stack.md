# Per-Error Capture and Lazy Error Stacks

Status: active
Last reviewed: 2026-10-09
Owner: theMackabu

## Goal

Make `err.stack` Node-compatible and cheap. Each error records its own frames
and source site when it is created. Text is rendered only when it is read or
printed. The isolate-wide `js->errsite` global is gone, so a site can no
longer go stale or be clobbered by a nested call.

## Problem

- `err.stack` held the colored uncaught render: a `file:line:col` line, a
  highlighted source excerpt, a caret and ANSI codes. There was no
  `Name: message` first line.
- `util.inspect(err)` printed `TypeError {}`.
- The Error, AggregateError and SuppressedError constructors stamped an own
  `name` from `new.target`, so `class MyErr extends Error {}` reported
  `"MyErr"`. Node reports `"Error"`. These three are linked: the own `name`
  existed so that the eagerly built header would show the subclass.
- Every `new Error()` and every internal error paid for the full colored
  render (about 11 µs), even when the error was caught (see the
  "error stack text" entry in [tech-debt.md](../tech-debt.md)).
- `js->errsite` is ambient state. It goes stale after a handled error, any
  nested JS call can clobber it, and it pins code units by hand.

## Step 1 — capture record and lazy `stack` (done 2026-10-09)

- `js_capture_stack` stores a record array in `SLOT_ERROR_STACK`. The record
  holds the site (`kTypeFunctionInfo` of the site function, source position,
  span, line, column), and three fields per VM frame: the function as
  `kTypeFunctionInfo`, the callee, and the bytecode offset. It also holds a
  fallback file string when a frame has no function filename. `kTypeFunctionInfo`
  values are traced by `gc_mark_value`, so the record keeps the code units it
  needs alive without pins.
- Sites that have no code unit (lexer and `node:syntax` errors) render their
  context prefix eagerly into the record. Those errors are rare.
- `stack` is an own accessor (Node 26 does the same), shared via `js_mkfun`.
  The first read builds `Name: message\n    at fn (file:line:col)` from the
  current `name` and `message` and caches it in `SLOT_ERROR_STACK_TEXT`.
  Assignment stores the value there and drops the record. The getter walks
  the prototype chain, so `Object.create(err).stack` works.
- Printers (`io_print_error_stack`) render the colored output from the record
  with `js_error_render_pretty`, but only while `SLOT_ERROR_STACK_TEXT` is still
  the generated value. A user-assigned or redefined `stack` prints verbatim.
  `io_print_error_props` reads the slot, never the property, so a user
  `stack` getter is not called while reporting.
- `Ant_Exception_Stack` materializes lazily. `js_throw` skips the eager
  throw-site render for errors that carry a record.
- The constructors and `make_error_value` no longer define an own `name`.
  `name` comes from the prototype.
- `util.inspect` and nested inspection print the stack in Node's shape:
  the `Class [name]` header, brackets when there are no frames, indentation of
  nested stacks, own enumerable props, and `[cause]`/`[errors]`. They drop
  own `name`/`message`/`stack` keys whose value already appears in the stack.

Validation: uncaught, unhandled-rejection and `console.log`/`console.error`
output is byte-identical to `44430c01` under `FORCE_COLOR=1` and `NO_COLOR=1`
for 14 scripts: nested throws, null reads, primitive throws, rejections,
syntax errors from `new Function`/`eval`/`JSON.parse`, ESM fs errors, stack
overflow, async throws, a custom `stack`, and errors with props. The one
intended difference is subclass headers, which now print `E2 [Error]: sub`
like Node. Harness: 361/362, with `test_hono_adapter` failing on master too
(missing `packages/hono/dist`). Spec `--all`: 103/103.
`tests/test_error_stack_format.cjs` covers the new behavior.

Timing (100k iterations, same build config, `44430c01` → step 1):
`new Error` 1.1 s → 24 ms, caught `throw new Error` 1.2 s → 23 ms, caught
internal `TypeError` 1.5 s → 37 ms, `new Error().stack` 2.1 s → 68 ms.
Caught `generator.throw(5)` is unchanged at 1.9 s, because primitive throws
still render eagerly.

## Step 2 — remove `js->errsite` (done 2026-10-09)

- `js_error_site_t` is a value type and no longer an isolate field. It is
  filled by `js_error_site_from_bc`, `js_error_site_from_frame`,
  `js_error_site_from_vm_top` (`src/silver/stack_trace.c`) and
  `js_error_site_from_source`, and passed to `js_create_error_at` /
  `js_mkerr_typed_at`. A `NULL` or invalid site means the newest frame.
- Precise sites are now passed directly: the lexer (`sv_lexer_error_site`,
  and the parser's `SV_MKERR*` macros through it), `ant:syntax` script-mode
  errors, the nullish property and element reads in
  `src/silver/ops/property.h`, and the JIT element read in `src/silver/glue.c`.
- Removed: `js_error_site_save`/`restore` (accessor and setter calls in
  `src/ant.c`, `inspector_append_call_location`), every
  `js_clear_error_site`, the `errsite.unit` clearing in
  `src/silver/code_unit.c`, and two JIT helpers that set the site after the
  error already existed (`jit_get_field_fallback`,
  `jit_helper_import_named`). Those two were a real source of stale sites:
  after a caught nullish read in JIT code, the next unrelated error reported
  the old read's location in its header.
- `js_get_call_location` (`reportError`, the inspector) resolves the VM top
  frame each time and no longer leaves a site behind.
- `tests/test_code_unit_lifetime.c` now checks the new invariant: an error
  keeps its unit alive while the error lives, its source excerpt still
  renders after a collection, and the unit is freed with the error.
  `tests/test_error_stack_format.cjs` covers the stale-site case.

Validation: the step 1 byte-for-byte set is unchanged. Nine more scripts
match `44430c01` byte for byte: strict-mode octal escapes, interpreter and
JIT nullish element and field reads, errors in setters, getters and setters
that catch nullish reads, `ant:syntax` script-mode import, and ESM parse
errors. Harness 361/362 (hono, as before), spec `--all` 103/103, and 31 of 34
runnable C tests. `gc-rope-table-oom`, `ic-poly-shape-release` and
`arguments-storage` fail identically at `44430c01`; `jit-address-modes` and
`jit-property-ic` do not compile against the current JIT emitter signatures;
`desktop-error-details` needs `packages/desktop/runtime/ant_runtime.h`, which
is missing. Timing is unchanged from step 1.

## Step 3 — compiled frames from the native stack (2026-10-09)

Compiled code pushes no VM frames, so captures used to miss every compiled
activation, and the VM top frame of an OSR-entered function kept the `ip` of
its OSR entry (an error after a hot top-level loop reported the loop's line).
Compiled frames are now recovered from the native stack when a stack is
captured. Compiled code itself does no bookkeeping.

- MIR fork patch `vendor/packagefiles/patches/mir-call-site-info.patch`:
  `MIR_op_t` gets a `tag` in its padding (`ops[0]` of a call insn carries it;
  kept by copies); `target_translate` on aarch64 and x86_64 records the
  return-address offset of every tagged call, and
  `MIR_gen_set_code_info_func` reports the code range, those offsets and the
  frame-pointer-to-CFA distance after each function is generated.
- While a body is emitted, `src/jit/jit_internal.h` tags every call insn with
  an index into a per-compile site table: the bytecode offset plus up to four
  levels of inlined callees and their offsets (the inliner keeps the stack in
  `jit_call_tagging`).
- `src/jit/unwind.c` keeps a per-isolate registry: code range → `sv_func_t`
  and the function's call sites in one block (a varint stream of
  return-offset delta, zigzag bytecode delta and inline depth, with a
  checkpoint every 32 sites, plus the inline frames; ~3 bytes per site).
  This is the same encoding as V8's `SourcePositionTable`. A capture keeps
  (function, return address) per compiled frame; `sv_jit_frames_at` decodes
  the bytecode offset and inlined callees when the stack is formatted.
  Compiled functions and their inlined callees are immortal, so the record
  stays valid. Off Apple, each function also gets a DWARF FDE (libgcc's
  `__register_frame` takes the CIE+FDE section, LLVM libunwind's
  `__unw_add_dynamic_fde` the FDE), all of them at the first walk and later
  ones as they are compiled; Apple never registers. The entry stub
  (`src/jit/entry_stub.c`) has CFI. Ranges are dropped in `sv_jit_destroy`;
  compiled code is never freed earlier.
- Entering compiled code records nothing. The walker places compiled frames
  between VM frames with the interpreter segments the collector already keeps
  (`gc_vm_seg_t`, which now also stores the segment's first VM frame): a
  compiled frame deeper than one segment and older than the next was entered
  while the VM frame below the newer segment was on top. The OSR entry
  (`sv_jit_try_osr`, never inlined) pushes an `sv_jit_osr_mark_t` (the
  compiled frame just deeper is the OSR host, which is skipped), a cold
  path. A bailed-out activation that continues in the interpreter is hidden
  in favour of its VM frame: it is stopped at one of the calls into the
  resume helpers, which are flagged when emitted, so the range records their
  return offsets and the walker recognizes them.
- Walking: on Apple targets every frame keeps a frame record, so the
  frame-pointer chain is followed directly (bounded by the thread's stack top
  and required to move up the stack). Elsewhere `_Unwind_Backtrace` walks C
  frames built without frame pointers. Each entry into compiled code
  (`js->jit_active_depth`) leaves one run of compiled frames, so the walk
  stops after the oldest run, or after `Error.stackTraceLimit` compiled
  frames. Captures only walk while compiled code is active, so errors in
  interpreted code are unaffected.
- `sv_stack_iter_t` (`src/silver/stack_trace.c`) merges the compiled activations
  with VM frames; errors, callsites (`prepareStackTrace`), raw stacks and the
  VM-top site lookup all use it. A VM frame that called the frame above it
  from the interpreter loop resumes after the call op, so it is looked up one
  byte earlier (the segments tell which frames those are); compiled code
  called straight from the interpreter now leaves the caller's `ip` at the
  call op, like native calls. Both report the call op's position.
- A compiled frame has no closure to read a name from. Functions without a
  source name record the name they are given at runtime
  (`js_define_function_name_value`: computed keys, accessors) in
  `debug->display_name`, and mark it as varying once two closures get
  different names; a compiled frame then reports `<anonymous>` rather than a
  name that may belong to another closure. Interpreted frames read the
  callee's own `name` data property, without running getters.

Validation: on macOS aarch64 and in a Linux musl aarch64 container (static
build, libgcc unwinder), stack traces of 16 scenarios match `--jitless`
output (`tests/test_error_stack_jit_frames.cjs` keeps seven of them).
They cover compiled chains, inlined callees, OSR in a function and at top
level, native callbacks, bailouts, getters, `prepareStackTrace`, `console.trace`
and stack overflow. The only differences are frames the interpreter removes
by proper tail calls (`return f()`), which compiled code keeps, as V8 does.
Harness 363/364 locally (hono needs its dist build), spec 103/103, the 142 JIT/codegen tests pass on both
platforms. Fixed-work bench-v8 against the step 2 build: total instructions
−0.08%, cycles −0.1% (noise). Creating an error while compiled code is on the
stack costs about 0.3 µs on macOS (0.25 µs in step 2) and about 2.5 µs on
Linux, where the unwinder walks every C frame (0.5 µs `--jitless` in the same
unoptimized container build).

Cost against master with the unreachable-code fix (`d9646559`), TypeScript
workload with ~1,700 compiled functions and 82k call sites: JIT compile time
−0.1% (medians of 5 runs per function), peak RSS +1.4 MB median (runs vary
by ±1.5 MB; the table itself is ~0.5 MB by its layout). Storing the sites as
16-byte structs and registering FDEs eagerly had cost +2–3 MB.

A/B against master in one no-PGO build dir (instructions retired, min of 3):
bench-v8 fixed-work total −0.04% (every test within ±0.1%), GC fixtures
unchanged, 10M native callbacks (`a.map(f)`) −0.06%, compile-heavy script
(3,000 compiles) +0.03%, TypeScript +0.07%, Newt Prelude +0.05% (spread
±0.1%). Two earlier versions regressed and were changed: a C-side marker
pushed in `sv_jit_invoke` cost 13 instructions per C→JIT entry (+3.9% on
callbacks), now replaced by the interpreter segments; a `tag` field on
`MIR_insn` grew every insn by 8 bytes and cost +0.4% per compile, now kept in
`MIR_op_t` padding.

A first version kept per-activation records in the JIT prologue and stored
the bytecode offset before every helper call; it cost +1.5% cycles over
bench-v8 (richards +2.8%) and was replaced by this design.

V8 needs no separate table for this: an optimized frame's return address
finds a safepoint entry, whose deoptimization index gives every frame at
that call site (inlined ones included) with its bytecode offset
(`OptimizedJSFrame::Summarize`). Both tables exist anyway, for lazy deopt and
precise GC. Ant has neither for compiled code yet, so the call-site table
should become part of the stack maps when compiled frames are made precise
(see the GC native-stack scan entry in [tech-debt.md](../tech-debt.md)):
one record per safepoint holding the tagged slots and the bytecode offset.

## Review fixes (2026-10-10)

- Capture records hold at most `Error.stackTraceLimit` frames (read from the
  original Error constructor without getters; a non-number captures no
  stack, as in V8), three values per frame, and no site unless one is given:
  the site is derived from the newest frame when the stack is formatted.
  Source positions are found by binary search. The bottom module-wrapper
  frame is dropped at capture.
- Primitive throws keep a capture record on the exception record;
  `Ant_Exception_Stack` renders the colored text on first use.
- Every render path lists the site as a frame only when it lists no other
  frame (it used to also do so whenever the VM had one frame, which repeated
  the top-level frame).
- The `stack` accessor is added through a shape transition keyed by its
  getter and setter (`ant_shape_add_accessor_tr`, `js_define_accessor_desc`)
  and `message` with its attributes directly, so errors share shapes; the
  internal slots are reserved once.
- The inspector reports an error's own `stack` value in
  `Runtime.getProperties`, and `js_throw` snapshots stack text that was
  already formatted, as master did with the data property.

## Follow-ups and known gaps

- Compiled frames of a function whose closures were given different names at
  runtime (`{ [n]: function () {} }` in a factory) print `<anonymous>`; the
  interpreter prints each closure's name.
- `Error.prepareStackTrace` only runs for `Error.captureStackTrace`, not for
  errors' own `stack` (V8 calls it for both). This already happens on master.
- `NO_COLOR=1` does not disable color in the uncaught render even when
  stderr is piped. This already happens on master.
- `DOMException` still defines own enumerable `name`, `message` and `code`.
  Node uses prototype getters.
- Windows does not walk compiled frames yet (`sv_jit_collect_activations`
  returns none): it needs `RtlAddFunctionTable` entries and an
  `RtlVirtualUnwind` walk. Errors created in compiled code there still use
  the VM top frame, including the frozen OSR `ip`.
- Linux captures with compiled code on the stack cost ~2.5 µs because the
  unwinder walks C frames from their CFI. Building the runtime with frame
  pointers on Linux would allow the Apple walk there.
- x86_64 (MIR call-site offsets, `rbp`-based CFA) is built but has not been
  exercised on hardware yet; CI covers it.
- Frame names differ from Node (`<function>` for module scope instead of
  `Object.<anonymous>`). `prepareStackTrace` CallSite arrays omit frames
  that Node includes.
