# Silver Speculative MIR Fast Tier

Status: active
Last reviewed: 2026-08-28
Owner: theMackabu

## Goal

Move profiled, stable Silver operations out of generic C helpers and into guarded
MIR without changing their JavaScript semantics. The first landing gate covers
dense numeric `GET_ELEM` and `PUT_ELEM` plus numeric `ToInt32`/`ToUint32`
bitwise operations. Once that gate passes, measure the named follow-up surfaces
and retain only independently safe improvements.

The requested throughput target is a `bench-v8` geometric mean of at least
8000 on an exact PGO candidate, with no material subtest regression. The
original retained candidate reaches 4993.80. A later numeric-index setter
follow-up reaches 5937.89 under the same-profile control described below. This
plan therefore remains active even though the first fast tier and several safe
follow-ups are implemented.

## Constraints

- The C helpers remain the complete semantic implementation for cold,
  mismatched, exceptional, and unsupported cases.
- Generated code may directly load or store only after proving every invariant
  on which the corresponding helper depends.
- Guard failure must either reconstruct the pre-bytecode stack and deopt or
  invoke the complete helper locally. It must not continue after a partial
  side effect.
- `NaN`, infinities, fractions, `-0`, holes, prototypes, accessors, frozen
  objects, `BigInt`, and coercion side effects must retain JavaScript behavior.
- Performance evidence uses pinned binaries from one source identity and one
  PGO profile, run serially in interleaved AB/BA order.
- Do not retain an experiment merely because one benchmark improves. Reject
  any path with an unexplained regression or an unproven slow edge.

## Design Reference

The reference snapshot is WebKit commit
`d3e6e0f710a4f913cc4281834f9810ab07962797`. The commit itself is an unrelated
Wasm change; the cited JavaScriptCore files at that exact snapshot are the
design reference:

- `Source/JavaScriptCore/dfg/DFGFixupPhase.cpp:1247-1278` widens an absent
  `GetByVal` prediction, converts a proven full-number index to checked
  `Int32` for a specific in-bounds array mode, and refines the mode from the
  observed operands.
- `Source/JavaScriptCore/dfg/DFGFixupPhase.cpp:1490-1604` performs the
  corresponding `PutByVal` refinement and fixes `Array::Int32` inputs to a
  known cell, an `Int32` index, and an `Int32` value while leaving generic and
  slow-put modes available.
- `Source/JavaScriptCore/dfg/DFGSpeculativeJIT64.cpp:2807-2825` starts the
  in-bounds `Int32`/contiguous get path and emits its public-length bounds
  speculation check.
- `Source/JavaScriptCore/dfg/DFGSpeculativeJIT64.cpp:7768-7817` keeps a data-IC
  fast/slow pair for generic puts, but sends `Array::Int32` and contiguous puts
  to the specialized compiler.
- `Source/JavaScriptCore/dfg/DFGSpeculativeJIT.cpp:4150-4184` consumes exact
  `Int32` operands and emits the bitwise machine operation directly, including
  constant-operand forms.

Ant does not reproduce DFG's graph or array-mode machinery. It adopts the
same essential boundary: observed site evidence selects a specialization;
generated guards protect a direct machine operation; and a complete semantic
path remains reachable before any unsafe mutation.

## Retained Implementation

### Site feedback

The low four bits of each bytecode feedback byte retain the existing value
classes. The high four bits now hold a three-bit saturating specialization
sample count and a permanent mismatch bit. A site becomes eligible after seven
matching observations and no mismatch. This deliberately prefers missed
optimization to polymorphic mis-specialization.

The interpreter records:

- a dense, existing, numeric array element for `GET_ELEM`;
- the same element plus a numeric value and writable array for `PUT_ELEM`;
- exact integral Number operands in the `Int32` through `Uint32` range for
  bitwise operations;
- Number/Number operands for equality.

Each feedback change increments `tfb_version`, so an already compiled function
can be reconsidered with the new evidence.

### Dense numeric elements

The generated get path proves:

1. an exact array index in `[0, UINT32_MAX - 1]`;
2. an Array receiver with the fast-array bit set and the exotic bit clear;
3. a non-null dense buffer;
4. `index < len` and `index < cap`;
5. a Number value in the selected slot.

It then loads the value directly into the JIT's numeric representation.

The put path additionally proves a Number value and a non-frozen receiver. It
only overwrites an existing numeric dense slot: it does not grow length, fill a
hole, convert storage, or bypass an accessor. On guard failure, the main JIT
reconstructs the original operand stack and resumes at `PUT_ELEM`.

The baseline dense array setter now checks the existing frozen flag. Strict
assignment throws and sloppy assignment leaves the element unchanged. This is
required for the deoptimized path to preserve the write guard's semantics.

### Numeric-index setter fallback

The complete `PUT_ELEM` helper now recognizes Number keys that are exact array
indices before materializing their decimal property strings. It calls a C
indexed setter which mirrors the existing dense-array setter for an in-bounds
write or a low-density array growth. Mapped arguments are updated first.
Proxies, non-array receivers, sparse or complex array cases, and non-index
Numbers still materialize the canonical property string and use `js_setprop`.

This remains a semantic fallback, not a wider direct MIR store. The generated
`PUT_ELEM` path still refuses to grow an array, fill a hole, or cross an
accessor boundary. Consequently the change removes allocation and property-key
lookup work from the already-required slow operation without weakening its JIT
guards.

### Numeric word32 operations

The generated path rejects non-Numbers, `NaN`, infinities, fractions, and
values outside `[INT32_MIN, UINT32_MAX]`. It performs `&`, `|`, `^`, `<<`,
`>>`, `>>>`, and `~` with 32-bit MIR instructions, masks shift counts with
`0x1f`, sign-extends signed results, zero-extends unsigned right shift, and
returns the result in the numeric representation.

The narrowed input range is conservative: other JavaScript Numbers still take
the original coercing helper. `BigInt`, objects with conversion hooks, and
mixed Number/BigInt cases therefore remain on the semantic path.

### Safe follow-up extensions

The first gate passed, so the following measured extensions are retained:

- `OP_REGEXP` is JIT-eligible and calls the factored literal-construction
  helper, preserving fresh object identity and independent `lastIndex` state.
- Number-only equality sites use direct floating-point equality. A later type
  mismatch calls the original equality helper locally, with numerically
  tracked inline operands reboxed before that call. Strict equality also
  handles identical tagged values and unequal non-string/non-BigInt tagged
  values directly; strings, BigInts, and all Numbers retain their semantic
  helper path when raw identity is insufficient.
- Inlined `GET_FIELD_OPT` and `GET_FIELD2` reuse the existing epoch/shape/slot
  field-IC fast path, with the original helper as the miss path.
- Repeated own-property additions may reuse an exact retained
  `from_shape -> to_shape` transition when the receiver is extensible,
  non-exotic, non-array, unsealed, unfrozen, and the new default-attribute slot
  is the next in-object slot. Every value guard runs before the non-failing
  shape transition. Reference stores stay direct only for young receivers;
  old receivers use the complete helper, except for the existing guarded rope
  remembered-set path.

## Rejected Experiments

### Stable field deoptimization

Deoptimizing a function when a profiled field guard missed did not improve the
field-heavy tests enough to justify the churn. The corrected experiment made
Raytrace 0.918x baseline without producing a Richards gain. Its feedback,
opcode flags, and deopt paths were removed. The existing IC-with-helper-miss
design remains.

### Direct `Array.prototype.push`

A one-argument dense push fast path initially improved Splay by about 3.7% and
Richards by about 0.8%. Semantic testing then showed that an inherited indexed
setter can intercept the new index. Ant's current generic array-set path also
bypasses that setter, so falling back to it cannot prove correct push
semantics. The entire push experiment, its flags, protector, feedback, and
tests were removed. A future push tier first needs a correct indexed Set path
or an equivalent proven prototype-chain watchpoint and slow operation.

### Simple young closure allocation

