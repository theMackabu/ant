# Anti-Slop Linting for C and Bundled JavaScript

Status: completed
Completed: 2026-08-17

## Problem

The anti-slop Oxlint plugin protects TypeScript type evidence, but Ant's main
implementation is C and its bundled Node-compatible modules are currently
plain JavaScript. Applying the rules literally would reject valid runtime
dispatch, VM terminology, library callbacks, and offset-based representations.

## Decision

- Keep the Oxlint toolchain isolated under `tools/oxlint/` so runtime bundle
  generation does not install lint-only dependencies.
- Register every anti-slop rule at error severity. Scope an override to
  `src/builtins/` that permits runtime `typeof` dispatch and the term `shape`;
  the other rules remain active, including TypeScript rules for future `.mts`
  builtins.
- Use Clang-Tidy 21 or newer for Ant-owned C. Enable a conservative set of
  analyzer, widening, allocation, `sizeof`, memory, return-value, and
  redundant-cast checks. Do not blanket-ban `void *`, C casts,
  integer-to-pointer conversions, or engine shape names.
- Make the default C command enforce changed lines only. Require a report-only
  full run for header changes because a header can affect many translation
  units.
- Route future C, header, bundled JavaScript, and lint-policy changes to the
  matching Maid task.

## Validation

- Confirmed the copied plugin is byte-for-byte identical to the skill asset.
- Installed and resolved `oxlint` 1.78.0 and `@oxlint/plugins` 1.78.0 from the
  committed lockfile.
- Ran `maid lint_builtins` successfully.
- Confirmed an intentional `Reflect.get` probe fails with
  `anti-slop/no-reflect-get`, then removed the probe.
- Ran `maid lint_c`; it passed with no changed C translation units.
- Confirmed `maid lint_c_all` reports the missing Clang-Tidy 21 tool clearly
  when it is not on `PATH` or supplied through `ANT_CLANG_TIDY`.
- Located the matching Nix Clang-Tidy through `ANT_CLANG_TIDY` and used its
  first full baseline to remove the noisy multi-level pointer-conversion check;
  ordinary C allocation and deallocation made that check unsuitable for Ant.
- Added automatic macOS SDK discovery through `xcrun` after the standalone Nix
  Clang-Tidy correctly exposed that it does not inherit the compiler wrapper's
  SDK search path.
- Completed a 197-translation-unit baseline. Existing diagnostics remain
  report-only; an explicit source run proved that the changed-source mode turns
  the same diagnostics into errors.
- Temporarily changed an existing ignored-return line, confirmed the default
  line filter rejected that exact changed line, and restored the source without
  leaving a runtime diff.
- Exercised the validation router with representative C source, header,
  builtin, and lint-policy paths.

## Follow-ups

- Continue reviewing the report-only `maid lint_c_all` baseline and tune only
  demonstrated false positives before making the full run enforcing.
- Convert bundled modules from `.mjs` to `.mts` incrementally so the contract
  and type-evidence rules can protect their public boundaries.
- Add CI enforcement after the full C baseline is understood and stable.
