# Automatic GC Rooting for C Runtime Code

Status: active
Last reviewed: 2026-09-06
Owner: theMackabu

## Goal

Remove handwritten local-root bookkeeping from Ant-owned C runtime code by
generating GC root frames during compilation. Contributors should be able to
write ordinary C declarations and expressions without deciding where to insert
`GC_ROOT_PIN`, `GC_ROOT_SAVE`, `GC_ROOT_RESTORE`, or their per-variable
replacement.

The intended source remains:

```c
ant_value_t source = js_getprop_fallback(js, regexp, "source");
if (is_err(source)) return source;

source = js_tostring_val(js, source);
if (is_err(source)) return source;
```

The compilation pipeline must generate the storage, registration, and lifetime
management that make this code safe when getters, conversions, allocations,
callbacks, and nested JavaScript execution trigger collection.

This is a compiler/runtime integration project. It is not a plan to replace
the collector, switch the runtime to C++, change the public value ABI, or
disable collection while native code runs.

## Status and Authorization

The user selected compiler-generated roots as the desired architectural
direction and requested this detailed task document. The initial investigation
was read-only. Creating this plan and its index entry is the only implementation
work performed by this documentation change.

No root-frame implementation, compiler integration, runtime migration,
compiler-support change, build, runtime test, or benchmark has been completed
for this design. The phases below describe future work. Proposed interfaces,
file names, diagnostics, and experiments are design sketches, not existing
commands or implemented contracts.

The direction is selected; the exact transformation mechanism is not. The
first milestone must settle feasibility across supported toolchains before
committing to a repository-wide migration.

## Success Criteria

The project is complete only when all of the following hold:

1. Covered Ant-owned C code uses ordinary `ant_value_t` declarations and
   expressions without handwritten local-root operations.
2. Generated protection includes arguments, named locals, expression
   temporaries, supported aggregates, and owners of derived pointers.
3. Roots remain correct during C-to-JavaScript-to-C re-entry and on all
   supported normal and exceptional control-flow paths.
4. Collection can continue inside allocating native operations and nested
   callbacks. Memory safety does not depend on postponing GC indefinitely.
5. Native and Wasm builds meet the same local-root correctness contract.
6. Existing supported GCC, Clang, Apple Clang, and MinGW workflows are either
   supported by the chosen pipeline or have an explicit, separately reviewed
   toolchain decision. They cannot disappear as an incidental consequence.
7. Unsupported root-relevant constructs produce actionable build failures
   in migrated code. They cannot silently fall back to unprotected C.
8. Persistent handles and native containers have documented ownership and
   tracing contracts; removing local macros does not remove those obligations.
9. Optimized correctness, retention, stack usage, build cost, and runtime cost
   satisfy recorded acceptance gates.
10. Migration coverage is mechanically accounted for, and temporary rollout
    machinery has either been removed or given a specific remaining purpose.

Zero occurrences of the old macros is not sufficient evidence of success.

## Scope and Non-Goals

### In scope

- A small, versioned internal root-frame contract consumed by the existing GC.
- A semantic compilation step that generates roots for Ant-owned C code.
- Explicit classification of GC-bearing values, pointers, and aggregate fields.
- Initialization, lifetime, derived-pointer, and call-effect analysis.
- Native and Wasm build integration, diagnostics, source mapping, caching,
  cross-compilation, and compatibility checks.
- A verifier and adversarial fixtures that can expose missing generated roots.
- Staged migration of the current root macros and direct local-root calls.
- Clear boundaries around persistent handles, containers, embedding, and JIT
  frames that the C transformation does not own.

### Out of scope for the initial implementation

- A new heap collector, compaction policy, nursery layout, or allocation ABI.
- A general-purpose garbage-collected dialect of arbitrary C.
- A new JavaScript bytecode format or replacement of the MIR JIT.
- Automatic discovery of arbitrary references concealed in external memory.
- Making opaque native callback contexts or async resources automatically
  traced without an ownership contract.
- Automatically generating every heap write barrier or object trace function.
- Removing conservative scanning from the entire production runtime.
- Global conversion of every C integer or pointer into a rooted wrapper.
- A permanent split where only developer builds get automatic protection.
- Performance claims inferred from fewer source macros or fewer calls.

## Current Source Evidence

Baseline for this document:
`001af42f4a2960a86ccd61fb3bdd4a3259260ace`, inspected on 2026-09-06.
The working tree was clean before this plan was added. Refresh this baseline
and the census before implementation; source locations and counts can drift.

At this snapshot, a textual search under `include/` and `src/` found 556
`GC_ROOT_PIN`, 181 `GC_ROOT_SAVE`, and 537 `GC_ROOT_RESTORE` occurrences
across 37 files. These counts include macro definitions and do not represent
the number of semantically necessary roots. They exclude other surfaces such
as `packages/wasm/` and do not count direct helper calls.

