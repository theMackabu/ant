# Node Core JavaScript Shims

Status: active
Last reviewed: 2026-07-29
Owner: theMackabu

## Goal

Determine whether Ant can reuse selected JavaScript implementations from a
pinned Node.js release as the compatibility layer for `node:*` modules while
keeping I/O, memory-heavy operations, and compute-heavy primitives in Ant's
native runtime.

The intended split is:

```text
Node JavaScript: API shape, validation, errors, orchestration, edge cases
Ant native code: I/O, buffers, parsing, encoding, compression, crypto, threads
```

Success means Ant can adopt useful upstream behavior without committing to
emulating all of Node, regressing startup or steady-state performance, or
exposing Node's internal binding interface to user code.

## Existing Implementation

- `src/builtins/node/` already contains bundled JavaScript implementations of
  several `node:*` modules.
- `src/tools/gen_builtin_bundle.js` bundles those modules at build time,
  preserves imports of `node:*` and `ant:*` modules as externals, and registers
  both prefixed and unprefixed aliases.
- `src/esm/builtin_bundle.c` and `src/esm/loader.c` load bundled JavaScript
  modules before filesystem resolution.
- `src/main.c` separately registers native implementations for most of Ant's
  current Node-compatible standard library.
- Ant therefore already has the packaging and loader shape needed for a pilot.
  The missing piece is a deliberately scoped compatibility contract for the
  private facilities used by Node's own JavaScript sources, such as
  `primordials`, `internal/*`, and `internalBinding()`.

## Scope

- A pinned, auditable subset of upstream Node JavaScript sources.
- Build-time transforms or wrappers needed to load those sources in Ant.
- Private Ant-owned adapters for the minimum internal bindings required by the
  selected modules.
- Compatibility, startup, bundle-size, and runtime benchmarks that decide
  whether each adopted module remains JavaScript or native.
- Documentation of the upstream revision, local patches, licensing, and update
  procedure.

## Non-Goals

- Embedding Node or V8.
- Supporting arbitrary Node internal modules as public APIs.
- Implementing N-API, native addons, or Node's complete C++ binding surface.
- Replacing proven hot native paths merely to reduce the amount of Ant-owned
  code.
- Following Node `main` continuously. Ant should pin a release and update it
  intentionally.
- Making `internalBinding()` or unrestricted `internal/*` resolution available
  to application code.

## Constraints

- Preserve constructor, prototype, and singleton identity across `node:name`
  and `name` aliases.
- Keep syscall, event-loop, GC, isolate, and resource ownership in Ant.
- Keep tight buffer loops and frequent native operations out of chatty
  JavaScript-to-C call sequences.
- Treat Node's private internals as an unstable vendored ABI. Every supported
  binding property must be explicit and tested.
- Do not silently fall back to the host `node` executable at runtime.
- Keep build provenance reproducible and include the upstream license and
  required attribution.
- Avoid source rewrites that make upstream updates impossible to review.
  Prefer a small transform plus Ant-owned adapter modules.
- Preserve Ant-specific APIs and behavior unless Node compatibility is the
  explicit contract for the affected `node:*` surface.

## Task List

### 0. Freeze a feasibility baseline

- [ ] Select and record a Node release, full source commit, and license text for
      the spike.
- [ ] Record Ant's current behavior for candidate modules using focused spec
      tests and a matching run under the pinned Node binary.
- [ ] Inventory the current native and bundled-JavaScript `node:*` modules,
      including which implementation wins for every alias.
- [ ] Extract the upstream dependency graph for two or three cold-path pilot
      modules.
- [ ] Classify every dependency as:
      - ordinary JavaScript;
      - `primordials`;
      - another `internal/*` module;
      - native `internalBinding()` access;
      - V8- or Node-bootstrap-specific behavior.
- [ ] Choose pilots only after the dependency graph is known. Prefer one
      mostly-pure module and one module requiring a small read-only binding.
- [ ] Use the existing `querystring` and `diagnostics_channel` shims as
      comparison points, but do not replace either until the upstream source is
      shown to improve compatibility or maintenance cost.
- [ ] Define stop conditions for the spike: excessive adapter surface,
      unsupported engine semantics, unacceptable bundle size, or worse
      performance than maintaining the existing implementation.

### 1. Build a repeatable upstream import

- [ ] Add a script or documented command that imports only the selected files
      from the pinned Node source revision.
- [ ] Store a manifest containing the Node version, commit, source paths,
      content hashes, license, and local patch list.
- [ ] Decide whether imported files are checked in verbatim or materialized
      during a controlled update step. The normal Ant build must not fetch the
      network.
