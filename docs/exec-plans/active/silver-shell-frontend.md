# Silver Shell Frontend

Status: active
Last reviewed: 2026-08-16
Owner: theMackabu

## Goal

Replace `ant:shell`'s `popen()` delegation with an Ant-owned shell frontend that
parses shell source, lowers its AST to Silver bytecode, and uses a small native
process substrate for operating-system work. There must not be a second shell
bytecode interpreter or a `/bin/sh -c` execution path.

The long-term language target is non-interactive POSIX `sh` compatibility.
Bash extensions are opt-in additions and do not change the POSIX baseline.

## First milestone

Deliver an end-to-end Bun-style asynchronous API with:

- simple commands and structured template interpolation;
- single quotes, double quotes, and backslash quoting;
- command lists using newline, `;`, `&&`, and `||`;
- concurrent pipelines using `|`;
- `<`, `>`, `>>`, and `2>&1` redirections;
- native `echo`, `pwd`, `cd`, `true`, and `false` built-ins;
- direct executable spawning for other commands;
- byte-backed stdout/stderr and exit status;
- `.text()`, `.json()`, `.arrayBuffer()`, `.bytes()`, `.blob()`, `.lines()`,
  and `.nothrow()` result controls;
- compilation cached by tagged-template identity.

## Architecture

```text
shell source and interpolation slots
    -> shell lexer/parser
    -> shell AST
    -> Silver lowering
    -> async sv_func_t
    -> Silver VM calls private shell intrinsics
    -> ShellContext and libuv process graph
```

The shell compiler owns shell grammar and expansion semantics. It may lower to
generated Silver AST or another private Silver compiler input, but the durable
executable representation is `sv_func_t` bytecode. Native code owns only work
that requires operating-system state or concurrent I/O.

Shell lowering produces a typed native process plan. Both `ant:shell` and
`node:child_process` submit that plan to one process substrate. The substrate
opens redirection descriptors asynchronously, wires pipeline stages with OS
pipes, starts the complete graph, and reports status/output without constructing
JavaScript ChildProcess objects or relaying intermediate bytes through JS.

Pipelines are submitted as a complete process graph. The implementation must
start all stages before awaiting completion; sequentially awaiting individual
stages is incorrect and can deadlock.

## Public API transition

The old API blocks and returns `ShellResult` immediately. The new API follows
Bun's asynchronous shape:

```js
const output = await $`printf hello`;
const text = await $`printf hello`.text();
const failed = await $`exit 7`.nothrow();
```

Resolved commands produce a named `ShellOutput`. Its `stdout` and `stderr`
properties are `Uint8Array` values. Conversion methods decode or expose stdout
without forcing the process substrate through an intermediate JavaScript
string. Shell output remains captured by default.

Nonzero status rejects by default. `.nothrow()` changes only the public promise
policy; the compiled shell always receives exit status as ordinary control-flow
data so `&&`, `||`, `if`, and `!` can work correctly.

Repository examples and tests using the synchronous API move with the change.

## Constraints

- Do not invoke `popen()`, `system()`, `/bin/sh -c`, `cmd.exe /c`, or PowerShell.
- Do not concatenate interpolated JavaScript values back into source text.
- Keep shell variables in a GC-managed `ShellContext`; they are not JavaScript
  lexical bindings.
- Preserve independent cwd and environment state for each shell invocation.
- Stateful built-ins run in the current shell context. Pipeline stages receive
  the POSIX-appropriate isolated context.
- Keep Windows, Linux, and macOS process behavior in the design even if the
  first validation environment is macOS.
- Preserve unrelated worktree changes.

## Task list

1. [x] Add focused parser and public-API tests for the first-milestone grammar.
2. [x] Implement the shell token stream, interpolation tokens, AST, and diagnostics.
3. [x] Add an entrypoint for calling a generated `sv_func_t` through the current VM.
4. [x] Lower shell control flow to ordinary Silver locals, calls, jumps, and await.
5. [x] Implement a GC-managed invocation context, single-command built-ins,
   direct spawn, basic redirections, and concurrent external pipelines.
6. [x] Cache tagged-template compilation in a weak identity map.
7. [x] Replace the `ant:shell` binding and update TypeScript declarations.
8. [x] Migrate in-repository callers to async usage.
9. [x] Run focused shell tests, adjacent child-process and collection tests,
   `maid preflight`, knowledge validation, and the complete spec suite.
10. [x] Add regressions for interpolation abrupt completions, unsupported file
    descriptors, `exit`, embedded NUL, and empty quoted fragments.
11. [x] Replace positional JS-array process IR with a typed native process plan
    shared by `ant:shell` and `node:child_process`.
12. [x] Wire pipelines with OS pipes and redirections with asynchronously opened
    descriptors; remove intermediate EventEmitter relays and whole-file buffers.
13. [x] Remove the dynamic `$(string)` overload and migrate repository callers
    to tagged templates only.