| Surface | Current behavior | Design consequence |
| --- | --- | --- |
| [Root declarations](../../../include/gc/roots.h) | Macros register local addresses and manually restore a saved count | Lifetime management is distributed through callers |
| [Root implementation](../../../src/gc/roots.c) | Local slots use a reallocating pointer array; temporary handles own copied values in a separate scope chain | Generated frames must coexist with these mechanisms during migration |
| [Value types](../../../include/types.h) and [encoding](../../../include/value.h) | `ant_value_t` is a `uint64_t` typedef containing numbers or tagged payloads | GC type information must survive frontend lowering |
| [Cage encoding](../../../include/cage.h) | Native payloads encode cage offsets; Wasm payloads encode linear-memory addresses | Integer shape alone is not a root-kind specification |
| [Allocation](../../../src/pool.c) | `js_type_alloc` can call `gc_run` before allocating | Constructors and ordinary allocation helpers may collect |
| [Objects and native calls](../../../src/ant.c) | `obj_alloc` can invoke GC; `sv_call_native` dispatches into C builtins | Native entry/exit and constructor boundaries need explicit contracts |
| [Closure allocation](../../../include/silver/engine.h) | `js_closure_alloc_prepare` can call `gc_pressure` | The collecting-call inventory must include inline headers |
| [GC traversal](../../../src/gc/objects.c) | VM slots, frames, activations, explicit roots, native subsystems, and conservative C stack words are traversed | Generated frames add a root source; they do not replace every existing source |
| [String marking](../../../src/gc/strings.c) | Marking identifies expected string allocation starts | A raw pointer into bytes does not automatically preserve its owner |
| [RegExp conversion](../../../src/modules/regex.c) | Source and flags access/conversion can re-enter JavaScript | A native temporary must survive collection in a nested getter |
| [Blocking await](../../../src/reactor.c) | C holds values while pumping the event loop | Native frames can remain active across extensive nested work |
| [JSON](../../../src/modules/json.c) | Temporary handles preserve values in algorithm state | Generated stack roots do not replace dynamic container ownership by themselves |
| [Inspector](../../../src/inspector/runtime.c) and [N-API scopes](../../../src/napi/references.c) | Some direct root-stack uses represent longer-lived handles | A global search-and-replace would confuse distinct lifetimes |
| [JIT generation](../../../src/jit/) | MIR emits its own value storage and selected root-related spill slots | A C compiler pass does not automatically instrument generated MIR code |
| [Wasm build](../../../packages/wasm/meson.build) | A separate build compiles shared runtime C with `ANT_WASM_EMBED` | Both build graphs must participate |

Reproduce and extend the census with:

```sh
rg -n 'GC_ROOT_(PIN|SAVE|RESTORE)|gc_push_root|gc_pop_roots|gc_root_scope' include src packages/wasm
rg -n 'gc_temp_root_|gc_register_root' include src packages/wasm
rg -n '\b(gc_run|gc_run_minor|gc_maybe|gc_pressure)\(' include src packages/wasm
```

These are discovery commands, not a sound call-graph analysis. Search results
must be classified by lifetime, ownership, and reachable collection effects.

## Why This Direction

### Scoped rooted declarations remain a useful intermediate

`GC_LOCAL(js, name, initializer)` could combine declaration, registration, and
scope cleanup. It would remove manual restores and make loop/block lifetimes
safer. It still asks contributors to distinguish protected and unprotected
values and to name intermediate results when necessary.

It may serve as a test adapter or migration tool for the eventual frame
contract. A repository-wide macro conversion is not a prerequisite for this
project and should not become an expensive intermediate destination.

### Conservative scanning is not the selected complete solution

Conservative stack/register discovery is viable and is used by other engines.
Ant's current implementation, however, does not cover every raw/interior
representation, and Wasm locals need not reside in linear memory.

The project can retain conservative scanning for legacy or JIT execution while
making migrated C independently correct. It must not claim that successful
execution with the scanner enabled proves generated-root completeness.

### Collection deferral is not the selected complete solution

Moving collection to VM boundaries still leaves suspended C frames underneath
nested JavaScript execution:

```text
JavaScript -> C builtin -> JavaScript getter -> GC
                 |
                 +-- C temporary is needed after the getter returns
```

Deferring GC across the whole native dynamic extent can retain unbounded
garbage during callbacks, large native loops, or blocking event-loop pumping.
Re-enabling GC inside the callback requires protecting the suspended C state
again. This plan keeps collection available and generates that protection.

### Stack maps can be a later backend

Precise native stack maps could reduce some steady-state root-storage costs,
but require backend and stack-walking integration. Initial generated frames
provide a more inspectable contract and a common addressable representation
for native and Wasm. They do not require adopting LLVM's sample runtime ABI.

## Architecture Decisions to Settle First

### Transformation mechanism

Evaluate two bounded prototypes against the same fixture set:

| Candidate | Advantages | Obligations and risks |
| --- | --- | --- |
| A semantic frontend tool emits instrumented GNU C | Generated output can potentially be compiled by existing GCC and Clang backends; frames are inspectable; no LLVM runtime dependency is implied | Correct emission of expressions, macros, inline headers, GNU extensions, target types, and diagnostics is substantial work; a Clang parser must understand the selected target's source and headers |
| Frontend integration preserves GC metadata and lowers frames during compilation | Direct access to semantic types and generated temporaries; avoids a general C pretty-printer | Compiler API/version coupling; Apple Clang plugin availability; a separate GCC solution or a deliberate compiler-support change |

A generic late LLVM pass that guesses which `i64` values are `ant_value_t`
is not an acceptable implementation. Neither debug information nor variable
names are a correctness contract after optimization.

The first candidate is worth proving because
[BUILDING.md](../../../BUILDING.md) includes GCC/MinGW support. It is a candidate,
not a promise that Clang can transparently parse every GCC target configuration.
If it cannot, record the failing construct and choose a supported integration
instead of accumulating fragile text substitutions.

Select one production mechanism after the proof. Do not maintain two unrelated
rooting semantics merely to keep both experiments alive.

### Host tool versus target runtime

The generator runs on the build host and analyzes code for the target.
Cross-compiling Ant must not require executing the target binary. A host-side
LLVM/Clang tooling dependency does not imply shipping LLVM in the Ant runtime,
but its installation, versioning, build time, and distribution are real costs.

Record how to obtain the tool in local, Nix, CI, Windows, and wasi-sdk builds.
Do not require an unrecorded developer-local compiler installation.

### Root classification

Maintain one semantic classification shared by generation and verification:

- tagged `ant_value_t` values, including errors, symbols, strings, functions,
  objects, promises, generators, and BigInts where the representation applies;
- typed pointers to managed objects, closures, upvalues, ropes, and other
  managed allocations actually present in the source inventory;
- derived pointers and offsets with an identified owning allocation;
- aggregate fields and array elements containing the above;
- ordinary non-GC scalars and pointers that need no root entry.

Aliases, qualifiers, nested aggregate types, casts, and function signatures
must not accidentally erase the classification. Do not root every `uint64_t`
merely because the typedef has the same underlying type.

Each root kind needs an explicit visitor behavior. Reinterpreting every raw
pointer as an `ant_value_t` is incorrect. A pointer-to-slot also differs from
the value in the slot and from the allocation owning that slot.

### Collection effects

