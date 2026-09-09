# Execution Plans

Status: active
Last reviewed: 2026-04-09
Owner: theMackabu

Use this directory for durable, versioned plans when work spans multiple
decisions, checkpoints, or follow-up changes.

## Layout

- Active plans: [active/README.md](active/README.md)
- Completed plans: [completed/README.md](completed/README.md)
- Technical debt tracker: [tech-debt.md](tech-debt.md)

## When To Create A Plan

- The task spans multiple subsystems.
- The work will happen across multiple commits or pull requests.
- Validation has meaningful risk, tradeoffs, or deferred follow-ups.
- Future contributors will need the reasoning, not just the final diff.

## Plan Expectations

- State the problem, constraints, and intended outcome up front.
- Keep a short decision log as the work evolves.
- Record validation status and unresolved risks.
- Move finished plans into `completed/` once the work is done.

`todo/` can still hold scratch notes, but durable execution history belongs in
this directory.

## Separate runtime artifacts

The platform workflow uploads `ant-runtime-<target>` separately from `ant-<target>`.
Derive the runtime artifact name from the configured Ant artifact name so musl
target naming stays consistent. `maid download` skips runtime artifacts; the API
resolves their own artifact IDs and extracts `ant-runtime` (or `ant-runtime.exe`).
The runtime resolver retains run selection and release fallback, including ZIP
extraction for runtime release assets. Version metadata still comes from the
matching `version-ant-<target>` artifact. Default downloads also skip bench-v8
scores; `maid download -- --all` downloads every artifact, including runtime,
version, and score ZIPs. Artifact enumeration follows all API pages.

Validation: `cd docs/api && bun test` passes 18 tests covering all targets,
explicit/latest runs, extraction, release fallback, and download filtering.
Native build/reconfigure and spec runs do not validate this packaging-only change;
the next CI build must produce the new artifacts before the updated API is deployed.

## Eval binding fixes

- `typeof arguments` skips the implicit object when the function owns an eval
  environment, retaining the existing undefined-safe lookup fallback.
- The eval pre-scan checks parameter patterns and scoped body declarations before
  deciding `owns_eval_env`. Body bindings do not shadow parameter initializers;
  block, loop, catch, and switch bindings do not hide calls outside their scope.
  Static local/upvalue and spread eligibility checks remain in place.
- Inherited eval calls resolve the callee before arguments and compare it with
  the same named builtin installed by global initialization (`js_builtin_eval`).
  Arguments are emitted once with the identity flag kept above them on the stack,
  then dispatch selects an ordinary call or `OP_EVAL`. This uses existing opcodes;
  inherited builtin calls use runtime eval rather than literal inlining.

Validation: the saved pre-fix binary fails all eight core parameter/body-shadowing
cases with `value` undefined and fails the nested-bytecode growth regression
(541 to 8,941 bytes at depths 4 and 8). The fixed function grows linearly (148,
280, and 412 bytes at depths 4, 8, and 12). Focused eval regressions pass, including
scope-boundary and argument-effect controls. The full spec suite passes 4,221
tests across 102 files. Repository preflight and diff checks pass.

The user's normal `maid build` succeeds. In this agent environment, compilation
requires the Xcode `SDKROOT`, and the default linker rejects `libpkg.a` alignment.
Validation used the configured Ninja library target and the generated link
command with `-Wl,-ld_classic`, followed by ad-hoc codesigning. No build settings
or third-party sources were changed.