- [ ] Reject unrecorded source drift during regeneration.
- [ ] Keep generated or vendored upstream files separate from Ant-owned
      adapters so reviews can distinguish provenance from implementation.
- [ ] Add an update procedure that produces a readable diff between two pinned
      Node revisions.

### 2. Provide private bootstrap facilities

- [ ] Inventory the exact primordial names used by the pilot dependency graph.
- [ ] Provide frozen or otherwise protected primordial references with the
      semantics required by those modules.
- [ ] Decide whether `primordials` are injected by the builtin bundler or
      imported from a private Ant module.
- [ ] Add resolution for an allowlisted private namespace corresponding to the
      required `internal/*` modules.
- [ ] Ensure application imports cannot reach that namespace.
- [ ] Implement a private `internalBinding(name)` adapter that rejects every
      binding outside an explicit allowlist.
- [ ] Define each allowed binding as a small, typed Ant-owned interface rather
      than exposing broad runtime objects.
- [ ] Preserve one canonical namespace and object identity when several
      upstream modules request the same binding.
- [ ] Add deterministic startup errors for missing bindings and missing binding
      properties, including the importing builtin's name.
- [ ] Verify bootstrap ordering does not depend on user-mutable globals.

### 3. Teach the builtin pipeline about vendored Node sources

- [ ] Extend `gen_builtin_bundle.js` to distinguish Ant-authored builtins,
      upstream Node sources, and private internal dependencies.
- [ ] Preserve useful source names and stack traces after bundling.
- [ ] Confirm esbuild does not rewrite required property access, primordial
      identity, or CommonJS wrapper semantics.
- [ ] Support only the upstream source formats needed by the pilots; do not
      grow a general Node bootstrap loader prematurely.
- [ ] Detect unsupported Node bootstrap globals at build time where possible.
- [ ] Produce per-module and total bundled-byte reports.
- [ ] Ensure unused internal modules are excluded from the final binary.
- [ ] Add a focused test proving `node:name` and `name` resolve to the same
      exports and do not instantiate the upstream module twice.

### 4. Land the first mostly-pure pilot

- [ ] Import the chosen module and its minimal JavaScript-only dependency
      closure.
- [ ] Add compatibility tests for exports, property descriptors, prototypes,
      invalid inputs, error classes/codes, and representative edge cases.
- [ ] Differentially run the focused cases under pinned Node and Ant.
- [ ] Document intentional differences instead of patching tests to accept
      both behaviors silently.
- [ ] Compare implementation size and local patch count with Ant's existing
      module.
- [ ] Measure cold import time, first call, repeated calls, and binary-size
      impact.
- [ ] Keep or revert the pilot based on the acceptance gates below.

### 5. Land one small native-binding pilot

- [ ] Select a module whose binding is narrow and not on a tight per-byte or
      per-event hot path.
- [ ] Implement only the binding properties reached by the pinned upstream
      dependency graph.
- [ ] Add tests for invalid binding names, absent properties, repeated access,
      namespace identity, and initialization failure.
- [ ] Count JavaScript-to-native crossings for representative operations.
- [ ] Confirm resources created by the binding follow Ant's GC and event-loop
      ownership rules.
- [ ] Compare behavior and performance against both pinned Node and the prior
      Ant implementation.
- [ ] Record whether the adapter appears reusable or is merely disguising a
      module-specific port.

### 6. Establish hot-path policy

- [ ] Add microbenchmarks for each adopted module covering cold imports,
      normal calls, and adversarial large inputs.
- [ ] Add representative end-to-end benchmarks so fewer lines of native code
      cannot hide regressions from repeated boundary crossings.
- [ ] Record a baseline for startup time, peak memory, final binary size, and
      bundled builtin bytes.
- [ ] Define a review threshold for promoting an operation back to native code.
- [ ] Keep hashing, compression, encoding, buffer copying/search, socket and
      filesystem operations, parsers, serialization, and worker transport
      native unless measurements demonstrate otherwise.
- [ ] Allow JavaScript state machines around native primitives when crossings
      occur at meaningful chunks rather than per byte or per element.
- [ ] Require benchmark evidence before replacing an already-correct native
      implementation on a known hot path.

### 7. Harden compatibility and isolation

- [ ] Verify user mutation of globals and prototypes cannot corrupt protected
      Node shims when upstream expects primordials.
- [ ] Verify builtin modules do not gain filesystem or network authority beyond
      the Ant APIs deliberately exposed to them.
- [ ] Test cyclic private-internal dependencies and partial initialization.
- [ ] Test CommonJS/ESM interop, dynamic import, `createRequire()`, loader hooks,
      snapshots, workers, and sandbox execution for adopted modules.