Define at least `may_collect` and `no_collect` effects in generated analysis
metadata. These need not be handwritten annotations on ordinary functions.

- Start from actual collection entrypoints and propagate through calls.
- Treat unknown indirect calls, external callbacks, and user-code invocation
  as potentially collecting unless a checked contract says otherwise.
- Include inline functions, cross-translation-unit calls, constructors,
  getters, setters, proxies, coercions, finalization paths, and event pumping.
- A `no_collect` exception must be justified and checked transitively.
- A hidden API-specific exemption cannot turn a collecting path into a leaf.
- Effect knowledge must be refreshed when bodies, targets, or build options
  change; stale call summaries must invalidate generated output.

Initial correctness may conservatively expose roots across all calls.
Effect analysis is required before eliding frames or shortening protection
based on a claim that a call cannot collect.

## Root-Frame Contract

The following describes required semantics, not a finalized C layout.

A generated activation has:

- a link to the previous generated activation in the appropriate execution
  context;
- enough ownership/context information to select the correct isolate;
- a static layout descriptor where possible;
- live slot addresses or owned root storage, with explicit initialization state;
- descriptors for supported dynamic ranges and typed pointer roots.

One frame can describe multiple values. The common fixed-size frame must not
grow the existing reallocating `js->c_roots` array or allocate on the heap.
Descriptor storage belongs in read-only generated data where practical.

### Frame and slot invariants

1. **Publish initialized state.** The collector cannot read a linked slot
   until its address, kind, and value are valid.
2. **Observe current values.** Reassignment must update what the collector
   sees. A copied initial value is not a root for later assignments.
3. **Match storage lifetime.** Remove or deactivate an entry before the C
   storage it describes ceases to exist.
4. **Never allocate during linkage or unlinkage.** Root maintenance cannot
   itself trigger GC or introduce a fallible registration gap.
5. **Restore the right chain.** Nested calls and recursive invocations must
   leave the execution context's prior head intact.
6. **Do not move linked descriptors by value.** Returning or copying a linked
   frame object would leave internal addresses and links invalid.
7. **Do not assume struct fields form a C array.** Use real slots or valid
   offset descriptors derived from actual layouts. Respect padding, alignment,
   aliasing, and target pointer width.
8. **Preserve observable ordering.** Root stores and linkage must remain
   observable to collection under inlining, LTO, and optimization.
9. **Handle returns without a gap.** Evaluate and protect the return result,
   unlink without collection, and deliver it to caller protection before the
   next possible collection. Apply equivalent rules to output parameters.
10. **Keep old and new mechanisms independent during rollout.** A legacy
    `gc_pop_roots` must not truncate generated frames or persistent handles.
11. **Preserve ordinary value ABI.** `ant_value_t` remains a scalar 64-bit
    value at existing native, JIT, and embedding boundaries.
12. **Do not depend on conservative rescue.** Migrated C's correctness must
    be demonstrable without its values being found accidentally on the stack.

The implementation should favor slots that are the authoritative value
storage. If it uses shadow copies instead, every write, aliasing operation,
and out-parameter update needs a proven synchronization rule. This choice is
a prototype gate because it changes both correctness and optimizer behavior.

### Initialization and activation

Function-entry frame creation does not mean scanning every local immediately.
Parameters can be exposed after entry setup; locals become visible only when
their storage and contents are valid.

Choose and document an initialization representation: null slot addresses,
per-slot active bits, initialized generated storage, or a combination. A single
"initialized prefix" is insufficient when branch-specific lifetimes create
holes.

Do not overwrite an uninitialized or const-qualified user variable with a
sentinel merely to simplify traversal unless the transformation proves that
the rewritten C has the same defined behavior. Partial aggregate initialization
and evaluation of initializers can themselves cross collection points.

### Frame lifetime and control flow

The generated lifetime logic must cover:

- fallthrough, early return, nested blocks, `break`, and `continue`;
- forward and backward `goto`, labels inside scopes, and switch dispatch;
- declaration initializers skipped by a jump;
- repeated execution of a declaration in a loop;
- conditional expressions, short-circuit operators, and statement expressions;
- recursion, inlining, and return-value evaluation;
- VLA and compound-literal lifetimes where the selected frontend supports them.

A cleanup attribute may be an implementation detail, but it is not a complete
control-flow proof. Decide separately how nonlocal exits, foreign unwinding,
Wasm traps, cancellation, and instance teardown affect the chain. Do not
assume normal C scope cleanup runs after `longjmp` or a Wasm trap.

Where a boundary can recover and reuse an isolate after abnormal unwinding,
it must restore a saved generated-frame head before another collection.
Otherwise define and enforce that the instance is unusable and is torn down
without traversing stale frames.

## Expression Temporaries and Derived References

### Unnamed results are part of the contract

In the illustrative expression:

```c
combine(js, make_left(js), make_right(js));
```

the result evaluated first must survive evaluation of the other operand.
Protecting only source-level declarations misses this case.

The transformation must preserve C sequencing rules and evaluate each
expression the same permitted number of times. It may not move work out of a
short-circuited branch, duplicate a volatile access, or reuse a stale result.
Respect the selected compiler's semantics for GNU extensions.

### Owner liveness extends beyond tagged-value liveness

Consider:

```c
char *bytes = js_getstr(js, value, &length);
ant_value_t output = js_mkstr(js, NULL, length + extra);
/* bytes is used after the allocation */
```

The source-level last use of `value` precedes the allocation, but `bytes`
still requires the string allocation. The same issue occurs with object
fields, array storage, upvalue cells, rope nodes, and cage-derived offsets.

Required handling:

- Describe borrow-producing runtime APIs and their owning values or allocations.
- Preserve the owner across calls while the derived reference is live.
- Propagate ownership through supported assignments, pointer arithmetic,
  aliases, local aggregates, and returns.
- Separate "allocation survives GC" from "buffer address remains valid."
  A callback that reallocates array backing storage can invalidate a pointer
  even when its owning object is rooted.
- Require re-fetching or an appropriate borrow contract across such mutation.
- Reject unsupported pointer concealment or an escaping untracked borrow in
  migrated code with a diagnostic naming the operation and required contract.

