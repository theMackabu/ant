# Architecture

Status: active
Last reviewed: 2026-04-09
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
- `src/gc/` contains memory management primitives and object/string handling.
- Files like `src/errors.c`, `src/descriptors.c`, and `src/shapes.c` support
  core engine behavior shared across subsystems.

### Host platform surface

- `src/modules/` implements built-in modules and runtime-facing JS APIs.
- `src/builtins/` holds bundled JavaScript shims and Node-compatible modules.
- `src/http/`, `src/net/`, and `src/streams/` provide protocol, networking, and
  streaming support.
- `src/esm/` handles module loading, export wiring, and built-in bundle access.

### Tooling and generated inputs

- `src/tools/` generates bundled sources such as the builtin bundle and JS
  snapshot.
- `src/core/` stores TypeScript sources and runtime metadata that feed
  generation steps.
- `src/pkg/` is the Zig package manager.
- `src/strip/` is the Rust type-stripper used during builds.
- `meson/` and the root [meson.build](meson.build) describe the build graph,
  dependency setup, and custom code generation targets.

## Tests and Validation

- `tests/` contains focused runtime tests.
- `examples/spec/` is the main spec regression suite.
- `test262/`, and `tools/wpt/` support broader conformance and standards work.
- See [docs/repo/testing.md](docs/repo/testing.md) for the recommended command
  set by change type.

## Change Placement Guidelines

- Parser, bytecode, execution semantics, or JIT-adjacent work belongs under
  `src/silver/`.
- Heap, string, or lifetime bugs usually belong under `src/gc/`.
- Built-in API behavior should land in `src/modules/`, `src/builtins/`, or
  `src/esm/` depending on whether the change is C runtime code, bundled JS, or
  module-loader plumbing.
- Networking and protocol work should stay in `src/http/`, `src/net/`, or
  `src/streams/` unless it is only wiring.
- Build graph changes should prefer `meson/` or `meson.build`; avoid burying
  build logic in ad-hoc shell scripts.

## Boundaries To Preserve

- Do not hand-edit third-party code in `vendor/` unless the task is explicitly a
  vendored dependency change.
- Do not check durable architecture knowledge into `todo/`; use
  [docs/exec-plans/index.md](docs/exec-plans/index.md) for multi-step work and
  `docs/repo/` for stable reference docs.
- Keep generated outputs reproducible. If a generated file changes, update or
  document the generator path in the same change.
