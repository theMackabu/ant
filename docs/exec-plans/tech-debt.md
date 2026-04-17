# Technical Debt Tracker

Status: active
Last reviewed: 2026-04-09
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

- Area: `src/modules/readline.c`
  - Issue: Rendering assumes readline owns the full visible prompt line, so redraws are anchored to the logical prompt text instead of the terminal position where editing actually begins.
  - Impact: Full redraw paths can clobber externally rendered prefixes, boxed prompts, or other same-line UI written before `question()` or `prompt()` starts editing.
  - Proposed fix: Track an explicit render origin / prompt anchor, separate logical prompt text from the screen position where input begins, and make redraws preserve external prefixes and custom prompt chrome.
  - Status: open

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
  - Issue: `node:worker_threads` is still a minimal compatibility implementation, and `Worker.postMessage` remains explicitly unimplemented.
  - Impact: Build tools and libraries that rely on real worker thread messaging or broader worker lifecycle behavior still cannot use the native surface directly.
  - Proposed fix: Expand worker thread support incrementally, starting with message passing and the most commonly used worker APIs, while preserving the existing lightweight process-backed architecture where practical.
  - Status: backlog

- Area: `src/modules/async_hooks.c`
  - Issue: `node:async_hooks` is still a minimal compatibility layer intended mainly to satisfy framework expectations.
  - Impact: Async context tracking semantics remain shallow, which can break libraries that rely on realistic async IDs, resources, or hook lifecycle behavior.
  - Proposed fix: Replace the placeholder async ID and resource behavior with real runtime-backed tracking, while keeping `AsyncLocalStorage` compatibility stable during the transition.
  - Status: backlog

- Area: `src/streams/readable.c`
  - Issue: `ReadableStreamBYOBReader` is still explicitly unimplemented, and byte-source support is still called out as incomplete.
  - Impact: Web Streams byte-oriented consumers cannot rely on BYOB reader semantics, leaving an important platform feature gap for stream-heavy or browser-compatible code.
  - Proposed fix: Add real byte-source plumbing and implement `ReadableStreamBYOBReader` on top of it instead of routing byte sources through the default reader path.
  - Status: backlog
