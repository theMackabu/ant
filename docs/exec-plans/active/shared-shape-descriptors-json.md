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
V8 searches for an existing transition before applying its keyed-store growth
policy. Ant follows that ordering; its property-count limits are its own policy.

## Implementation

Shapes hold a visible prefix of a reference-counted descriptor table and lookup
index. A transition at the end of that table shares it; a branch copies only its
visible prefix. Existing metadata writes detach shared storage. The lookup
rejects index entries outside the shape's prefix. Cached descriptor/index
pointers keep ordinary reads direct; an intrusive owner list refreshes them
only when storage grows. Borrowed descriptor pointers must be reacquired across
property additions and metadata mutations, as documented in `include/shapes.h`.

New named-store transitions have a 1024-property depth limit; computed-key
stores fork privately after 32 properties. Existing transitions are reused
before either limit is applied, preserving stable named layouts when accessed
through keyed stores. Numeric, string, and symbol stores use the keyed policy
in both VM and JIT paths, as does Object.fromEntries. The first child is stored
inline; only additional children require a hash table. These policies bound
new dictionary chains without penalizing linear stable layouts with a hash
allocation at each node.

JSON conversion checks the previous record's layout within an array, reserves
value slots once, and fills matching layouts directly. On a miss, wide objects
with at least 128 keys build a private final layout without intermediate
transitions. The cutoff is defined once; sibling layout feedback is checked
first. Repeated standalone bulk objects can reuse completed layouts through
the bounded per-runtime cache described below.
Tiny standalone objects retain single-pass construction. Duplicates use lookup while filling,
preserving first-key insertion order and the last value. Temporary object roots,
strong layout references, initialized slots, and write barriers protect recursive
conversion and GC.

An initial constructor regression was traced with `sample` and disassembly to
register saves in `ant_shape_release` on reference-count-only calls. Keeping
destruction in a separate non-inlined function recovered it. Profiles and
rejected intermediate binaries are retained with the measurement artifacts.

## Original descriptor-sharing validation and measurements

These artifacts predate the keyed-store and inline-child follow-up; they are
not measurements of the current working tree.

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

## Keyed-policy follow-up validation

Added native checks for numeric/symbol store routing, reuse of named transitions
past the keyed cutoff, and failed deletion with both live and collected descriptor
tails. Updated the validation router to run registered C tests through Meson.
Validation on 2026-09-08:

- Configured Meson build succeeded; existing PGO reports control-flow mismatches.
- Four Meson native tests passed with assertions enabled, including keyed-store
  policy, transition identity, descriptor OOM, and ASCII scanning.
- All 76 JIT files and 33 focused property, JSON, string, and RegExp files passed.
- Spec suite: 4221 tests passed across 102 files, zero failures.
- Router assertions passed under Node and Ant for registered native, unregistered
  native, and JavaScript test files.
- `maid preflight` and `git diff --check` passed.

Logs are in `/tmp/ant-keyed-policy-fixes` and `/tmp/ant-keyed-{build,native,preflight}.log`.
No benchmarks or PGO regeneration were performed; no new performance claim is made.

## Known independent issue

The pinned baseline and candidate both turn `JSON.parse('-0')` into positive
zero; `JSON.parse('-0.0')` preserves negative zero. This comes from the existing
integer conversion path and is outside descriptor/layout construction. The
new duplicate-key test uses the real-number form to check sign preservation.


## Completed JSON layout cache

The 128-key cutoff chooses bulk construction, without requiring distinct shapes
for repeated standalone parses. Bulk layouts are eligible for a per-runtime LRU
cache with at most 16 entries and 256 KiB of accounted shape, descriptor, index,
and key-byte storage. The fixed cache bookkeeping and allocator overhead are
additional; the entry and byte limits bound retention independently of input
layout diversity. Oversized and duplicate-key inputs are not admitted.

Lookups hash the ordered keys and verify all keys and descriptor attributes on
a fingerprint match. Existing array-sibling feedback is still checked first.
Cache entries retain shapes, not object values, and are released on eviction or
runtime destruction. These detached layouts have no transition-tree parents or
children, so tree GC cannot reclaim cache-held shapes. Their interned string
keys have the same lifetime as other shape keys. Cache allocation failure simply
skips admission.

A bulk-layout flag survives cloning. Shared bulk layouts detach on additions
without recording child transitions; unshared bulk layouts grow in place.
Metadata writes continue using existing object-level copy-on-write. This keeps
cached shapes immutable while allowing parsed records to mutate independently.
A rooted parent object owns its shape even if recursive parsing evicts its cache
entry before values have finished initialization.

Validation on 2026-09-08: configured build, five native Meson tests with
assertions enabled, all 76 JIT files, 35 focused files, and all 4221 specs across
102 files passed. Native checks cover shape identity, forced hash collisions,
private appends, both cache budgets, eviction, oversized admission rejection,
GC, and cache cleanup. JavaScript checks cover metadata mutation, key ordering,
duplicates, special keys, revivers, and eviction during recursive value filling.
`maid preflight` and `git diff --check` passed.

