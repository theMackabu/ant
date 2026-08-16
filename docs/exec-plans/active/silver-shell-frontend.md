# Silver Shell Frontend

Status: active
Last reviewed: 2026-08-15
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
- captured stdout/stderr and exit status;
- `.text()`, `.lines()`, and `.nothrow()` result controls;
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

## Validation status

- `meson compile -C build`: passed.
- `./build/ant tests/test_shell.js`: passed, including structured interpolation,
  control operators, stateful `cd`, ordered redirection, early pipeline exit,
  missing executable handling, weak-cache reuse, and throw/nothrow policy.
- Focused child-process tests selected by `test_child_process*`: passed.
- `./build/ant tests/test_collections_constructor_iterables.cjs`: passed.
- `./build/ant tests/test_weakmap.js`: passed.
- `./build/ant examples/spec/run.js --all`: 3,920 passed, 0 failed across
  100 spec files.
- `maid preflight` and `maid knowledge`: passed.

## Remaining risks

- This is the first grammar slice, not POSIX compatibility. The expansion,
  compound-command, variable, special-built-in, and signal work listed above
  remains required.
- Redirection data is currently captured and applied through libuv file I/O.
  Descriptor destinations respect source order, but exact stdout/stderr byte
  interleaving requires native descriptor wiring.
- Multi-stage pipelines currently use direct executable spawning for commands
  that have a native single-command built-in. Native pipeline stages and their
  isolated context semantics remain part of the pipeline-subshell work.
- Pipeline forwarding queues libuv writes and propagates early close/SIGPIPE,
  but it does not yet apply a bounded high-water mark for backpressure.
- This milestone was built and exercised on macOS. The Windows and Linux paths
  still need platform CI coverage.