Temporary candidate-only census instrumentation, removed before the retained
build, found the following `jit_helper_closure` distribution:

| Test | Total | Zero upvalues | Inherited only | Local | Mixed | `<= 4` upvalues | Eval env |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Earley-Boyer | 2,727,981 | 1,308 | 3,002 | 1,308 | 2,722,363 | 10,682 | 0 |
| Navier-Stokes | 1,834 | 0 | 524 | 0 | 1,310 | 1,834 | 0 |

The other six `bench-v8` tests made no calls to this helper. A simple
zero-upvalue arena bump would cover less than 0.05% of Earley-Boyer's dynamic
closures. The dominant mixed-capture form needs the all-or-nothing
closure-plus-upvalue transaction specified in
[Silver JIT Inline Arena Allocation](silver-jit-inline-arena-allocation.md).
One helper call per captured upvalue would violate that plan's boundary and
was not implemented.

### Larger nursery threshold

Doubling `GC_NURSERY_THRESHOLD` from 32 KiB to 64 KiB improved Earley-Boyer by
about 4.2% and Raytrace by about 1.2% in the isolated serial comparison, but
Earley-Boyer peak RSS rose from roughly 83 MiB to 110 MiB, about 32%. That
violates the 3% memory gate, so the threshold change was reverted.

### Second-entry own-property PIC

Reusing the field-add guard storage as a second own-property read cache was
semantically guarded, but the added generated checks regressed the hot
monomorphic case. The first serial round put Richards near 0.84x and DeltaBlue
near 0.69x, with no compensating suite-wide benefit. The C and MIR alternate
cache paths and their test scaffolding were removed.

## Performance Evidence

### Pinned identities

- Source: `301296d2b2421a0fc99da496b7682d05f1a62118`
- Version: `14.1.301296d2.0`, `arm64-apple-darwin`
- Host: Apple M4 Pro, arm64
- PGO profile SHA-256:
  `1d4e2903df54ca7f6404fbd10c25e3f04949f02cde8cecf47fec74e92648ae10`
- Base SHA-256:
  `8b0d015337fb901ce372471a47c133811f6c9cf2e565128c37ecd65ee77aceaa`
- Candidate SHA-256:
  `c0fae688f7b7821b1792463b232fe9c55f92aff99240e8e596199cde633f8d74`

The focused first gate used two serial interleaved AB/BA rounds. The numeric
tier alone produced 2.17x Crypto and 3.98x Navier-Stokes, so the conditional
follow-up phase was activated.

The final retained candidate used two full serial rounds, with per-test order
base/candidate in round one and candidate/base in round two:

| Test | Base GM | Candidate GM | Ratio |
| --- | ---: | ---: | ---: |
| Richards | 3210.71 | 3711.85 | 1.156x |
| DeltaBlue | 4919.17 | 5319.18 | 1.081x |
| Crypto | 1583.87 | 3444.14 | 2.175x |
| Raytrace | 3637.36 | 3681.55 | 1.012x |
| Earley-Boyer | 7187.10 | 7429.62 | 1.034x |
| RegExp | 2870.67 | 2983.87 | 1.039x |
| Splay | 4435.39 | 4925.85 | 1.111x |
| Navier-Stokes | 3569.33 | 14147.37 | 3.964x |
| **Full geometric mean** | **3623.53** | **4993.80** | **1.378x** |

All eight per-test geometric means improved, but the candidate remains 3006.20
points below the 8000 target. Reaching it requires another 1.602x over the
retained candidate. The named low-risk helper removals and threshold tuning do
not account for that gap.

### Numeric-index setter follow-up

The follow-up is an uncommitted working-tree candidate on base `bcab2d85`. A
fresh candidate profile was used to build both source variants so the control
does not compare incompatible effective PGO:

- PGO profile SHA-256:
  `563fb68f8646f995c9942e21d3f5340c28d76e093c839fd1d34c7c0222f56e98`
- Base SHA-256:
  `d72f26d3d14a02a6916b2136c533334a5fd52c28a58edd65d315dec37fd36095`