Artifacts and scripts: `/tmp/ant-json-layout-cache`. The baseline is the exact
pre-cache binary from the preceding keyed-policy fix, with its working-tree diff
saved alongside it. The source PGO is unchanged; Clang reports mismatched profile
counts for changed control flow. These are binary comparisons with existing PGO.

- Baseline SHA-256: `13a56028511cdd0b228e188d2dcab0da1bb0a15911fa6d7405701d0f2b7a3719`.
- Candidate SHA-256: `7a4cd53b71518cfcf29bd1f8ab0880658863f55c6ae26e6efa64d387c462eefb`.
- PGO SHA-256: `8faf005fd978c0b0c4343545c8938e58401ad61c165727a72ca0e9c4db6ab9fc`.

Each case ran serially ABBA twice, four samples per binary, on this M4 Pro.
The main fixture retains 2000 separately parsed records, then performs two
million calls reading two named fields. RSS is sampled after parsing; it includes
runtime, input, and object memory and is not an isolated cache-size measurement.
Medians below are elapsed milliseconds and MiB; smaller is better.

| Record layout | Parse baseline → candidate | Read baseline → candidate | RSS baseline → candidate |
| --- | ---: | ---: | ---: |
| 128 keys, repeated | 6.98 → 4.02 | 131.82 → 64.07 | 36.2 → 9.0 |
| 256 keys, repeated | 14.04 → 7.55 | 168.68 → 65.33 | 66.2 → 11.1 |
| 500 keys, repeated | 25.85 → 14.21 | 185.43 → 65.72 | 92.4 → 15.3 |
| 256 keys, alternating | 15.14 → 7.67 | 168.81 → 89.42 | 66.5 → 11.5 |
| 500 keys, unique | 111.67 → 113.67 | 191.11 → 188.50 | 257.3 → 256.9 |

Longer controls used 200000 four-key records, 20000 repeated 127-key records,
and 4000 unique 500-key records, each with the same ABBA-twice ordering. The
first two read 20 million times; the unique case read four million times.

| Control | Parse time change | Read time change | RSS change |
| --- | ---: | ---: | ---: |
| 4 keys, repeated | -1.30% | -0.77% | +0.21% |
| 127 keys, repeated | +1.30% | +2.28% | +0.32% |
| 500 keys, unique | +2.78% | -0.31% | -0.20% |

The repeated bulk-layout gains are clear. Unique-layout parsing pays about 2.8%
for cache probing in the longer control; its memory is effectively unchanged.
The 127-key control was 1.3% slower parsing and 2.3% slower reading; that path does
not enter the cache. These controls do not establish zero regression everywhere.
No overall bench-v8 speedup or fresh-PGO result is claimed.


## Remaining shape, array, and string review follow-up

Verified against the committed tree before edits:

- Clone index reservation already used `count + extra`; retained and added a
  native regression that checks the following append performs no allocation.
- Descriptor copies now use one descriptor-only prefix copier; append and
  metadata-write preparation have named entry points. OOM preserves original
  owners, property metadata, and shape accounting.
- Unlinking a tail owner marks its table dirty. On reuse, one owner-list scan
  lowers the visible backing count and rebuilds the existing index in place.
  Deferring this until reuse avoids quadratic owner scans while pruning a chain.
- Known integer element indices rely on the following unsigned comparison with
  a 32-bit array length, which rejects negatives and values at least UINT32_MAX.
- Array logical length can exceed dense capacity for sparse arrays. A spare flag
  proves `len <= cap` for JIT accesses; allocation, dense growth, and the length
  setter maintain it. Direct length writes elsewhere initialize a fully reserved
  literal or reduce length, so they cannot invalidate a true proof. A false flag
  after such a reduction is conservative. The existing flags guard now includes
  this proof and the separate capacity load/comparison is removed.
- JSON duplicate-slot lookup asserts its preparation invariant instead of
  misreporting a missing prepared slot as OOM.
- Empty string separators split into UTF-16 code units, preserving BMP characters
  and lone surrogates and dividing supplementary characters into surrogate halves.
  The implementation scans bytes once and preserves the ASCII metadata path.

Validation on 2026-09-09:

- Configured build passed; existing PGO reports control-flow mismatches.
- All six native Meson tests passed, including allocation-failure injection,
  reclaimed-tail sharing, clone index reservation, and the dense-length flag.
- All 77 JIT files and 36 focused string/property/JSON/RegExp files passed.
- All 4221 specs passed across 102 files.
- New split and sparse-length tests also passed under Node.
- MIR dumps confirm read/write guards test the new flag and use unsigned
  `index < len`, without an element capacity register/load or redundant known-int
  UINT32_MAX comparison.
- `maid preflight` and `git diff --check` passed.

Artifacts: `/tmp/ant-review-followups`. No benchmark result is claimed for these
follow-up edits. Sparse arrays whose length exceeds capacity take the JIT slow
path even for indices within the allocated prefix; their logical length is never
used to force a potentially enormous dense allocation.
