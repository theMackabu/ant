# Shared Shape Descriptors and JSON Construction

Status: active
Owner: theMackabu
Last reviewed: 2026-09-08

## Goal

Share property descriptors and their lookup index along stable shape chains,
avoiding a full property copy at every addition. Build JSON objects directly
from parsed keys and reuse layouts for repeated records. Evaluate a distinct
policy for dynamic keyed stores if descriptor sharing alone leaves excessive
transition growth.

## Invariants

- Each shape exposes only its own descriptor prefix, including during lookup
  and GC tracing. Descendant-only keys must remain invisible to ancestors.
- Appending to a descriptor owner may extend shared storage. Branching or
  modifying existing metadata requires a private copy of the visible prefix.
- Attribute changes, accessor replacement, deletion, and compaction must not
  mutate another object's descriptors. Allocation failure preserves old state.
- JSON duplicate keys replace values without changing first insertion order;
  `__proto__` is an own data property. Numeric-key ordering, embedded NULs,
  revivers, and GC rooting retain existing semantics.

## Work

1. Replace per-transition descriptor/index copies with shared backing storage.
2. Add native sharing, branching, mutation, prefix, and allocation checks.
3. Build JSON layouts separately from recursively converting values; reuse
   compatible layouts for neighboring records and reserve property storage.
4. Measure stable constructors, unique-key maps, JSON workloads, and bench-v8
   against the pinned starting binary. Add keyed-store policy only if needed.

## Evidence

V8 shallow checkout `/tmp/v8`, commit
`79ec23a5e9e0486df87af5d23cd95bc629dbb714`, demonstrates shared descriptor
prefixes, separate keyed-store limits, and dedicated JSON object construction.
Ant's earlier 32-child cutoff is an unvalidated experiment and will be replaced
by the structural work, not treated as an established policy.

## Implementation

Shapes hold a visible prefix of a reference-counted descriptor table and lookup
index. A transition at the end of that table shares it; a branch copies only its
visible prefix. Existing metadata writes detach shared storage. The lookup
rejects index entries outside the shape's prefix. Cached descriptor/index
pointers keep ordinary reads direct; an intrusive owner list refreshes them
only when storage grows. Borrowed descriptor pointers must be reacquired across
property additions, as documented in `include/shapes.h`.

The transition depth limit is now 1024 rather than 32. Beyond it, private shapes
still bound tree depth. The former experimental 32-child fan-out cutoff was
removed. No distinct named/keyed-store API is added in this change: the measured
keyed workloads already benefit from sharing. A streaming workload with new
keys on every iteration remains a useful separate policy test; the unique-key
benchmark here repeats a finite collection of 1000 distinct layouts.

JSON conversion checks the previous record's layout within an array, reserves
value slots once, and fills matching layouts directly. On a miss, wide objects
build a private final layout without intermediate transitions. Tiny standalone
objects retain single-pass construction. Duplicates use lookup while filling,
preserving first-key insertion order and the last value. Temporary object roots,
strong layout references, initialized slots, and write barriers protect recursive
conversion and GC.

An initial constructor regression was traced with `sample` and disassembly to
register saves in `ant_shape_release` on reference-count-only calls. Keeping
destruction in a separate non-inlined function recovered it. Profiles and
rejected intermediate binaries are retained with the measurement artifacts.

## Validation and measurements

Artifacts are retained in `/tmp/ant-shared-descriptors`. The pinned baseline
SHA-256 is `93b5e5ae935a9696117bfce9d09dbb3b4355a52a88a43881e7433b73f86a0e05`.
It is the pre-change local binary, not the historical master release.
The PGO input remains fixed at SHA-256
`3cd71ccd7b9f3444904d56886bad5b5ee768d6669086d1494180a4829ffaa3b7`.
Clang reports discarded profile counts for changed control flow, so these are
comparisons of the actual binaries with unchanged PGO, not regenerated profiles.

Candidate SHA-256: `9f386329b3b901fea6a713cd79e45c85c8f49d9621de8f944498bf89b978bfa6`. The final build is
byte-identical to the measured `ant-release-split` artifact. All comparisons
ran serially, ABBA twice, with four samples per binary. Rates below are median
work units per second; a work unit differs between fixtures.

| Focused workload | Baseline | Candidate | Change |
| --- | ---: | ---: | ---: |
| 64-field named constructor | 753.67 | 777.03 | +3.10% |
| 1000 distinct 48-key layouts (repeated) | 111.29 | 187.59 | +68.56% |
| Repeated 128-key layout | 127.54 | 165.63 | +29.86% |
| JSON repeated 64-field records | 1532.89 | 3665.59 | +139.13% |
| JSON distinct 48-field records | 1280.05 | 1456.52 | +13.79% |
| JSON standalone four-field object | 3423.85 | 3377.97 | -1.34% |

| bench-v8 workload | Baseline score | Candidate score | Change |
| --- | ---: | ---: | ---: |
| Richards | 5434.74 | 5371.20 | -1.17% |
| DeltaBlue | 5755.57 | 5745.65 | -0.17% |
| Crypto | 9984.66 | 10019.85 | +0.35% |
| RayTrace | 10803.00 | 11007.80 | +1.90% |
| EarleyBoyer | 9764.46 | 9796.38 | +0.33% |
| RegExp | 3968.65 | 4003.74 | +0.88% |
| Splay | 5830.06 | 5886.58 | +0.97% |
| NavierStokes | 20172.32 | 19974.16 | -0.98% |
| Geometric mean | 7913.31 | 7933.77 | +0.26% |

The full-suite mean is effectively flat at this run's variability. One
EarleyBoyer candidate sample was 7233.57 versus the other three at 9754–9848;
all samples, including that outlier, are retained. Richards and NavierStokes
medians were about 1% lower. Tiny standalone JSON remains 1.34% lower. The
large JSON/layout improvements are workload-specific, not a universal 10% win.

Validation on the final binary:

- Configured Meson build succeeded.
- All 76 JIT files and 16 focused JSON/property files passed.
- Spec suite: 4221 tests passed across 102 files, zero failures.
- Native shared-prefix/branch/mutation/GC tests and allocation-failure tests
  passed with UndefinedBehaviorSanitizer. The latter checks append and
  copy-on-write failures and shape-byte accounting after collection.
- AddressSanitizer could not run on this host: the initial LLVM runtime hung
  before main in allocator initialization (sample retained); an Apple Clang
  attempt also timed out and was terminated. No ASan pass is claimed.
- `maid preflight` passed knowledge and structure checks. Its suggested `.c`
  test invocations were handled as native compilations, not JavaScript inputs.
- `git diff --check` passed. Prior inline numeric-key edits were preserved.

## Known independent issue

The pinned baseline and candidate both turn `JSON.parse('-0')` into positive
zero; `JSON.parse('-0.0')` preserves negative zero. This comes from the existing
integer conversion path and is outside descriptor/layout construction. The
new duplicate-key test uses the real-number form to check sign preservation.
