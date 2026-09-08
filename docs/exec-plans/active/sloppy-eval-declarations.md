# Sloppy Direct Eval Declarations

Status: active
Last reviewed: 2026-09-07
Owner: theMackabu

## Goal

Make sloppy direct eval instantiate var and function declarations in the
caller's variable environment. Keep strict eval and lexical declarations
isolated, and preserve the existing test_eval.cjs assertions.

## Cause and boundary

`hoist_var_pattern` and `hoist_lexical_decls` compile eval declarations into
the eval function's local slots. `sv_eval_in_frame` creates a temporary
environment that exposes existing caller cells but no persistent variable
environment for new names. Caller references to undeclared names are compiled
as globals. The test consequently throws ReferenceError after eval returns.

Changing the declaration store alone cannot fix caller resolution; publishing
names globally would violate function isolation. Extend the existing eval
environment mechanism and compiler resolution for functions that can perform
sloppy direct eval. Keep ordinary calls and functions without eval on their
existing paths. This does not require a general scope or call-ABI redesign.

The semantic reference is
[PerformEval and EvalDeclarationInstantiation](https://tc39.es/ecma262/multipage/global-object.html#sec-evaldeclarationinstantiation).

## Minimal-fix boundary review

The failure is deterministic and reachable through ordinary direct eval.
The saved baseline still fails `tests/test_eval.cjs:32` with
`ReferenceError: 'leaked' is not defined`; Node passes the complete file.

Verified causal chain:

- `src/silver/compiler.c:1895` (`hoist_var_pattern`) allocates eval vars as
  local slots. `hoist_lexical_decls` at line 2127 and `hoist_one_func` at
  line 2248 likewise allocate and initialize function declarations locally.
- `src/silver/ops/calls.h:298` (`sv_eval_in_frame`) creates a temporary
  capture object and passes it to the eval activation. It does not install
  a persistent variable environment on the caller.
- `src/silver/compiler.c:669` selects environment lookup only for
  `inherits_eval_env`; ordinary caller references resolve through local,
  upvalue, or global instructions. `src/silver/ops/globals.h:270` reports
  the missing global name after eval returns.
- Closure creation at `src/silver/ops/upvalues.h:244` captures the existing
  frame environment. Installing a new environment only when eval executes
  would leave closures created earlier unable to see the new declarations.
- Existing capture state (`src/silver/ops/eval_env.h`) stores a fixed list
  of captured cells. Its lookup and store helpers cannot add caller slots.
  GC marks those cells and frees the state; it cannot publish declarations
  or repair name resolution.
- `sv_runtime_binding_t` (`include/silver/engine.h:277`) records location
  kind and constness, but not the distinction between mutable lexical and
  var declarations needed for eval declaration-conflict checks.

Interventions, smallest first:

1. Change the declaration destination: insufficient because subsequent caller
   instructions still resolve missing names globally.
2. Publish declarations on the global object: rejected because eval inside a
   function must not expose its new vars to unrelated functions or indirect
   eval. Rejecting declarations instead also fails the requested behavior.
3. Special-case literal eval strings: rejected because dynamic source follows
   the same defect chain, and eager static hoisting would expose declarations
   before the eval call executes.
4. Extend the compiler/runtime environment contract: persistent per-activation
   variable storage, dynamic lookup across affected function boundaries,
   capture shared with closures created before eval, and lexical-declaration
   conflict metadata. This is the smallest identified correct general fix.

The user approved option 4 on 2026-09-07 after this boundary review.
The implementation changes the internal binding contract and keeps the native
call ABI unchanged.

## Implementation

Sloppy functions that may execute dynamic direct eval get a persistent variable
environment at entry. Eval declarations are instantiated there before execution,
with lexical conflicts checked before any names are published. Existing var and
parameter declarations continue to use captured live cells. Strict eval and
lexical declarations retain their isolated local slots.

Closures capture lexical cells above that shared variable environment, including
closures created before eval runs. Name resolution stops static upvalue lookup
where an eval declaration could shadow an outer binding. Capture metadata records
lexical/catch binding kinds and import resolution information. Import names are
interned because namespace lookup expects null-terminated names.

Literal expression evals retain their existing inline path; the prescan also
checks nested eval expressions. Ordinary functions and calls without an affected
eval scope use their existing bytecodes. New environment initialization and
closure capture instructions are interpreter-only. This is scoped declaration
support, not a general scope redesign or a claim of complete eval conformance.

## Validation

- Fresh matching baseline built from `c0c6c299`, using the same toolchain,
  optimization options, and existing PGO profile as the candidate. The profile
  was not modified. The earlier stale-build hang did not recur after rebuilding.
- Negative control: the matching baseline fails `tests/test_eval.cjs` with
  `ReferenceError: 'leaked' is not defined` and the new declaration test with
  `ReferenceError: 'evalVar' is not defined`.
- Original assertions remain unchanged. Both new tests pass in Node and Ant.
- All 14 focused tests pass: eval declarations, import bindings, lexical and
  strict environments, JIT eval lookup, new.target, module reentrancy, closure/GC
  churn, loop captures, and WebSocket buffered frames.
- Full spec suite: 4,221 tests, 102 files, zero failures.
- JIT suite: 10 files, zero failures.
- Configured Meson build succeeds. PGO reports expected control-flow hash
  mismatches in changed functions; no profile retraining was performed.

The declaration test covers dynamic source, repeated eval, existing var and
parameter cells, lexical conflicts, strict isolation, declaration instantiation
before abrupt completion, destructuring, loop declarations, Annex B block
functions, catch bindings, arguments reassignment, deletion, activation isolation,
and closures created before/during eval and retained after return.

## Performance and costs

Eight alternating before/after rounds used separately saved binaries with
matching build settings. Medians: ordinary JS calls -0.18%, literal expression
eval -0.46%, async cases -0.66% to -2.78%. The largest positive differences were
Date.now calls +0.52% and sub-millisecond empty-object construction +1.43%.
Other applications were active, so these measurements do not establish exact
zero overhead or causal speedups for unrelated paths.

A separate dynamic-eval control, creating a fresh activation and evaluating
`x + 1` 20,000 times, measured 48.29ms before and 55.28ms after (+14.47%). The
persistent variable environment and arguments/cell capture have a real cost.
The new bytecodes also keep affected functions out of the JIT. Optimizing those
allocations or adding JIT lowering is follow-up work, not hidden inside this
correctness change. The implementation does not claim zero slowdown for dynamic
eval.

Reproductions, matching binaries, build logs, validation logs, and raw benchmark
samples are under `/tmp/ant-eval-declarations` (local artifacts, not durable
repository dependencies).

## Work

- [x] Reproduce and verify the causal chain and negative control.
- [x] Obtain approval for the internal binding contract change.
- [x] Implement persistent caller declarations and closure resolution.
- [x] Cover declaration instantiation and affected binding kinds.
- [x] Run focused tests, full spec/JIT suites, and matched performance controls.
- [x] Run final repository documentation and structure guards: knowledge, structure,
  preflight, and diff whitespace checks pass.

Implementation is ready for review; keep this plan active until the change lands.