Keeping every named owner rooted for its C storage lifetime is a useful first
policy, but does not solve an unnamed owner's pointer escaping the full
expression, nor an owner whose source storage ends while a borrow persists.
The compiler must retain an independent owner root when necessary.

### Aggregate and dynamic storage policy

Automatically cover fixed local arrays and structs with known root-bearing
fields. Track initialized elements rather than scanning unused capacity.
Struct assignment, copies through supported memory functions, designated
initializers, and output parameters need explicit tests.

Unions require an active-member/trace contract or a conservative representation
that is safe to inspect. Blindly treating every union field as a valid managed
pointer is unacceptable. Packed fields and unusual alignment need valid access
operations rather than undefined pointer casts.

Dynamic native storage needs a traced container abstraction with explicit
base, initialized length, and ownership. Registering a pointer to a malloc
buffer does not register its elements. On growth or realloc, publish the new
base and bounds before collection can occur, and stop visiting the old range.

Existing temporary-root handles are candidates for this role. Their eventual
replacement should be driven by ownership and storage needs, not by a desire
to make the macro search return zero.

## Execution and Ownership Boundaries

### Isolates, threads, and re-entry

Resolve the generated-frame context without assuming every function has a
parameter named `js`. Some code obtains the isolate through another object,
and some helpers operate on values without a direct isolate argument.

Compare an isolate-owned chain with a thread-local execution-context chain.
Specify how root ownership is selected, how inactive isolates are excluded,
and how nested entry into another isolate restores the prior context.
Do not introduce a process-global mutable chain as an implicit single-thread
assumption. A native build with threads and a Wasm instance must both have a
well-defined context model.

The prototype must cover:

- nested native calls within one isolate;
- C callbacks that execute JavaScript and return into the same C frame;
- host callbacks entering the runtime from an external library;
- worker execution and isolate teardown;
- nested isolate entry where supported by the embedding contract;
- collection attempted without an established runtime context.

An unresolved context must fail before unsafe execution. Reaching for an
arbitrary global isolate is not a fallback.

### Interpreter, JIT, and suspended activations

Generated C frames protect C execution. The interpreter's value stack and
saved activation frames retain their existing GC responsibilities.

MIR-generated code is outside the C compiler's input. Keep its current root
and conservative-scanning requirements explicit. Audit the JIT-to-C helper
boundary for live JIT values, arguments, results, and bailout state.

Do not disable conservative scanning globally in a mixed JIT runtime merely
because one C module has migrated. JIT stack maps or broader MIR root emission
are separate follow-ups unless the boundary audit finds a prerequisite defect.
Any such prerequisite must be recorded and resolved before claiming the
affected mixed-mode coverage.

A suspended JavaScript activation must not retain addresses into a returned
C root frame. If a native activation itself can suspend, either its storage
must remain valid and discoverable or its state must move into an explicitly
traced representation.

### Persistent ownership and embedding

Classify all old root-stack users before deleting an API:

| Lifetime | Intended mechanism |
| --- | --- |
| C local or expression temporary | Generated frame |
| Fixed local aggregate | Generated field/range descriptors |
| Dynamic temporary value collection | Traced container with scoped ownership |
| Native state retained after return | Persistent handle or owner trace callback |
| VM/JIT execution state | Existing VM/JIT root contract |
| External N-API handle scope | Explicit embedding lifetime contract |

Inspector object handles, async callbacks, module state, N-API references,
and worker messages cannot inherit the lifetime of the C function that created
them. Conversion must preserve their existing externally observable lifetime.

Preserve the external C ABI. External addons and third-party C libraries do
not automatically pass through Ant's generator. Root their boundary arguments
and retained state through explicit APIs; document borrowed return pointers
and callback contracts.

## Build Integration Requirements

### Deterministic generation

Generated output must be a declared build product, not an in-place edit of
tracked source. Suggested implementation-owned locations are `tools/gc-roots/`
for the generator and `build/.../gc-roots/` for generated artifacts; finalize
names after the prototype. The Wasm build must use its own build directory.

The output identity must include:

- original source and transitive header contents;
- target triple, data layout, sysroot, and relevant include paths;
- preprocessor defines, language mode, and relevant compiler options;
- generator version, root-frame ABI version, and effect/type summaries;
- source selection and rollout policy.

Use dependency files and a correct build graph. A clean build cannot require
an already-generated `compile_commands.json` from that same build to discover
the command needed to generate its inputs. Reuse a compilation database for
developer tooling where useful; derive production commands from Meson.

Do not feed GCC-only command-line flags unmodified into a Clang host tool.
Translation of flags must preserve target semantics and must be tested against
the actual target headers and predefined macros.

### Compatibility and diagnostics

- Preserve required GNU C extensions, including computed goto in the VM.
- Define how inline functions in headers are transformed without conflicting
  with non-instrumented or third-party includers.
- Avoid changing exported layouts, calling conventions, symbol names, or the
  meaning of source-location builtins.
- Preserve source locations for compiler diagnostics, sanitizers, debuggers,
  profiling, and coverage. Generated names should be stable and collision-free.
- Account for source strings such as `__FILE__`, `__LINE__`, and
  `__func__`; a new generated path must not silently alter relevant behavior.
- Keep generated C and an explanation of each root available for inspection.
- Integrate with ccache, PGO/LTO, unity/non-unity configurations if supported,
  IWYU, and the existing C lint workflow.
- Distinguish original-source compilation data used by editors from generated
  compilation data used to inspect the actual binary.
- If the generator is absent, incompatible, or fails, migrated sources must
  not compile through an uninstrumented fallback.

### Compatibility evidence matrix

Refresh this matrix against current build documentation before implementation.

| Target/workflow | Required evidence |
| --- | --- |
| macOS arm64 and x64 | Native correctness, supported Apple Clang/LLVM workflow, optimized output |
| Linux arm64 and x64, glibc | GCC and Clang source/build compatibility according to declared support |
| Linux musl release builds | Cross/sysroot correctness and static-link workflow |
| Windows x64, MSYS2 MinGW | GNU C mode, target headers, calling convention, generator distribution |
| wasm32 reactor via wasi-sdk | Roots in addressable linear memory, stack limit, imports/exports, traps and reuse contract |
| Nix and CI builds | Reproducible host tool and dependency closure |

