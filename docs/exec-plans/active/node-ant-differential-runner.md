# Node–Ant Differential Runner

Status: active
Last reviewed: 2026-08-29
Owner: theMackabu

## Goal

Provide a deterministic developer tool that generates focused JavaScript
probes, compares their complete process behavior under Node and Ant, and
reduces mismatches to standalone reproducers.

## Initial scope

- Property access, ownership, descriptors, enumeration, and serialization.
- RegExp construction, execution, replacement, and state.
- Promise and microtask ordering around native-to-JavaScript calls.
- Node stream public property values and descriptor shape.

The runner is for discovery and triage. Its findings do not automatically
become conformance requirements because Node-version differences and deliberate
surface omissions must still be classified.

## Design decisions

- Generate probes from a seed so every finding is reproducible.
- Compare status, signal, timeout, stdout, and stderr rather than stdout alone.
- Canonically encode values that ordinary JSON cannot preserve.
- Minimize structured scenario arrays instead of rewriting arbitrary JavaScript
  text, keeping reduced repros syntactically valid and understandable.
- Keep stream probes atomic so the known own-key divergence does not force
  every randomly generated property case to fail.
- Keep orchestration Node-hosted so a broken Ant behavior cannot compromise the
  comparison driver.

## Tasks

1. [x] Add deterministic generation and process comparison.
2. [x] Add property, RegExp, Promise-timing, and stream-shape families.
3. [x] Add structured mismatch minimization and standalone repro output.
4. [x] Add Node-side unit tests and usage documentation.
5. [x] Run repository preflight and its recommended validation.
6. [ ] Classify the initial mismatch corpus and record useful follow-up work.

## Validation status

- `node tests/differential/test.mjs`: passed, including deterministic
  generation, CLI parsing, minimization, child-process capture, and a complete
  Node-versus-Node comparison through the runner.
- Repository structure and knowledge checks: passed by invoking the underlying
  `.github/agents/` checks directly; `maid` is not installed in the sandbox.
- `git diff --check`: passed.
- An Ant-versus-Node corpus run was not possible because this fresh sandbox has
  no configured `build/ant`. The runner's missing-executable diagnostic was
  exercised instead. The validation router's generic `tests/` recommendations
  are not directly applicable to these Node-hosted orchestration modules.