- Candidate SHA-256:
  `b5e8f05fbd40c3cfd5b2e1c5d80bfde1ae9c0afa714c1f8a24a4d4fc189e134d`

Four full serial AB/BA rounds produced these per-test geometric means:

| Test | Base GM | Candidate GM | Ratio |
| --- | ---: | ---: | ---: |
| Richards | 3845.08 | 3968.70 | 1.032x |
| DeltaBlue | 5414.97 | 5455.43 | 1.007x |
| Crypto | 3485.22 | 5604.47 | 1.608x |
| Raytrace | 3940.61 | 3946.06 | 1.001x |
| Earley-Boyer | 8305.80 | 8395.12 | 1.011x |
| RegExp | 3694.23 | 3700.53 | 1.002x |
| Splay | 5909.07 | 5857.86 | 0.991x |
| Navier-Stokes | 14647.71 | 17736.15 | 1.211x |
| **Full geometric mean** | **5433.41** | **5937.89** | **1.093x** |

Splay's sub-percent negative result did not reproduce in the isolated controls.
A 12-sample AB/BA run with the same PGO profile was 1.006x, the earlier
eight-sample fresh-PGO comparison was 1.012x, and the no-PGO comparison was
1.020x. It is therefore treated as layout or run variance, not a retained
source regression. The no-PGO full suite also kept the other tests within
0.2% or faster while improving Crypto by 1.676x.

## Validation Status

Completed on the original retained candidate:

- configured PGO build and final link;
- focused numeric element/bitwise semantics test;
- focused equality/field/regexp semantics test, including inline numeric
  reboxing on a late loose-equality mismatch;
- focused field-add transition, GC retention, negative-lookup invalidation,
  non-extensible, sealed, frozen, array, and `__proto__` coverage;
- emitted MIR inspection for direct numeric loads, stores, word32 ops, and the
  guarded shape-transition call;
- exact two-round full serial AB/BA benchmark;
- repository validation router and knowledge checks;
- full spec suite: 3972 passed, 0 failed across 101 files;
- final `maid preflight` repository knowledge and structure checks;
- source whitespace validation.

Completed on the numeric-index setter follow-up:

- fresh PGO training, final link, pinned candidate, and same-profile base build;
- full no-PGO and PGO serial benchmark controls plus the isolated 12-sample
  Splay control;
- focused speculative numeric-element test in both PGO and no-PGO builds,
  including dense and sparse growth, `-0`, fractional keys, proxies, frozen and
  non-extensible arrays, and mapped arguments;
- focused array, JIT element, arguments, and proxy regression tests;
- full spec suite: 3972 passed, 0 failed across 101 files;
- repository validation router, knowledge check, configured-tree reconfigure,
  final PGO link, and final `maid preflight`.

`tests/test_jit_typed_array_elem_fast.cjs` still reports the existing
`BigInt64Array` Number-assignment failure. The same failure reproduces on both
pinned base builds, including no-PGO, so it is not attributed to this change.

### Descriptor-driven Map template follow-up

The fixed `CALL_MAP_GET_TEMPLATE2` and `TAIL_MAP_GET_TEMPLATE2` opcodes were
replaced by `CALL_MAP_TEMPLATE` and `TAIL_MAP_TEMPLATE`. Each opcode references
a descriptor containing the operation (`get` or `has`), one to three
substitutions, and the constant template segments. The descriptors live in the
same code-arena allocation immediately before an eight-byte header and the
function bytecode; `func->code` still points at the first opcode. A spare
`sv_func_t` bit records whether that sidecar exists, so removing the descriptor
pointer and count reduced `sv_func_t` from 216 bytes back to its 200-byte
pre-feature size. Functions without Map-template descriptors allocate no
sidecar bytes. Templates with more than three substitutions continue through
the ordinary compiler path. Property lookup and method identity remain
observable: built-in numeric calls use the fast lookup, while overrides and
nonnumeric substitutions call the method with the fully materialized key.

