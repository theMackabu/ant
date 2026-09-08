# Swarm JIT Modularization

Status: completed
Last reviewed: 2026-09-07
Owner: theMackabu

## Outcome

Replace the 14,406-line `src/silver/swarm.c`, including its 8,603-line
compiler function, with responsibility-based translation units in
`src/silver/swarm/`. The public `include/silver/swarm.h` API is unchanged.

## Boundaries

- `runtime.c`: MIR lifetime, compile-and-call, and OSR runtime entry.
- `compile.c`: compilation policy, opcode dispatch, post-op bookkeeping,
  finalization, and cleanup.
- `setup_prototypes.c` and `setup_frame.c`: helper imports/prototypes and
  generated frame setup, including OSR restoration.
- `emit_*.c`: opcode families (literals, stack, locals, arithmetic,
  comparisons, bitwise/type tests, control flow, calls, methods, intrinsics,
  strings, properties, bindings, and iteration).
- `analysis.c`, `values.c`, `guards.c`, `strings.c`, `properties.c`,
  `closures.c`, and `inline.c`: existing analysis and MIR-emission helpers.
- `compile.h`: private per-compilation state and opcode-emitter declarations.
- `internal.h`: shared private JIT types and helper declarations.

The engine, library, and runtime source groups in `sources.json` explicitly
include `src/silver/swarm/*.c`. Files are compiled separately, not included as
source fragments. No generated build files are versioned.

## Invariants

The extraction preserves helper bodies and opcode behavior. A single
stack-owned `jit_compile_t` contains shared state; emitters access it directly
instead of copying state in and out or relying on macro aliases.

Emitters can update the bytecode cursor and instruction size for fusion.
Dispatch, branch-join normalization, and post-op MIR invalidation stay in the
coordinator. This preserves integer-local tracking and cached-element
invalidation across opcode families. The coordinator rejects unsupported
opcodes before calling a family emitter.

Return and error emission use explicit context-aware functions in
`emit_common.c`, preserving catch routing and upvalue cleanup. The bailout
descriptor references the context's actual virtual stack. Allocation-failure
cleanup and MIR instruction ordering remain unchanged.

## Validation

- Compared all 112 extracted helper bodies against the original source using
  C tokens: identical apart from whitespace/comments and linkage declarations.
- Compared all 161 opcode cases against the original source, normalizing only
  context member access and the two extracted error/return helpers: identical.
- Reconfigured and rebuilt the native Meson target after source extraction.
- Focused `test_jit_*` CJS/JS tests, the two inline-special-object entrypoints,
  and `test_direct_eval_jit.cjs`: 53 passed, 0 failed.
- `./build/ant examples/spec/run.js --all`: 4,221 tests passed across 102 files,
  0 failures.
- `maid preflight`: repository knowledge and structure checks passed.

This is a source-organization refactor, not an optimization. No new language
behavior or performance claim is introduced. Cross-platform builds and
performance benchmarks were not run in this Linux orb.

## Master merge (2026-09-07)

Kept the modular source layout and transferred master's changes from the
deleted monolith to the corresponding modules: call/feedback headers,
invocation-owned constructor targets, feature scanning for constructor and
super context, and the expanded interpreter-resume ABI. Removed the obsolete
ambient constructor-target write.

Validation: repository preflight and conflict-resolution whitespace checks passed.
The full staged diff reports existing trailing whitespace in incoming master files.
Native build regeneration initially could not find libm; supplying the Xcode SDKROOT
resolved that check, but regeneration then stopped because llvm-nm is missing.
Focused constructor/JIT regressions and the spec suite were not run against a
rebuilt binary. Broader WASM/package checks recommended for incoming master
changes were not run; conflict resolution only changes the native Swarm modules.
