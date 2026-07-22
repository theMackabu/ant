# LLVM 21 Codegen Investigation

Status: active
Last reviewed: 2026-07-21
Owner: theMackabu

## Goal

Determine whether LLVM/Clang 21 materially regresses Ant's native runtime
codegen relative to LLVM/Clang 20, and whether recent source-level
devirtualization, inlining, or string work is compensating for compiler
behavior rather than improving a stable baseline.

The outcome must distinguish compiler effects from stale PGO data, source
changes, benchmark noise, and platform differences. If LLVM 21 is measurably
worse, select and validate a release-toolchain response instead of carrying
unnecessary source complexity solely to recover lost compiler performance.

## Current Findings

- The release configuration pins LLVM 21 in `.github/versions.json`.
- The current local `cc` is Clang 21.1.8 targeting `arm64-apple-darwin`.
- The configured native build uses `-O3`, LTO, and a Clang profile through
  `-fprofile-use` for Ant runtime sources.
- Ant does not pass `-fno-vectorize` or `-fno-slp-vectorize` in tracked build
  configuration.
- A local Clang 21 smoke test reports successful loop vectorization, so there
  is no evidence that Ant's direct C/C++ build has loop auto-vectorization
  disabled globally.
- Commit `b4b27e1e` (PR #60) explicitly optimized string accumulation and
  devirtualized hot Silver runtime paths. Its proximity makes source revision
  a required axis in the comparison, not evidence of an LLVM regression.
- Prior benchmark work showed that stale or mismatched PGO can create broad,
  misleading slowdowns. No LLVM conclusion is valid with a reused profile.

## Scope

- LLVM/Clang 20 and 21 native builds on the same source revisions
- Non-PGO and independently trained PGO configurations
- The checked-in `tests/bench.js` suite and focused native-runtime workloads
  affected by PR #60
- Compiler optimization remarks and disassembly for hot native loops and call
  boundaries
- macOS ARM64 first, followed by Linux where the initial result is material
- Release workflow and toolchain pinning only if the measurements justify it

Do not mix this investigation with Silver JIT backend selection. LLVM builds
Ant's native runtime; Silver's generated-code behavior needs separate evidence
before it is attributed to the host compiler.

## Constraints

- Use the exact same source tree, Meson options, CPU target, linker mode, and
  benchmark inputs for each compiler comparison.
- Use separate build directories and compiler caches for every matrix cell.
- Generate PGO data independently with the matching compiler and
  `llvm-profdata`; never reuse an LLVM 20 profile with LLVM 21 or vice versa.
- Record both compiler and linker provenance, including full version strings
  and resolved executable paths.
- Run non-PGO comparisons before PGO comparisons so profile effects remain
  visible.
- Keep warmup count, sample count, environment variables, power mode, and
  background load consistent. Record medians and dispersion rather than a
  single best run.
- Do not make performance assertions from the existing `build/` tree; its
  compiler, source revision, cache, and profile may not match the checkout.
- Do not add `always_inline`, explicit SIMD, or compiler-version conditionals
  until optimization output and end-to-end measurements identify a concrete
  missed optimization.
- Preserve the unrelated local change in `packages/nix/vendor.nix`.

## Task List

1. Freeze the comparison protocol.
   - Record the current commit, CPU model, OS version, power state, and resolved
     paths and versions for `clang`, `clang++`, `ld`, and `llvm-profdata`.
   - Choose one command for the full benchmark suite and focused commands for
     the PR #60 string accumulation and Silver call/devirtualization paths.
   - Record benchmark iteration and warmup behavior before collecting results.
   - Define the reporting threshold before seeing results: treat changes below
     normal run-to-run variance as parity.

2. Make isolated LLVM 20 and LLVM 21 build trees.
   - Configure both from the same clean source commit with identical release
     options, `-O3`, LTO mode, deployment target, and CPU target.
   - Disable compiler caching or give each compiler/version a separate cache.
   - Save Meson configuration and representative compile/link commands for
     each tree.
   - Verify the produced binaries report the expected Ant revision and inspect
     their linked-library provenance.

3. Establish the non-PGO compiler baseline.
   - Build both toolchains without profile generation or profile use.
   - Run correctness smoke tests before timing either binary.
   - Alternate LLVM 20 and LLVM 21 benchmark runs to reduce thermal and
     time-order bias.
   - Record per-row median, spread, and LLVM 21 relative to LLVM 20 in the
     non-PGO results table.
   - Re-run any apparent regression in a fresh process before classifying it.

4. Inspect codegen only where the non-PGO results point.
   - Capture Clang optimization remarks for loop vectorization, SLP
     vectorization, inlining, and missed transformations in implicated files.
   - Diff generated assembly or disassembly for the specific hot functions;
     do not use whole-binary diffs as primary evidence.
   - Compare instruction count, branches, calls, vector width, and code size
     around the hot region.
   - Use sampling profiles to confirm that a codegen difference contributes to
     end-to-end time rather than merely looking different.

5. Build a fresh PGO pair.
   - Generate LLVM 20 raw profiles with the LLVM 20 instrumented binary and
     merge them with LLVM 20 `llvm-profdata`.
   - Repeat independently for LLVM 21.
   - Save profile provenance: source commit, compiler version, training
     command, profile timestamp, and profile hash.
   - Build the two release binaries with their matching profiles and verify
     compile commands reference the intended profile paths.
   - Repeat the alternating benchmark protocol and fill the PGO results table.

6. Separate compiler effects from PR #60 source effects.
   - If the current revision shows a material LLVM 20/21 delta, repeat the
     focused benchmarks at `b4b27e1e^` and `b4b27e1e` under both compilers.
   - Use a two-by-two table so the compiler delta, source delta, and their
     interaction remain distinct.
   - Check whether PR #60 improves both compiler versions, disproportionately
     recovers LLVM 21, or changes unrelated JIT/runtime behavior.
   - Do not revert useful source work merely because it compensates for LLVM;
     evaluate maintainability and wins on supported toolchains separately.

7. Confirm any material result on Linux.
   - Reproduce the smallest decisive matrix on Linux using the same source,
     benchmark protocol, and per-compiler PGO discipline.
   - Keep architecture results separate; do not average macOS ARM64 and Linux
     x64 or ARM64 into one headline number.
   - Inspect CI/release artifacts if local and release behavior disagree.

8. Choose the toolchain response.
   - Keep LLVM 21 if differences are parity or workload-specific without a
     meaningful release impact.
   - If LLVM 21 regresses important workloads, compare the current supported
     alternative before pinning or upgrading the release matrix.
   - Prefer a toolchain change for broad compiler regressions and a narrow
     source change for a demonstrated missed optimization with cross-toolchain
     benefit.
   - Document the chosen compiler version and the benchmark evidence in this
     plan, `BUILDING.md`, and release workflow metadata as applicable.

9. Validate any resulting build or source change.
   - Run `maid preflight` and the focused commands it recommends.
   - Re-run the affected Meson setup/build flow from a fresh build directory.
   - Run focused runtime and JIT regressions for any changed hot path.
   - Run `./build/ant examples/spec/run.js --all` for runtime-semantic changes,
     or record why a narrower validation scope is sufficient.
   - Run `maid knowledge` and `maid structure` for toolchain or documentation
     changes.

## Results Tables

Populate these tables with medians and a dispersion measure from the frozen
protocol. Keep raw run logs alongside the investigation notes or CI artifacts.

### Current Revision

| Configuration | Compiler | Total | Important regressions | Notes |
| ------------- | -------- | ----: | --------------------- | ----- |
| non-PGO       | LLVM 20  |     - | -                     |       |
| non-PGO       | LLVM 21  |     - | -                     |       |
| fresh PGO     | LLVM 20  |     - | -                     |       |
| fresh PGO     | LLVM 21  |     - | -                     |       |

### PR #60 Interaction

| Source revision | Compiler | Configuration | Focused result | Notes |
| --------------- | -------- | ------------- | -------------: | ----- |
| `b4b27e1e^`     | LLVM 20  | fresh PGO     |              - |       |
| `b4b27e1e^`     | LLVM 21  | fresh PGO     |              - |       |
| `b4b27e1e`      | LLVM 20  | fresh PGO     |              - |       |
| `b4b27e1e`      | LLVM 21  | fresh PGO     |              - |       |

## Decision Log

- 2026-07-21: Opened the investigation because LLVM 21 is the pinned Ant
  release compiler and external LLVM 21 codegen/inlining reports overlap with
  recent Ant optimization work.
- 2026-07-21: Do not assume Ant shares a reported globally disabled loop
  vectorizer. Ant's direct Clang 21 C compilation vectorizes a smoke-test loop,
  and no tracked Ant flag disables the loop or SLP vectorizers.
- 2026-07-21: Require both non-PGO and independently trained PGO comparisons.
  Existing or cross-version profiles are not admissible evidence.
- 2026-07-21: Use a two-by-two source/compiler comparison around PR #60 only
  after the same-revision compiler comparison shows a material delta.

## Validation Status

- Confirmed `.github/versions.json` pins LLVM 21.
- Confirmed the current local compiler is Clang 21.1.8 targeting macOS ARM64.
- Confirmed representative Ant runtime compile commands use `-O3`, LTO, and a
  PGO profile.
- Confirmed no tracked Ant configuration disables loop or SLP vectorization.
- Confirmed Clang 21 vectorizes a standalone representative loop with
  `-Rpass=loop-vectorize`.
- LLVM 20/21 benchmark matrix: not started.
- Fresh per-compiler PGO comparison: not started.
- Linux confirmation: not started.

## Stop Conditions

- Stop compiler attribution if repeated isolated runs show LLVM 20 and 21 are
  within the protocol's measured noise on important workloads.
- Stop investigating a benchmark row when its assembly difference does not
  survive end-to-end reruns or appear in a sampling profile.
- Do not expand to every supported platform until one platform shows a
  repeatable, material compiler delta.
- Do not change the release pin from a synthetic-only win that regresses
  correctness, artifact portability, size, or important real workloads.
- Rebuild the affected matrix cell from scratch whenever compiler provenance,
  PGO provenance, or cache isolation is uncertain.

## Follow-Ups

- Decide where benchmark raw data and compiler optimization records should be
  retained if they are too large for this plan.
- Add a small reproducible codegen fixture only if it isolates a confirmed Ant
  hot-path regression; avoid tests for speculative compiler behavior.
- If LLVM 21 is retained, record the comparison date and results so the same
  suspicion is not repeatedly reopened without new evidence.