The MIR ABI is fixed at three substitution value slots so arities one through
three stay register-only on AArch64. There are two generic helper entrypoints
(normal call and tail fast attempt) and one shared canonical numeric-pair fast
leaf. The JIT recognizes the canonical pair descriptor at compile time and
uses that leaf for both normal and tail calls; a normal-call guard miss branches
to the generic helper without replaying expression evaluation. The fallback
key builder performs one final string allocation instead of constructing an
intermediate concatenation.

The performance check used identical release, LTO, project-PGO-disabled builds
from base `7b51fd332c09499f2f19fa4ea93028ea9d07a008` and the working-tree
candidate. The checked-in PGO profile discarded counters for the changed
interpreter/compiler/JIT functions, so it was deliberately excluded from this
claim.

- Base SHA-256:
  `b20dbec0ca1e75c9d390103f42f2b58f0a2fa720c8cee9d5ad2ca4611b12f49c`
- Candidate SHA-256:
  `eda260b2471f920c8aab6b9bd9b3931db4bbabba95a4d4d7ba8181d97cafe8da`

Two serial AB/BA process rounds, each with 11 samples, produced:

| Map template shape | Base time GM (ms) | Candidate time GM (ms) | Throughput ratio |
| --- | ---: | ---: | ---: |
| Canonical short numeric pair | 219.397 | 217.487 | 1.009x |
| Canonical long numeric pair | 685.255 | 682.242 | 1.004x |
| Overridden `get` fallback | 181.093 | 160.944 | 1.125x |
| String-key built-in fallback | 193.013 | 153.948 | 1.254x |

The zero-growth sidecar was then compared directly with the pointer-and-count
candidate above using the same no-PGO build configuration and serial AB/BA
protocol. The pre-sidecar binary was
`eda260b2471f920c8aab6b9bd9b3931db4bbabba95a4d4d7ba8181d97cafe8da`;
the sidecar binary was
`ebc019a2ea823b2d7f94bfa0310b6b3081500138f6bc0bccdca912f3f55a690e`.

| Map template shape | Pre-sidecar time GM (ms) | Sidecar time GM (ms) | Throughput ratio |
| --- | ---: | ---: | ---: |
| Canonical short numeric pair | 219.347 | 218.283 | 1.005x |
| Canonical long numeric pair | 687.429 | 688.093 | 0.999x |
| Overridden `get` fallback | 168.701 | 165.326 | 1.020x |
| String-key built-in fallback | 155.632 | 152.770 | 1.019x |

The canonical rows include two complete serial AB/BA rounds (four processes
per binary); the fallback rows include one complete round (two processes per
binary). Every process took 11 samples. The canonical JIT path remained within
0.5% in both directions, while both fallback shapes improved. The descriptor
lookup happens at JIT compile time, not in the generated hot loop.

Validation completed for this follow-up:

- focused arity-one through arity-three `get` and `has` tests, arity-four
  fallback, override behavior, coercion ordering, optional-base guards, and
  tail recursion;
- emitted MIR inspection confirming generic arities and the canonical
  numeric-pair leaf;
- neighboring Map, Map GC, string-concatenation JIT, upsert, and collection
  constructor tests;
- full spec suite: 3972 passed, 0 failed across 101 files;
- configured-tree build and focused test. The build emitted the expected stale
  PGO counter-discard warnings, which is why the benchmark used no-PGO builds.

## Follow-ups Required For 8000

1. Correct generic indexed Set semantics before reconsidering direct array
   growth or `Array.prototype.push`.
2. Complete the mixed local/inherited capture transaction in the inline arena
   allocation plan, then measure Earley-Boyer with the same pinned protocol.
3. Design a field PIC that does not add checks to the monomorphic path; the
   measured second-entry shape check must not be revived as-is.
4. Treat 8000 as representation-level work, not a reason to stack speculative
   micro-fast-paths. The dominant Earley-Boyer closure form needs a
   transactional allocation design, while the RegExp and Raytrace profiles
   remain distributed across allocation, arrays, and property work.