The first proof can use one native host and Wasm. Those results do not imply
that the remaining rows pass. Every required row must have evidence or a
resolved scope decision before default enablement.

## Task List and Milestone Gates

All implementation tasks below are pending.

### P0. Census and contract inventory

- [ ] Pin source, toolchain, target configurations, and existing binaries used
  for later comparisons.
- [ ] Produce a per-file/per-function inventory of macro uses, direct root
  operations, temporary handles, persistent roots, and root-bearing containers.
- [ ] Inventory collection entrypoints and call paths, including header
  inlines, indirect calls, event pumping, and foreign callbacks.
- [ ] Inventory managed pointer types and borrow-producing APIs.
- [ ] Inventory source constructs that complicate lowering: GNU statement
  expressions, computed goto, unions, VLAs, compound literals, macros,
  varargs, inline assembly, memory copies, and functions without direct `js`.
- [ ] Record native/Wasm differences and C/JIT/embedding ownership boundaries.
- [ ] Select the initial fixture set and performance workloads before editing
  the runtime.

Deliverables: linked inventory, type/effect contract draft, baseline identities,
and a list of unsupported constructs requiring decisions.

**Gate:** every old root mechanism has a lifetime category. The inventory
includes real C-only temporaries, not just globally reachable JavaScript values.

### P1. Prove the transformation mechanism

- [ ] Implement bounded experiments for the candidate frontend mechanisms.
- [ ] Generate protection for scalar locals, parameters, reassignment, early
  return, loop scopes, and nested expression results.
- [ ] Demonstrate one owner-preserving string borrow and one local aggregate.
- [ ] Compile emitted output or integrated lowering on one optimized native
  target and Wasm.
- [ ] Check GCC/MinGW feasibility early using real target flags and headers.
- [ ] Produce source-to-generated-output examples and inspect emitted code.
- [ ] Demonstrate a hard diagnostic for a root-relevant unsupported construct.
- [ ] Measure prototype build overhead and stack storage without claiming
  production performance.
- [ ] Record the chosen mechanism, tool version policy, rejected alternative,
  and unresolved compiler-support rows.

Deliverables: small prototype, reproducible fixture commands, diagnostic
examples, and a decision-log entry.

**Gate:** no text-only rewriting; no use of erased integer types as the root
classification oracle; no unannounced compiler-support reduction. Do not begin
bulk source migration before this gate.

### P2. Implement and verify the runtime frame contract

- [ ] Add the internal descriptor, activation, and visitor contract.
- [ ] Establish isolate/thread context selection and nested-entry behavior.
- [ ] Guarantee allocation-free fixed-frame setup and teardown.
- [ ] Define initialization state for branch-dependent and partial values.
- [ ] Cover normal exits, return handoff, output parameters, and recursive
  activation independence.
- [ ] Keep generated frames separate from legacy count-based roots.
- [ ] Add debug checks for chain integrity, descriptor versions, live storage,
  and context mismatch.
- [ ] Define abnormal-exit handling at native and Wasm boundaries.
- [ ] Add focused runtime frame tests before migrating a production builtin.

Deliverables: reviewed ABI contract, focused frame tests, and visitor integration.

**Gate:** old and generated roots coexist; legacy restores cannot remove
generated roots; no registration OOM path; no traversal of dead C storage.

### P3. Make semantic coverage complete for the first supported subset

- [ ] Preserve type identity through aliases, qualifiers, signatures, and
  nested aggregate fields.
- [ ] Generate roots for unnamed results and partially evaluated expressions.
- [ ] Preserve evaluation count, sequencing, and short-circuit behavior.
- [ ] Implement owner retention for supported raw/interior pointers and
  borrowed offsets.
- [ ] Cover arrays, structs, out parameters, supported copies, and initialized
  ranges.
- [ ] Define checked behavior for unions, VLAs, varargs, and inline assembly.
- [ ] Classify unknown call effects conservatively.
- [ ] Implement diagnostics that identify the source value, collection edge,
  and unsupported escape or representation.
- [ ] Prove that supported borrowed pointers remain valid or are re-fetched
  across operations that can move backing storage.

Deliverables: a checked supported-language contract and positive/negative
compiler fixtures.

**Gate:** unsupported semantics fail compilation in migrated code. Listing a
construct in a comment is not a substitute for detecting it.

### P4. Establish an independent correctness oracle

- [ ] Add test-only forced collection at valid GC-capable boundaries.
- [ ] Run fixtures whose only live references are generated roots.
- [ ] Prevent conservative stack/register scanning from rescuing those
  references in an isolated fixture mode.
- [ ] Compile genuinely unannotated source; do not retain old address-taking
  or dummy pin calls.
- [ ] Include missing-root negative controls and verify they are detected.
- [ ] Add temporal checks that collection remains possible during callbacks
  and that root entries leave loops and completed activations.
- [ ] Exercise minor and major collections, heap reuse, and OOM boundaries.
- [ ] Run the meaningful subset at full optimization and under sanitizers,
  including an optimized sanitizer configuration where supported.

Deliverables: automated oracle, controlled negative tests, and a recorded
native/Wasm result matrix.

**Gate:** a missing root is observable as a test/verifier failure. Success
caused by conservative scanning, global references, or disabled GC does not pass.

### P5. Migrate a deliberately varied production slice

Use separate small changes with clear dependency order:

- [ ] A simple object-building builtin with multiple early returns.
- [ ] RegExp source/flags conversion with allocating user getters.
- [ ] A repeated per-item native conversion loop such as RPC array conversion
  on native builds, with an equivalent shared-core fixture for Wasm.
- [ ] A JSON path with dynamic algorithm state and re-entrant user callbacks.
- [ ] One C helper called from the interpreter and MIR JIT.
- [ ] A Wasm bridge conversion path with nested structures and errors.

