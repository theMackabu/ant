# AGENTS.md

Status: active
Last reviewed: 2026-04-09
Owner: theMackabu

This file is the table of contents for agent work in Ant. Start here, then
open the smallest linked document that matches the task instead of loading the
entire repository into context.

## Start Here

- Repository map: [ARCHITECTURE.md](ARCHITECTURE.md)
- Knowledge index: [docs/repo/index.md](docs/repo/index.md)
- Build setup: [BUILDING.md](BUILDING.md)
- Contribution rules: [CONTRIBUTING.md](CONTRIBUTING.md)
- Durable execution plans: [docs/exec-plans/index.md](docs/exec-plans/index.md)

## Fast Path

- Start here for most changes: `maid preflight`
- Build from an existing configured tree: `meson compile -C build`
- Fresh local setup: `maid setup`
- Run a focused test file: `./build/ant tests/test_<name>.cjs`
- Run the spec suite: `./build/ant examples/spec/run.js --all`
- Run the individual harness steps when needed:
  `maid validate_changes`, `maid structure`, `maid knowledge`

## Codebase Map

- `src/main.c`, `src/ant.c`, and `src/runtime.c` wire process startup and the
  runtime entrypoints.
- `src/silver/` contains the parser, compiler, VM, and JIT-facing execution
  logic for the Ant Silver engine.
- `src/gc/` contains heap layout, roots, strings, ropes, and collection logic.
- `src/modules/` and `src/builtins/` implement built-in modules and host APIs.
- `src/http/`, `src/net/`, and `src/streams/` are the transport and I/O stack.
- `src/pkg/` is the Zig package manager; TypeScript stripping is provided by
  the Skim Meson subproject.
- `meson/` and `meson.build` define the build graph and generated headers.
- `.github/agents/` contains the lightweight repo-harness checks and validation
  router used by local tasks and CI.
- `tests/`, `examples/spec/`, and `tools/wpt/` cover targeted
  runtime tests, the spec suite, and conformance harnesses.

See [ARCHITECTURE.md](ARCHITECTURE.md) for subsystem boundaries and change
guidance.

## Change Rules

- Prefer changes in `src/`, `include/`, `meson/`, `tests/`, `tools/`, and `.github/agents/`.
- Treat `vendor/` and `build/` as generated or third-party surfaces. Only edit
  them when the task explicitly requires it.
- Keep durable design notes and execution history in versioned markdown under
  `docs/`. Treat `todo/` as scratch space, not the source of truth.
- Add or update tests when behavior changes.
- When touching build or runtime invariants, document the reasoning in
  [docs/exec-plans/index.md](docs/exec-plans/index.md) or a linked plan if the
  work spans multiple steps.
- Builds and broad validation runs (for example `maid build` or
  spec runs with `--all`) may need broader system access; pause and get
  explicit user approval before proceeding when sandbox escalation is required.
- Check [docs/repo/testing.md](docs/repo/testing.md) before choosing validation
  scope or retrying a failing broad test command.
- Before finalizing most code changes, run `maid preflight` and then execute any
  additional build or spec commands it recommends, or explain why they were not
  run.

## Which Doc To Open Next

- Build, toolchain, or platform issue:
  [BUILDING.md](BUILDING.md)
- Runtime or subsystem question:
  [ARCHITECTURE.md](ARCHITECTURE.md)
- Test selection or validation scope:
  [docs/repo/testing.md](docs/repo/testing.md)
- Long-running or multi-step task:
  [docs/exec-plans/index.md](docs/exec-plans/index.md)

## Keep This File Small

`AGENTS.md` should stay a concise entrypoint. Add durable detail to the linked
documents instead of expanding this file into a manual.
