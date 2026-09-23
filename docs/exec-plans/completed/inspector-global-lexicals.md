# Inspector Global Lexical Resolution

Status: complete
Last reviewed: 2026-09-22
Owner: theMackabu

## Scope and decisions

The REPL stores lexical bindings outside the global object. Inspector safe
evaluation and member completion now resolve those bindings before global
properties. The CDP name list reports each shadowed name once.

Both inspector resolvers and name enumeration use a read-only wrapper over
the existing lexical index. Lookup does not allocate interned names, run
getters, or turn a TDZ binding into a global-property fallback. Property
traversal and side-effect checks remain intact. A new environment representation or
ordinary evaluation fallback is unnecessary and would broaden the fix or run
user code during previews.

CDP and PTY regressions cover the corrected paths, with additional coverage
for the preceding eval-delete, Annex B, and lexical-index growth fixes.
Trailing whitespace introduced by `bfe16b7b` was removed from its affected lines.

## Validation

- Negative controls against the binary from `bfe16b7b`: the CDP test rejected
  `lexicalNumber` with `EvalError: Possible side-effect in debug-evaluate`; the
  PTY test timed out waiting for the `viewMember` completion suffix.
- Both new tests and six existing eval, inspector, and REPL test files pass.
  Coverage includes missing and undefined names, TDZ shadowing, getters/proxies,
  name deduplication, imports, index growth over 160 bindings, and GC.
- Full spec suite: 4,240 tests in 102 files passed. No operand-depth or JIT
  virtual-stack overflow diagnostics appeared in these runs.
- Configured Meson build, preflight, knowledge checks, and diff whitespace
  checks passed. The local build used the installed macOS 15.4 SDK and Nix
  cctools linker required by this configured tree.
- No bytecode handler behavior changed; the all-runtime stack-depth harness
  was not run.