For each slice, record which roots disappear, which persistent/container roots
remain, the generated output, and the independent correctness evidence.
Do not rewrite algorithm behavior or change the collector schedule merely to
make migration pass.

**Gate:** the slice works under optimized native, mixed JIT/native where
applicable, and Wasm execution. Generated-root coverage and retained manual
ownership are explicitly distinguished.

### P6. Integrate production builds and measure cost

- [ ] Add the generator and its dependencies to both Meson build graphs.
- [ ] Make clean builds, incremental builds, header invalidation, and cache
  reuse deterministic.
- [ ] Validate the compatibility matrix and resolve compiler policy.
- [ ] Measure startup, steady-state runtime, collection cost, live retention,
  stack use, binary size, Wasm size, and clean/incremental build time.
- [ ] Test recursion and realistic Wasm memory/stack limits.
- [ ] Record diagnostic, debugging, profiling, and editor workflows.
- [ ] Compare a correctness-first implementation before attempting root
  elision, slot reuse, or more aggressive lifetime analysis.

**Gate:** accepted costs are supported by pinned comparisons; supported builds
cannot accidentally omit instrumentation.

### P7. Migrate the remaining source inventory

- [ ] Move through modules and helpers in bounded groups with focused tests.
- [ ] Convert parameter/field pins according to ownership, not spelling.
- [ ] Consolidate dynamic temporary containers where the contract requires it.
- [ ] Resolve persistent inspector/N-API/native-state lifetimes independently.
- [ ] Keep a machine-generated coverage report for every relevant build
  configuration, including conditional code and inline headers.
- [ ] Re-run focused and broad validation required for each migrated group.
- [ ] Enable automatic generation by default only after P6's compatibility
  and cost gates.
- [ ] Forbid new handwritten local-root operations in fully migrated surfaces.

**Gate:** every inventoried use is migrated, retained with a specific lifetime
reason, or moved to a separately scoped blocker. No unexplained allowlist.

### P8. Remove rollout scaffolding and finalize

- [ ] Remove the obsolete local save/pin/restore API after its remaining uses
  have been classified and resolved.
- [ ] Remove temporary dual execution or fallback modes that no longer serve
  a tested purpose.
- [ ] Keep a bounded adversarial regression suite and coverage enforcement.
- [ ] Document the supported C subset, borrow/container contracts, generated
  output inspection, and toolchain maintenance workflow.
- [ ] Update architecture, build, testing, and repository knowledge documents.
- [ ] Record final validation and performance evidence.
- [ ] Move this plan to `completed/` and update both plan indexes.

**Gate:** ordinary C-local rooting is automatic in the declared scope, and
remaining ownership APIs are intentional. Experimental availability alone is
not completion.

## Validation Design

### Proving root completeness

Use several independent techniques; none alone proves the whole compiler:

1. Small C fixtures with exact expected liveness and controlled collection.
2. Generated-output checks that validate critical ordering and slot activation,
   without snapshotting incidental formatting.
3. Runtime frame/descriptor assertions and negative controls.
4. Optimized execution with conservative rescue disabled for the isolated,
   fully covered fixture runtime.
5. JS semantic regressions exercising real builtin re-entry.
6. Bounded differential/fuzz generation of supported C expression and
   control-flow shapes, compared with an explicitly rooted reference.

The isolated runtime can keep known VM/heap roots while omitting conservative
native discovery. It must not accidentally disable a JIT or external root
source needed by unrelated code. If a whole-runtime mode cannot provide this
isolation safely, create a smaller fixture executable rather than treating
mixed-mode failures as evidence about the generator.

Forced GC must run only where the runtime can legally collect. Injecting a
collection in the middle of an unpublished, partially initialized heap object
would test a different contract and can create false failures.

Positive fixtures must remove all unintended strong references. Clearing a
JavaScript property is insufficient if the test still keeps the same value
in an argument array, global, constant table, or a legacy root.

Comparing generated roots with the legacy list is useful for diagnosis but
not an oracle: the legacy list can be incomplete, and making variables
addressable can itself conceal the compiler-liveness problem.

### Required fixture matrix

| Family | Minimum cases |
| --- | --- |
| Values | Heap objects, arrays, strings, ropes/builders, closures, upvalues, promises/generators, BigInts, collectible symbols where applicable, and heap-backed errors |
| Scalar lifetime | Parameters, initialized and delayed-initialization locals, reassignment, aliases, const values, and output parameters |
| Expressions | Nested allocating arguments, ternary branches, short circuit, comma expressions, initializer calls, and returned temporaries |
| Control flow | Early return, nested scopes, switch labels, forward/backward goto, break/continue, loop-local declarations, and recursion |
| Aggregates | Fixed arrays, nested structs, partial initialization, supported unions, compound literals, memory copies, and dynamic initialized ranges |
| Borrowing | Interior string bytes, object fields, array backing storage, cage offsets, owner overwrite, returned borrows, and pointer escape diagnostics |
| Re-entry | Getters, setters, proxy traps, coercions, JSON replacers/revivers, callbacks, and nested event-loop pumping |
| Modes | Interpreter, C helper called from MIR, nested VM entry, native embedding, and Wasm |
| Failures | Allocation failure, root-container growth failure, typed error returns, abnormal-exit boundary recovery, and instance teardown |
| Optimization | Debug, release optimization, LTO, fresh PGO where used, inlined and non-inlined helpers |
| Lifetime release | Repeated loop iterations, completed calls, root-slot reuse, and return-to-baseline root counts |
| Negative compiler cases | Hidden pointer representation, unsupported escape, missing execution context, unsound no-collect claim, and unsafe partially initialized range |

Retention assertions should use internal test observations where possible.
WeakRef and finalizer timing are not deterministic correctness oracles, and
production conservative scanning can legitimately retain an object longer.
Do not introduce language-visible deterministic finalization requirements.

### Existing coverage to retain and extend

Relevant starting points include:

- [JIT string concat roots](../../../tests/test_jit_string_concat_gc_roots.cjs)
- [Array sort roots](../../../tests/test_array_sort_gc_roots.cjs)
- [BigInt GC](../../../tests/test_bigint_gc.cjs)
- [Upvalue GC](../../../tests/test_upvalue_gc.cjs)
- [Async generator liveness](../../../tests/test_async_generator_gc_liveness.cjs)
- [Wasm tests](../../../packages/wasm/test/wasm.test.js)

