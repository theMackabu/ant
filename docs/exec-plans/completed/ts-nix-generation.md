# TypeScript Nix Generation

Status: completed
Last reviewed: 2026-09-05
Owner: theMackabu

## Outcome and layout

The dependency-free API in `packages/ts-nix/` builds Nix expressions with typed
constructors and reusable TypeScript functions. Six definitions live under
`packages/nix/src/`: flake, package, shell, toolchain, vendor, and version.
`packages/nix/generate.ts` writes their corresponding files under
`packages/nix/generated/`, or checks them without writing with `--check`.

A generated root `flake.nix` retains literal metadata and a function forwarding
to the generated flake's outputs. Nix requires this root entrypoint and checks its
shape before evaluating outputs; a bare import or attribute selection cannot
replace it. All generated files remain versioned so Nix can bootstrap Ant without
an installed Ant runtime. Paths are relative to the generated files.

The vendor-hash updater edits `src/vendor.ts`, regenerates outputs, and restores
both the source and generated vendor file if the build fails to report a new hash.
It checks for existing generation drift before temporarily substituting a fake hash.

## API and rendering decisions

Constructors include `Flake`, `Input`, `BinaryCache`, `Package`, `DevShell`,
`Outputs`, `Shell`, `Import`, `EachSystem`, `NixFunction`, `Let`, and `AttrSet`.
Builder methods return new objects. TypeScript infers declared callback argument
and input names. Functions support default arguments; `str` interpolates Nix
expressions into escaped strings; `.set()` accepts dynamic attribute names.

Expressions retain structure until rendering. The printer handles indentation,
operator precedence, and readable multiline shell strings. Callback bindings
retain identity, so ordinary names print directly and collisions get suffixes
and aliases only where necessary. This replaces eager string concatenation and
blanket generated aliases. Trusted raw Nix is preserved verbatim; the generator
cannot infer its binding semantics. The API does not compile arbitrary TypeScript
into Nix or type-check nixpkgs attributes.

## Validation and limitations

- Ant and Node tests evaluate emitted expressions with Nix, including nested
  same-name callbacks, defaults, builder reuse, dynamic keys, precedence, raw
  multiline strings, and indented-string escaping.
- TypeScript checks include all six definitions and expected-error fixtures.
- Original and generated definitions agree in mocked Nix evaluations for Darwin
  and Linux, clean/dirty/unknown revisions, native tuning, and PGO branches.
  Shell scripts and other evaluated derivation attributes are compared as values.
- The root wrapper and generated flake produce equivalent mocked outputs. All
  files parse with Nix; offline flake metadata and generation drift checks pass.
- The vendor-hash updater was checked in isolated repositories with a simulated
  hash mismatch and an unrelated build failure; regeneration and restoration pass.
- Repository preflight and knowledge checks pass. The recommended C rebuild was
  attempted earlier, but local Meson configuration failed in libuv because the
  configured toolchain could not find library `m`. No C runtime code changed.
- Mock evaluation does not replace a full nixpkgs evaluation or package build;
  a complete Nix build has not been run for this migration.