14. [x] Implement WeakMap ephemeron and WeakSet weak-key collection semantics,
    with major and minor GC regressions for the template cache dependency,
    symbol keys, nested maps, and long ephemeron chains.
15. [x] Relocate private shell declarations under `src/modules/shell/`, clean
    formatting, and repeat focused and broad validation.
16. [x] Replace the parallel `uv_process_t` lifecycle implementations with a
    shared native process-stage core. Keep `node:child_process` streams and
    events in its adapter, and keep shell pipeline aggregation in the process
    plan orchestrator.
17. [x] Retain the immutable parsed shell program beside its cached Silver
    function and pass clause indexes to the native runner instead of emitting
    nested JavaScript plan literals. Specialize empty, single-clause, and
    multi-clause generated control-flow shapes.
18. [x] Represent native built-ins as process-plan stages inside pipelines.
    Write their output through the pipeline descriptors asynchronously,
    preserve last-stage status, and isolate stateful built-in contexts.
19. [x] Keep shell capture, built-in results, redirections, and multi-clause
    accumulation byte-backed. Expose a shared `ShellOutput` prototype with
    synchronous conversion methods while preserving asynchronous methods on
    `ShellPromise`.
20. [ ] Extend the Swarm/MIR JIT to async functions, including suspension-safe
    compiled frames, `OP_AWAIT` resume entry, interpreter/JIT handoff, GC
    rooting, exception/finally behavior, and bailout handling across resumes.
    Confirm generated shell functions actually reach JIT code and benchmark
    them before changing hotness thresholds.
21. [ ] Prioritize the next POSIX frontend milestone: implement assignment
    words, parameter and arithmetic expansion, command substitution, and
    `while`, `until`, `for`, `if`, and `case` compound commands. Lower their
    control flow directly to Silver bytecode, preserve shell-context semantics,
    and add differential coverage against `dash` and Bash POSIX mode.
## Deferred POSIX work

- tilde, parameter, arithmetic, field-splitting, pathname, and quote-removal
  expansion in the required order;
- command substitution, here-documents, and here-strings;
- grouping, subshells, functions, loops, `case`, and `if`;
- positional and special parameters;
- `export`, `unset`, `readonly`, `set`, `shift`, `trap`, `wait`, `exec`, `eval`,
  dot/source, and remaining special built-ins;
- signal and process-group behavior, pipeline subshell boundaries, and exact
  POSIX diagnostic/exit rules;
- differential conformance against `dash` and Bash POSIX mode.

## Decision log

- 2026-08-15: Use Silver bytecode as the only language execution engine. Shell
  parsing and OS primitives remain separate components, but there is no shell
  interpreter loop.
- 2026-08-15: Adopt a Bun-style asynchronous public API. The user explicitly
  approved migrating the existing synchronous API and repository callers.
- 2026-08-15: Target POSIX `sh`, not unspecified full Bash compatibility.
- 2026-08-15: Store compiled tagged-template functions in an internal WeakMap.
  Template identity controls reuse without extending a dead template's lifetime.
- 2026-08-15: Preserve ordered descriptor duplication for the supported
  redirections. For example, `2>&1 >file` and `>file 2>&1` have different
  destinations even though cross-stream byte interleaving remains deferred.
- 2026-08-15: Accept only tagged-template calls. Dynamic source strings consume
  monotonic Silver code memory and discard structured interpolation, so the
  legacy `$(string)` overload is removed rather than cached.
- 2026-08-15: Process graphs use native typed plans and kernel pipes. JavaScript
  ChildProcess/EventEmitter objects are public adapters, not an internal shell
  transport.
- 2026-08-15: Weak collections register once per isolate instead of adding type
  checks to ordinary object tracing. Major collections resolve ephemerons with
  a key-indexed worklist. Minor collections process young collections plus
  remembered weak edges, avoiding repeated scans of old WeakMaps. WeakRef
  targets are retained through the current microtask checkpoint.
- 2026-08-16: A process plan is pipeline orchestration, not the shared process
  primitive itself. Extract a native process stage below both adapters so the
  public child-process API retains live streams while shell plans retain kernel
  pipe graphs and aggregate completion results.
- 2026-08-16: Store WeakMap entries in a compact open-addressed identity table
  instead of allocating one UTHash node per entry. During collection, first
  accumulate unresolved ephemerons in a contiguous pair list. Build the
  key-indexed worklist only when the first drain makes a pending key live; the
  common all-dead case therefore avoids hash construction while long and
  reversed ephemeron chains retain linear fixed-point processing.
- 2026-08-16: Cache the immutable native shell AST and its Silver function in
  one weakly held object. Generated Silver selects a clause by index, so static
  words, commands, redirections, and pipeline arrays are not rebuilt as
  JavaScript objects on every invocation. Empty and single-clause programs use
  dedicated lowering shapes; multi-clause programs retain only the accumulator
  and connector control flow they require.