These files are starting points, not evidence that the proposed integration
passes. In particular, the existing Wasm repeated-allocation test keeps values
in `globalThis.keep`; that does not establish C-local-only root coverage.

### Commands and test registration

Follow [the testing guide](../../repo/testing.md) at each implementation phase.
Register new compiler/C fixtures in a discoverable build/test target rather
than relying on undocumented ad hoc compiler commands.

Existing commands for later implementation validation include:

```sh
maid preflight
meson compile -C build
./build/ant tests/test_jit_string_concat_gc_roots.cjs
./build/ant tests/test_array_sort_gc_roots.cjs
./build/ant tests/test_bigint_gc.cjs
./build/ant tests/test_upvalue_gc.cjs
./build/ant tests/test_async_generator_gc_liveness.cjs
./build/ant examples/spec/run.js --all
npm --prefix packages/wasm test
```

The Wasm package's `test` script builds first. Do not describe it as a
test-only command or run it in a session that disallows builds.
New fixture commands must be recorded after their targets exist.
This command list does not mean these commands were run for this document.

## Performance and Resource Gates

Fewer macros is a maintainability result, not a throughput measurement.
Generated frames may eliminate reallocating registration and repeated helper
calls, but can also increase address-taking, stack traffic, root traversal,
retention, and code size.

### Measure separately

| Dimension | Evidence |
| --- | --- |
| Root maintenance | Entries/exits, slots activated, instructions, and fixed/dynamic allocation behavior |
| Native runtime | Allocating builtin loops, callback-heavy operations, JSON, regex conversion, and a representative server workload |
| VM/JIT | Interpreter cost and MIR-to-C helper cost, without attributing unmodified JIT behavior to the C pass |
| Collection | Root scan time, total minor/major time, collection counts, and root high-water |
| Retention | Peak live bytes, peak RSS, post-collection live bytes, and objects retained by generated roots |
| Stack | Per-function frame growth, recursion headroom, and Wasm's configured stack budget |
| Distribution | Native text/data size, descriptor size, Wasm size, startup, and package/dependency footprint |
| Developer workflow | Clean build, one-file rebuild, header rebuild, generator cache hit/miss, diagnostics, and profiling fidelity |

### Comparison protocol

- Pin source hashes, binary hashes, generator version, compiler versions,
  flags, target, host, workload, and input.
- First compare base/candidate with matching no-PGO settings to isolate the
  source/build integration.
- For release claims, train independent matching PGO profiles for base and
  candidate. Do not reuse stale profile data after instrumentation changes.
- Run serial interleaved AB/BA comparisons on the same host and workload.
- Record variability and repeat only where results leave a material question.
- Keep collector scheduling and unrelated optimizations fixed for attribution.
- Compare both the initial correct lowering and any optimized lowering.
- Do not infer native speed from Wasm results or vice versa.

Before collecting acceptance data, record a numerical budget for throughput,
startup, memory, stack, binary size, and build time in the decision log.
There is no measured budget in this plan yet. A candidate that changes these
tradeoffs materially requires an explicit decision supported by the data,
not an assertion that cleaner source makes the cost acceptable.

### Optimization order

After correctness and a profile identify actual costs:

1. Omit frames from functions proven to have no roots live across collecting
   calls.
2. Avoid rooting known immediate-only values where that fact is stable.
3. Reuse non-overlapping root slots while respecting aliases and borrows.
4. Shorten owner retention only with derived-reference liveness accounted for.
5. Reduce activation/clearing stores when the proof and generated code agree.
6. Consider native stack maps if measured frame traffic justifies a separate
   backend project.

Do not combine these optimizations with the first correctness proof.

## Rollout, Rollback, and Coverage

Use an explicit migration manifest for the prototype and phased rollout.
The manifest should identify source/configuration coverage and the reason
for each retained manual boundary. Its format is to be selected in P1.

During rollout:

- Both legacy and generated roots may be visited.
- A migrated source must fail its build if generation is unavailable.
- An unmigrated source retains its established manual-root behavior.
- Do not infer migration status solely from the absence of a macro.
- Coverage must account for conditional platform code and inline headers.
- A source change introducing a new unsupported construct must fail the
  migrated build, even if the previously generated artifact still exists.
- Production fallback cannot mean compiling newly unannotated source without
  generation.

Rollback means restoring the matching source and build configuration for a
migration unit. Merely disabling the generator after removing manual roots
is unsafe. Keep changes small enough that each unit can be reverted together
with its instrumentation selection.

Long term, replace a broad migration allowlist with a clear rule for all
Ant-owned GC-relevant C sources and a small, justified boundary list.
Every retained exception needs an owner, reason, and checked contract.

## Risk Register

| Risk | Consequence | Required mitigation/evidence |
| --- | --- | --- |
| Type identity disappears | GC values omitted or ordinary integers misclassified | Frontend classification with positive/negative fixtures |
| Expression result omitted | Collection between argument evaluations loses a value | Unnamed-result fixtures with forced GC |
| Borrow owner dropped | Interior pointer accesses reclaimed memory | Owner propagation, escape checks, mutation-aware borrow rules |
| Uninitialized slot visited | Invalid tagged value or raw pointer reaches marker | Explicit activation state and partial-initialization fixtures |
| Optimizer removes needed state | Debug passes while release fails | Observable frame contract, optimized/LTO tests, emitted-code inspection |
| Old roots rescue new omissions | Migration appears correct by accident | Isolated generated-only oracle and negative controls |
| Context selected incorrectly | Cross-isolate traversal or stale frame chain | Nested entry/worker fixtures and context assertions |
| JIT coverage overstated | Live MIR registers omitted during GC | Preserve JIT contract and test mixed boundaries |
| Compiler policy narrows silently | MinGW/GCC or Apple workflows fail | P1 feasibility gate and P6 compatibility matrix |
| Source-to-source lowering changes C behavior | Evaluation, ABI, or diagnostic regressions | Semantic frontend, extension inventory, source mapping tests |
| Native container mistaken for local root | Values in malloc storage are reclaimed | Explicit traced-container contract |
| Excess retention or stack growth | OOM, recursion regressions, larger Wasm footprint | Resource measurement and live-range/slot work after correctness |
| Trap/nonlocal exit leaves stale frames | Later GC visits invalid stack memory | Boundary reset or enforced non-reusability policy |
| Stale generated output | Newly introduced root hazard ships | Complete dependency/cache identity and clean-build tests |
| Missing tool silently bypassed | Unprotected migrated code executes | Mandatory generation and hard build failure |