- [ ] Check stack traces and error provenance for both upstream JavaScript and
      Ant native adapters.
- [ ] Add GC stress coverage for cached namespaces and binding-owned objects.
- [ ] Fuzz or differential-test parsers and input-normalization functions when
      an adopted shim accepts untrusted strings or buffers.

### 8. Decide whether to expand

- [ ] Summarize the two pilots: compatibility delta, adapter size, local patch
      count, performance, binary size, and maintenance cost.
- [ ] Define supported categories:
      - adopt upstream JavaScript;
      - use upstream JavaScript with a narrow native binding;
      - retain an Ant-native implementation;
      - unsupported or intentionally divergent.
- [ ] Rank additional cold-path modules by expected compatibility gain divided
      by binding and maintenance cost.
- [ ] Create separate follow-up plans for large dependency families such as
      streams or HTTP rather than silently expanding this spike.
- [ ] Keep `worker_threads` out of the shim-adoption decision until Ant's
      isolate ownership, structured clone/transfer, and event-loop model meet
      the required semantics.
- [ ] Document how and when the pinned Node baseline is updated.

## Acceptance Gates

Adopt an upstream shim only when all of the following are true:

- Its upstream revision and complete dependency closure are reproducible.
- Its private binding surface is explicit, allowlisted, and inaccessible to
  application code.
- Focused compatibility tests pass, with intentional deviations documented.
- It does not weaken Ant's resource ownership, sandbox, or GC invariants.
- Cold-start, memory, binary-size, and representative runtime costs are
  measured and accepted.
- The upstream source plus adapters is easier to maintain than the Ant-owned
  implementation it replaces.

Failure of a gate is a valid outcome. The spike should identify where upstream
reuse is valuable, not force every Node module through the same architecture.

## Validation

During the feasibility spike:

- `meson compile -C build`
- focused `tests/test_<name>.cjs` files for loader and binding behavior
- focused `./build/ant examples/spec/run.js <module>` runs
- the same behavioral fixtures under the pinned Node binary
- startup, import, repeated-call, memory, and bundle-size measurements

Before landing an adopted module:

- `maid preflight`
- all additional validation recommended by preflight
- the module's focused spec and regression tests
- affected CommonJS, ESM, loader-hook, sandbox, and snapshot tests
- `./build/ant examples/spec/run.js --all` when the module can affect shared
  loader or builtin-bootstrap behavior

Do not use wall-clock thresholds as normal correctness assertions. Store raw
benchmark commands and results, compare multiple samples, and use them as
review gates.

## Milestones

1. A pinned Node baseline and dependency report identify viable pilots.
2. One mostly-pure upstream Node module runs through Ant's builtin loader.
3. One narrow, allowlisted Ant native binding supports an upstream module.
4. Compatibility, isolation, startup, size, and runtime gates are measured.
5. Ant either adopts the successful pilots or records evidence for stopping.
6. A ranked module matrix guides any later expansion.

## Decision Log

- 2026-07-29: Treat upstream Node JavaScript as a possible compatibility
  frontend over Ant-native primitives, not as a replacement for Ant's runtime.
- 2026-07-29: Start with cold-path pilots and measured boundary crossings.
  Native code remains the default for I/O, memory-heavy, compute-heavy, and
  per-byte/per-element work.
- 2026-07-29: Pin one Node release and expose only a private, allowlisted subset
  of its unstable internal ABI.
- 2026-07-29: Use the existing bundled-builtin loader rather than creating a
  second module system.
- 2026-07-29: Make stopping after the feasibility spike an explicit successful
  result if maintenance, isolation, size, or performance gates fail.

## Validation Status

- Confirmed Ant already bundles JavaScript modules from `src/builtins/` and
  resolves their `node:*` and unprefixed aliases before normal filesystem
  resolution.
- Confirmed Ant also registers native standard libraries independently, so
  migration must define which implementation owns each canonical module.
- Confirmed the current tree contains Ant-authored JavaScript shims suitable as
  comparison points.
- No upstream Node sources, bootstrap adapters, runtime behavior, or build
  outputs have been changed.

## Follow-Ups

- Select the pinned Node release and initial pilot modules.
- Decide whether imported upstream sources are checked in verbatim or produced
  by an explicit maintainer-only regeneration command.
- Decide acceptable startup, binary-size, and runtime regression budgets before
  evaluating the first pilot.
- If the spike succeeds, create a compatibility matrix covering all current
  `node:*` modules without assuming they should share one implementation
  strategy.
