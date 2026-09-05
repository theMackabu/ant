# Cwd-aware path resolution

Status: active
Last reviewed: 2026-09-04
Owner: theMackabu

## Scope

The user subsequently requested replacing the C module with Node v26.5.1's
`path.js`. The C resolver design and results below are historical.

## Current JS implementation

- Removed `src/modules/path.c` and its native registrations. The bundled JS
  module preserves the bare, `node:`, and `ant:` path aliases and variants.
- Native initialization captures a fixed list of original values in GC-rooted
  slots. Path imports the lazy, frozen `ant:internal/primordials` module without
  loader injection. See [Private Primordial Capture](private-primordials.md).
- Small internal helpers provide validators, constants, and lazy loading.
- `process.cwd()` uses libuv and throws with `code`, `errno`, and `syscall`
  metadata on failure. Both direct cwd failure and path propagation tests pass.
- `matchesGlob()` uses minimatch 10.2.5 as a pinned **build-time** dependency in
  `src/tools/package.json`; `npm-shrinkwrap.json` locks transitive dependencies.
  Esbuild resolves tools dependencies and embeds them in the glob builtin,
  including license text collected from the packages actually bundled.
  No copied minimatch source, dependency license files, or runtime npm lookup
  remain in `src/builtins/ant/internal/deps`.
- Module branding follows Node: the namespace is `[object Module]`, while the
  default path export is `[object Object]`.

The npm-backed bundle rebuilt successfully. Ten seeded differential probes
(4,560 observations, including glob syntax and Windows-style glob paths) passed
against Node. Primordial/alias and cwd-error tests, differential runner unit
tests, and preflight passed. Generated bundle inspection confirmed notices for
minimatch, brace-expansion, and balanced-match are embedded.

## Earlier C implementation

Share cwd-aware resolution between `resolve()` and `relative()`, preserving
separate Win32 device and rootedness tracking. Treat the archived PR #44
implementation as design evidence only.

## Implementation decisions

- Capture cwd lazily once per public call; both `relative()` inputs share it.
- Collect borrowed input slices right-to-left, then assemble one owned buffer.
- Normalize separators during assembly and compact dot components in place.
  Compaction moves bytes left and scans removed components once.
- Pass the assembled root boundary to normalization so malformed UNC tails
  cannot be reinterpreted as a new root. Canonicalize UNC device separators.
- Compare resolved paths directly. POSIX uses component boundaries; Win32
  tracks Node's last common separator and device-less prefix rules. No
  component arrays or depth thresholds are needed.
- Preserve the existing shared normalizer's trailing-separator behavior;
  resolution trims trailing separators beyond the root.

## Validation

The user approved builds and tests on 2026-09-04. The macOS ARM64 verification
build succeeded with Temporal, PGO, and LTO disabled. Preflight, differential
runner unit checks, the three focused path test files, and all 24 path spec
assertions passed.

The Node differential probe exposed a UNC cross-share relative-path mismatch.
Relative comparison now treats UNC shares as components and preserves the full
resolved target when servers differ. All 38 explicit path scenarios pass,
including five additional UNC-root regressions.

A broader deterministic sweep (seed 44, 1,000 input pairs per style, resolve and
relative: 4,000 comparisons) initially found 52 Win32 mismatches. After fixing
UNC parsing and Win32 relative separator-boundary handling, the same sweep
passed with zero mismatches.

The path differential family now includes 48 fixed scenarios and 400 seeded
resolve/relative comparisons per probe. It covers relative, rooted,
drive-absolute, drive-relative, UNC, mixed separators, dot/parent components,
and device-less prefix edge cases. Verification against Node v26.5.1 passed:

- `meson compile -C build -j8`
- `node tests/differential/runner.mjs --family path --cases 100 --seed 44`
  (44,800 comparisons, zero mismatches; fixed scenarios repeat per probe)
- `node tests/differential/test.mjs`
- `./build/ant tests/test_path.js`
- `./build/ant tests/test_path_parse_format.cjs`
- `./build/ant tests/test_windows_file_url_paths.cjs`
- `./build/ant examples/spec/run.js path` (24 assertions)
- `git diff --check`

Native Windows cwd behavior and performance remain unverified. Any performance
claim requires measurements against a pinned base.
