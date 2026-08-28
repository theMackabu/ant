# Compiled Native Addon Assets

Status: completed
Last reviewed: 2026-08-28
Owner: theMackabu

## Goal

Make `ant compile` preserve native addon behavior in standalone executables.
Compiled programs must either embed the native package files needed by
`dlopen` and helper processes or report an actionable compile-time diagnostic.

## Scope

- Resolve statically knowable computed module specifiers, including template
  literals built from `process.platform` and `process.arch`.
- Detect dynamic relative native-addon loads inside npm packages.
- Embed native `.node` files and executable or shared-library companions.
- Materialize native package files into a private real directory before module
  evaluation while keeping bundle keys as module identities.
- Preserve real `__filename` and `__dirname` values inside materialized native
  packages so package code can locate helper executables.
- Warn for remaining non-constant imports and requires, including those inside
  `try` blocks.

## Constraints

- Keep ordinary compiled modules in the in-memory VFS.
- Preserve the existing recorded-edge resolver as the primary bundle lookup.
- Do not fall back to the source `node_modules` tree at runtime.
- Preserve package-relative paths and executable permissions during
  materialization.
- Reject unsafe materialization keys rather than writing outside the private
  extraction root.

## Task List

- [x] Extend trace results and bundle records with materialization metadata.
- [x] Add compile-time evaluation for simple constant module specifiers.
- [x] Discover native package payloads and executable companions.
- [x] Materialize payloads before bundle activation and remove them at normal
      shutdown.
- [x] Map materialized parent paths back to bundle keys during resolution.
- [x] Add focused compile tests for computed package names, native loading,
      helper execution, permissions, and diagnostics.
- [x] Validate the Yet `@lydell/node-pty` standalone executable end to end.

## Decision Log

- Materialize only packages that contain a dynamically loaded native addon.
  Ordinary application modules retain virtual `/$ant/...` filenames.
- Keep native files in module records and mark non-module companions as assets.
  This preserves one path table and one payload store in the bundle format.
- Use a private per-process temporary directory. This avoids stale native code
  caches and cross-process replacement races.
- Permit exact dynamic relative bundle lookup after recorded-edge lookup. This
  supports runtime-computed `.node` paths without reimplementing Node package
  resolution in the runtime.
- Defer unresolved `try/catch` diagnostics within each module. Suppress them
  when that module successfully contributes a native addon, so generated
  NAPI-RS fallback loaders do not imply that their bundled native path is
  missing. Dynamic imports in modules without a native fallback still warn.
- Prefer a Darwin target-architecture native package over its optional
  `-darwin-universal` fallback when both resolve directly to `.node` files.
  The compiled loader falls through its existing `try/catch` to the smaller,
  exact-architecture payload instead of embedding both binaries.
- Advance the compiled bundle format to version 2. Versioned readers reject
  older records instead of interpreting the new flags field ambiguously.

## Validation Status

- `meson compile -C build` and `meson compile -C build ant-runtime` pass.
- `./build/ant tests/test_compile_basic.cjs` passes.
- `./build/ant tests/test_compile_native_addon.cjs` passes, including computed
  package resolution, native loading, helper execution, physical resolved
  paths, cleanup, and the non-constant require warning inside `try`.
- `maid preflight`, `maid knowledge`, `maid structure`, and
  `maid validate_changes` pass.
- `./build/ant examples/spec/run.js --all` passes: 3972 tests in 101 files,
  with zero failures.
- Ant compiled `/Users/themackabu/Developer/projects/yet/dist/yet.js`; the
  resulting executable ran `--help` from `/tmp` without the source package
  tree.

## Follow-ups

- The native-addon fixture currently builds on Darwin and Linux. Add a Windows
  fixture build before claiming the focused test is cross-platform.