## Open Decisions and Dependencies

| ID | Decision | Needed by | Current position |
| --- | --- | --- | --- |
| D1 | Semantic source emitter or compiler-integrated lowering? | P1 | Compare bounded prototypes; no selected production mechanism |
| D2 | How are GCC/MinGW and Apple Clang supported? | P1/P6 | Existing documented support is a constraint, not an implicit casualty |
| D3 | Slot addresses or authoritative generated value storage? | P2 | Prefer one source of truth; measure and verify optimizer effects |
| D4 | Frame chain owned by isolate or execution context? | P2 | Must cover helpers without direct js and nested isolate entry |
| D5 | Borrow metadata and escape-analysis representation? | P3 | Central API/type contracts; no routine per-local annotations |
| D6 | Supported union/VLA/varargs/assembly subset? | P3 | Audit real uses and reject unsupported root-relevant cases |
| D7 | Abnormal-exit recovery and Wasm instance reuse? | P2/P4 | Cannot rely on C cleanup after trap or nonlocal unwind |
| D8 | Independent oracle for generated roots? | P4 | Prefer a small fully covered fixture runtime before broader modes |
| D9 | Resource budgets for default enablement? | P6 | Set from baseline and workload importance before acceptance runs |
| D10 | Remaining persistent/direct root-stack users? | P7 | Classify before API deletion; preserve embedding lifetimes |
| D11 | How are external source consumers built? | P6 | Supply mandatory generation or a deliberate supported boundary |

Related work that must remain consistent:

- [Wasm embedding](../completed/wasm-embedding-package.md): shared runtime build, memory
  limits, trap behavior, and explicit import/export surface.
- [JIT inline allocation](silver-jit-inline-arena-allocation.md): collection
  ordering and live values at helper transitions.
- [Array backing store GC pacing](array-backing-store-gc-pacing.md): allocation
  pressure and backing-store lifetime; do not change pacing to hide root bugs.
- [Elysia throughput](elysia-200k-rps.md): representative performance gates and
  independent profile-matched comparisons.
- [Architecture](../../../ARCHITECTURE.md),
  [build support](../../../BUILDING.md), and
  [testing policy](../../repo/testing.md).

If another plan changes these boundaries, refresh this plan's inventory and
contracts before continuing the affected phase.

## Decision Log

| Date | Decision | Reason/status |
| --- | --- | --- |
| 2026-09-06 | Select automatic root generation as the architectural target | User preferred ordinary declarations over manual local-root management |
| 2026-09-06 | Start with generated root frames | Common addressable representation for native/Wasm; retains existing heap traversal |
| 2026-09-06 | Keep transformation mechanism open until P1 | Compiler support and semantic completeness require evidence |
| 2026-09-06 | Preserve persistent/container ownership contracts | Local C lifetime cannot represent async or externally retained state |
| 2026-09-06 | Require a generated-root oracle independent of conservative rescue | Existing scanning and address-taking can conceal omissions |
| 2026-09-06 | Keep this change documentation-only | Implementation and validation phases remain pending |

Append decisions with evidence and rejected alternatives as work proceeds.
Update the main design when a decision changes it; do not make future readers
reconstruct the current contract from contradictory historical entries.

## Validation Status

| Item | Status |
| --- | --- |
| Current source and build-policy inspection | Completed for initial plan at the pinned revision |
| Compiler/GC primary-source reference check | Completed for initial design framing |
| Documentation links, whitespace, and repo harness | Pending the documentation-change checks |
| Runtime frame implementation | Not started |
| Compiler prototype and mechanism selection | Not started |
| Generated-only correctness oracle | Not started |
| Native/Wasm optimized correctness | Not run |
| GCC/MinGW/Apple compatibility proof | Not run |
| Runtime, retention, stack, and build-cost measurements | Not run |
| Production source migration | Not started |

## Follow-Ups After Completion

Consider these only when the completed implementation and measurements justify
them:

- Native stack maps to reduce measured frame traffic.
- More precise lifetime analysis for expensive retention cases.
- A separate audit of whether conservative scanning can be reduced for
  covered execution modes, including explicit MIR support.
- Generated tracing for selected native containers or structs.
- Moving-GC support, which would require a separate slot-update and pointer
  relocation contract; the current value-visiting root API does not provide it.

## Primary References

These explain mechanisms and limitations; they do not establish that Ant's
proposed integration is implemented or correct.

- [LLVM: Garbage Collection](https://llvm.org/docs/GarbageCollection.html):
  frontend responsibilities, expression intermediates, shadow stacks, and
  safepoint interfaces. Ant still needs its own type and runtime integration.
- [Clang: LibTooling](https://clang.llvm.org/docs/LibTooling.html):
  standalone semantic tooling as one candidate host-tool foundation.
- [Clang: Plugins](https://clang.llvm.org/docs/ClangPlugins.html):
  compiler-integrated frontend actions as another candidate, subject to
  toolchain distribution and version compatibility.
- [Clang: cleanup attribute](https://clang.llvm.org/docs/AttributeReference.html#cleanup):
  possible scoped-cleanup lowering detail, not an automatic rooting system.
- [WebKit: Understanding GC in JSC](https://webkit.org/blog/12967/understanding-gc-in-jsc-from-scratch/):
  conservative stack/register discovery as an alternative architecture.
- [WebAssembly: Runtime Structure](https://webassembly.github.io/spec/core/exec/runtime.html#call-frames):
  locals in execution frames are distinct from linear-memory storage.
