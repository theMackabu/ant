# Testing Guide

Status: active
Last reviewed: 2026-04-09
Owner: theMackabu

This guide keeps validation proportional to the change while still protecting runtime behavior.

## Common Commands

- Build the configured tree: `maid build`
- Fresh setup and build: `maid setup && maid build`
- Run one runtime test: `./build/ant tests/test_<name>.cjs`
- Run the spec suite: `./build/ant examples/spec/run.js --all`
- Validate repo knowledge docs: `maid knowledge`
- Validate changed-file boundaries: `maid structure`
- Ask the harness what to run for the current diff: `maid validate_changes`

## Validation By Change Type

### Runtime behavior in `src/modules/`, `src/esm/`, or `src/builtins/`

- Run the most specific `tests/test_<name>.cjs` coverage you can find or add.
- Run `./build/ant examples/spec/run.js <spec_name>` when the change affects shared runtime
  semantics or built-ins used broadly across the platform.

### Engine behavior in `src/silver/`, `src/gc/`, or runtime core files

- Rebuild with `maid build`.
- Run focused regression tests first.
- Run `./build/ant examples/spec/run.js --all` before landing behavior changes.

### Build or toolchain changes

- Re-run the affected Meson flow (`maid setup`, `maid reconfigure`, or
  `maid build`).
- Validate any new repo-knowledge or workflow checks locally with
  `maid knowledge` and `maid structure`.

## Notes

- Keep new tests close to the behavior they protect so future agent runs can
  discover the expected pattern quickly.
- If the right validation is expensive or unavailable, document the gap in the
  associated [execution plan](../exec-plans/index.md).
