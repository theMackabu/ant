# TypeScript Nix Generation

Status: completed
Last reviewed: 2026-09-05
Owner: theMackabu

## Outcome and decisions

Added a dependency-free TypeScript expression builder under `packages/ts-nix/`
and migrated the flake and toolchain definitions to `packages/nix/generate.ts`.
The package, shell, vendor, and version expressions remain imported Nix files.
This keeps the initial migration bounded while providing reusable generation
helpers. The builder does not translate arbitrary TypeScript semantics to Nix.

Generated files remain versioned to avoid requiring Ant before Nix can build Ant.
The generator has a read-only drift check. Literal strings and attribute names
are escaped; explicit expressions are separate from data. Helpers parenthesize
composed expressions to preserve Nix precedence. Trusted template syntax covers
constructs outside the initial helpers. Existing build settings are preserved.

## Validation

Focused tests run in Ant and use the actual Nix evaluator for literal round trips,
function calls within lists, bindings, conditionals, and attribute selections.
Tests also cover rejected inputs and string interpolation escaping. Both generated
files are parsed with Nix, and regeneration is checked for drift. Repository
preflight passed. The recommended existing-tree build was attempted but Meson
configuration failed in vendored libuv because the local toolchain could not
find library `m`. Focused tests passed using the existing Ant binary and Node.
Original and generated toolchain expressions agree with mocked package sets
for both Darwin and Linux branches.
A complete Nix package build is outside this generation-only change; package
build behavior has not been independently revalidated.

## Composable constructor API

Added a public entrypoint that exposes `Flake`, `Input`, `BinaryCache`, `Package`,
`DevShell`, `Outputs`, `Shell`, `Import`, `EachSystem`, `NixFunction`, `Let`, and
`AttrSet`. Builder methods return new objects; TypeScript helpers can return
resources and compose them with `Outputs`. Callback handles use private names to
avoid variable capture and infer declared input/argument names. Raw expressions
remain an explicit escape hatch. Construction is synchronous and generated names
are deterministic within each composed expression.

Rewrote Ant's generator into named toolchain, package-output, and system-output
functions. A subsequent readability pass preserved generated files byte-for-byte.

Expanded Nix evaluation tests cover nested same-name callbacks, shared bindings,
builder reuse, default output aliases, shell composition, and flake metadata and
outputs with mocked dependencies. Original and generated flake evaluations agree
with mocked nixpkgs on Darwin and Linux for clean, dirty, and unknown revisions.
This checks the wiring; it does not replace a full nixpkgs evaluation or build.
TypeScript fixtures verify inferred names and reject incorrect callback types.