- 2026-08-16: Process plans distinguish external processes from native stages.
  A native stage owns its output and status, writes through the same descriptor
  graph as an external process, and does not mutate the parent shell context.
  This gives `:`, `exit`, `cd`, `echo`, `pwd`, `true`, and `false` pipeline
  semantics without executable lookup or a second shell process.
- 2026-08-16: Keep shell output byte-backed from process capture through public
  settlement. Process plans select text output only for adapters that require
  it; shell redirections and multi-clause accumulation do not decode. Public
  results use a shared `ShellOutput` prototype and expose `Uint8Array` output.

## Validation status

- `meson compile -C build`: passed.
- `./build/ant tests/test_shell.js`: passed, including structured interpolation,
  control operators, stateful `cd`, ordered and large redirections, early
  pipeline exit, failed-stage cleanup, weak-cache reuse, and throw/nothrow
  policy. A 50-run repetition also passed.
- Focused child-process tests selected by `test_child_process*`: passed.
- `./build/ant tests/test_child_process_nul.cjs`: passed.
- A deterministic 552-case differential child-process fuzz run matched the
  pre-change Ant binary for every non-NUL case. `spawnSync`, `execFile`, and
  `exec` result payloads matched Node apart from the already-known ENOENT and
  successful `spawn` event-shape differences. All four embedded-NUL cases now
  reject instead of silently truncating; Node also rejects them.
- `./build/ant tests/test_inspector_await_promise.cjs`: passed, including major
  and minor WeakMap ephemeron and WeakSet weak-key collection cases,
  nonregistered symbol keys, same-job WeakRef retention, nested WeakMaps, and a
  reversed 512-link ephemeron chain. Stress runs remained linear through
  400,000 reversed links.
- Serial AB/BA allocation runs against an optimized clean-HEAD build showed no
  meaningful ordinary-allocation regression (medians overlapped at roughly
  77-78 ms). A 100,000-entry live WeakMap construction stress case was roughly
  7% slower than the old strong-marking implementation; this is isolated to
  weak-edge mutation and avoids the earlier repeated full-table minor scans.
- A later 12-run serial AB/BA comparison against optimized `3d0df5b4` measured
  300,000 WeakMap inserts at 11.06 ms versus 22.31 ms and lookups at 5.35 ms
  versus 15.73 ms. Dead-key GC churn improved from 241.49 ms to 228.24 ms for
  unique keys and from 154.78 ms to 123.94 ms when keys were shared across
  maps. Equivalent allocation/churn without WeakMap operations took 189.96 ms
  and 107.27 ms, so proposed 150 ms and 90 ms GC targets are below the current
  non-WeakMap workload floor. The 300,000-entry operation run also reduced
  maximum RSS from 107.5 MB to 96.0 MB.
- `./build/ant tests/test_collections_constructor_iterables.cjs`: passed.
- `./build/ant tests/test_weakmap.js`: passed.
- `./build/ant examples/spec/run.js --all`: 3,920 passed, 0 failed across
  100 spec files.
- `maid preflight` and `maid knowledge`: passed.
- After the shared process-stage extraction, `meson setup --reconfigure build`,
  `meson compile -C build`, every `tests/test_child_process_*.cjs` test, and
  `tests/test_shell.js` passed. A fresh 552-case differential matched clean Ant
  `3d0df5b4` in all 548 non-NUL cases and Node in 547; the sole Node difference
  is the pre-existing synchronous ENOENT result shape. All four NUL cases
  reject like Node and intentionally differ from the older truncating behavior.
- After typed-plan caching and lowering specialization, `meson compile -C
  build`, `tests/test_shell.js`, `tests/test_debug_shell_compile.cjs`, every
  `tests/test_child_process_*.cjs` test, `maid preflight`, `maid knowledge`, and
  the 3,920-test spec suite passed. The representative single-pipeline dump
  shrank from 445 generated JavaScript bytes to 44. In eight interleaved pairs
  against the saved pre-change binary, medians improved from 240.07 ms to
  228.75 ms for 20,000 cached single-clause invocations and from 137.44 ms to
  132.33 ms for 10,000 cached two-clause invocations.
- After adding native pipeline stages, focused shell, child-process, collection,
  WeakMap, WeakSet, and inspector GC tests passed, as did `maid preflight`,
  `maid knowledge`, and the 3,922-test spec suite. Native `echo` pipelines
  passed 100 repetitions each with 128 KiB consumed output and an ignored
  downstream pipe. Six interleaved weak-collection comparisons found no
  regression: set/get medians were 11.64/6.02 ms before and 11.57/5.65 ms
  after; unique/shared weak-GC medians were 233.31/129.13 ms before and
  227.24/128.76 ms after.

## Remaining risks

- This is the first grammar slice, not POSIX compatibility. The expansion,
  compound-command, variable, special-built-in, and signal work listed above
  remains required.
- This milestone was built and exercised on macOS. The Windows and Linux paths
  still need platform CI coverage.
