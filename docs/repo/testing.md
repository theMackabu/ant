# Testing Guide

Status: active
Last reviewed: 2026-09-10
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

### Engine behavior in `src/silver/`, `src/jit/`, `src/gc/`, or runtime core files

- Rebuild with `maid build`.
- Run focused regression tests first.
- Run `./build/ant examples/spec/run.js --all` before landing behavior changes.

### Build or toolchain changes

- Re-run the affected Meson flow (`maid setup`, `maid reconfigure`, or
  `maid build`).
- Validate any new repo-knowledge or workflow checks locally with
  `maid knowledge` and `maid structure`.

### Documentation only

- Run `maid preflight` and any documentation checks it recommends.
- Check links in edited plans as well; `maid knowledge` checks the core
  entrypoints, not every archived document.
- Runtime builds and spec runs are unnecessary when only documentation changes.

## Performance Comparisons

- Pin baseline and candidate binaries and alternate their run order on the same
  host. Check identical work and output before comparing timings.
- Verify child processes use the intended binary, including benchmark runners
  and commands that resolve executables through `PATH`.
- Match build configuration and PGO conditions. Stale or discarded profile
  counts can change unrelated hot code; separate that effect from the source
  change before claiming a regression or improvement.
- Use whole-run counters to test a suspected mechanism. A short profile sample
  or a synthetic microbenchmark does not establish a whole-program speedup.
- Record the tested revision, configuration, workload, result, and limits in
  the relevant plan. Old suite counts, host timings, and temporary binary paths
  are historical evidence, not current acceptance thresholds.

## Notes

- Keep new tests close to the behavior they protect so future agent runs can
  discover the expected pattern quickly.
- In sandboxed agent sessions, builds and broad validation commands such as
  `maid build`, or `./build/ant examples/spec/run.js --all` may need broader system access.
  Pause and get explicit user approval before retrying them with sandbox
  escalation.
- In sandboxed agent sessions, `./build/ant examples/spec/run.js --all` can fail
  in the `fetch` spec because outbound network access is blocked. Treat that as
  an expected sandbox limitation, and prefer targeted spec files when the
  change does not need networked coverage.
- If the right validation is expensive or unavailable, document the gap in the
  associated [execution plan](../exec-plans/index.md).
