# Architecture

Status: active
Last reviewed: 2026-09-07
Owner: theMackabu

This document is the top-level map for Ant's runtime and build graph. It is
meant to answer "where should this change live?" before anyone starts editing.

## Design Priorities

- Keep the runtime small and fast to start.
- Prefer explicit, in-repo implementations over opaque build-time magic.
- Isolate third-party code under `vendor/` and keep Ant-owned logic in `src/`,
  `include/`, `meson/`, `tools/`, and `tests/`.

## Runtime Layers

### Process and startup

- `src/main.c` is the CLI executable entrypoint.
- `src/ant.c` and `src/runtime.c` handle runtime initialization and shared
  process setup.
- `src/cli/` contains command-line specific behavior such as version and package
  commands.

### JavaScript engine

- `src/silver/` contains the language pipeline: lexer, parser, compiler,
  directives, VM glue, and bytecode operations.
- `src/jit/` contains Silver's native JIT compiler, MIR emission, and JIT/OSR
  runtime entrypoints. Its Silver-facing interface is `include/silver/jit.h`.
- `src/gc/` contains memory management primitives and object/string handling.
- Files like `src/errors.c`, `src/descriptors.c`, and `src/shapes.c` support
  core engine behavior shared across subsystems.

Silver's headers separate data definitions from execution helpers:

- `include/silver/engine.h` defines VM, frame, closure, and function layouts,
  basic accessors, and runtime entry declarations.
- `include/silver/feedback.h` contains type feedback, specialization tracking,
  and JIT tiering helpers; it depends on the engine definitions.
- `src/silver/ops/async.h` implements async/TLA entry and await operations;
  it also depends on the engine definitions.
- `include/silver/call.h` owns call preparation, dispatch, and cleanup. It
  includes the feedback and async helpers before defining call dispatch.
- `src/silver/ops/calls.h` implements call bytecodes using those shared helpers.

Consumers include the header for the operations they use. Keep `engine.h`
independent of call, feedback, and opcode implementation headers so consumers
of VM definitions do not create an include cycle. See the completed
[header boundary plan](docs/exec-plans/completed/silver-header-boundaries.md).

Constructor context belongs to an invocation. JS frames and JIT invocation
arguments carry `new.target`; native callbacks receive it explicitly through
`ant_params_t`. Ordinary internal calls pass `js_mkundef()`, helpers delegating
the same construction preserve its target, and separate constructions use
their own target. Native constructors link a C-stack frame to root the target
across GC and re-entry. Direct eval receives the lexical target explicitly.
Reserve `ant_params_t` for callbacks using the `ant_cfunc_t` ABI. Internal
helpers use `ant_native_params_t` for the shared `js`, `args`, and `nargs`
parameters, followed by any helper-specific parameters. Include an explicit
constructor target only when the implementation needs it. `ant_params_t`
extends that shared prefix with `call_new_target` for the callback ABI.
There is no ambient `ant_t::new_target` field. The
[constructor-context plan](docs/exec-plans/completed/net-connection-websocket-arg-regression.md)
records the regression, implementation, and validation.

### Host platform surface

- `src/modules/` implements built-in modules and runtime-facing JS APIs.
- `src/builtins/` holds the JavaScript bootstrap, bundled shims, and
  Node-compatible modules.
- `src/http/`, `src/net/`, and `src/streams/` provide protocol, networking, and
  streaming support.
- `src/esm/` handles module loading, export wiring, and built-in bundle access.

### Tooling and generated inputs

- `src/tools/gen_builtins.js` generates both the JavaScript bootstrap snapshot
  and the independently loadable builtin-module bundle.
- `src/cli/messages.toml` stores the runtime and command-line message catalog.
- `src/pkg/` is the Zig package manager.
- TypeScript stripping is provided by the Skim Meson subproject.
- `meson/` and the root [meson.build](meson.build) describe the build graph,
  dependency setup, and custom code generation targets.

### Embedding packages

- `packages/wasm/` builds the Silver parser, bytecode interpreter, and GC as a
  `wasm32-wasip1` reactor. Its Ant-owned JavaScript loader supplies the small
  reviewed WASI/host import set; the CLI, JIT, native modules, and generated
  Emscripten runtime are outside that target.

## Tests and Validation

- `tests/` contains focused runtime tests.
- `examples/spec/` is the main spec regression suite.
- `test262/`, and `tools/wpt/` support broader conformance and standards work.
- See [docs/repo/testing.md](docs/repo/testing.md) for the recommended command
  set by change type.

## Change Placement Guidelines

- Parser, bytecode, and interpreter execution semantics belong under `src/silver/`.
- Native JIT analysis, lowering, and runtime integration belong under `src/jit/`.
  The JIT depends on Silver bytecode, frame, closure, and feedback definitions.
- Heap, string, or lifetime bugs usually belong under `src/gc/`.
- Built-in API behavior should land in `src/modules/`, `src/builtins/`, or
  `src/esm/` depending on whether the change is C runtime code, bundled JS, or
  module-loader plumbing.
- Networking and protocol work should stay in `src/http/`, `src/net/`, or
  `src/streams/` unless it is only wiring.
- Build graph changes should prefer `meson/` or `meson.build`; avoid burying
  build logic in ad-hoc shell scripts.
- Browser and Node embedding changes belong in `packages/wasm/`; keep the C
  ABI explicit and verify its complete import/export surface during the build.

## Boundaries To Preserve

- Do not hand-edit third-party code in `vendor/` unless the task is explicitly a
  vendored dependency change.
- Do not check durable architecture knowledge into `todo/`; use
  [docs/exec-plans/index.md](docs/exec-plans/index.md) for multi-step work and
  `docs/repo/` for stable reference docs.
- Keep generated outputs reproducible. If a generated file changes, update or
  document the generator path in the same change.
