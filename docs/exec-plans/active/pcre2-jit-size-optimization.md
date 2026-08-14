# PCRE2 JIT Compiler Size Optimization

Status: active
Last updated: 2026-08-14

## Goal

Reduce Ant's stripped binary size by compiling only PCRE2's JIT compiler at
Clang's size-optimization level, without changing optimization for the rest of
PCRE2 or accepting an unmeasured RegExp execution regression. Remove
pathological runtime costs in Ant's surrounding RegExp and string-search paths
where those costs dominate PCRE2 execution.

## Scope and constraints

- Patch the PCRE2 Meson subproject through `pcre2.wrap`; do not depend on edits
  to the extracted, ignored subproject.
- Compile only the 8-bit `pcre2_jit_compile.c` used by Ant at `-Oz`.
- Keep the rest of PCRE2, including the ordinary matching engine, at `-O3`.
- Compare the exact pre-change binary and candidate with warmed, serial,
  alternating A/B runs of `tests/bench_regex.cjs`.

## Current result

The matched `strip -x` artifacts decreased from 9,371,072 to 9,074,544 bytes,
a 296,528-byte (289.6 KiB, 3.16%) reduction. After applying the project's
normal ad-hoc codesign step to both artifacts, the matched sizes are 9,335,216
and 9,040,384 bytes: a 294,832-byte (287.9 KiB, 3.16%) reduction. The Mach-O
`__text` section accounts for 293,560 bytes (286.7 KiB) of that reduction.

The first unaligned candidate produced these paired median changes over 30
warmed AB/BA pairs:

| Benchmark | Change | Paired p10-p90 |
| --- | ---: | ---: |
| compile route regexp x5000 | -0.24% | -2.73% to +1.31% |
| route matches x400 | +1.17% | -3.58% to +5.89% |
| token scan x500 | +4.27% | +1.48% to +6.40% |
| identifier split x500 | +7.20% | +4.59% to +8.62% |

The construction row was flat, but the two heavier matching rows showed a real
regression. An experiment that moved `pcre2_jit_match()` into its own `-O3`
translation unit did not improve any row and was reverted.

The exact contemporary `-O3` build placed the every-match
`regexp_exec_internal` helper on a 16-byte boundary, while the smaller
`-Oz`/LTO layout placed it four bytes past one. Giving that existing hot helper
an explicit 16-byte alignment recovered the regression without affecting the
binary's final file size. Thirty warmed final-candidate AB/BA pairs against the
exact `-O3` build measured:

| Benchmark | `-O3` median | Final median | Paired change | Paired p10-p90 |
| --- | ---: | ---: | ---: | ---: |
| compile route regexp x5000 | 6.315 ms | 6.310 ms | -0.08% | -1.74% to +1.44% |
| route matches x400 | 42.820 ms | 43.785 ms | +2.07% | -5.60% to +4.75% |
| token scan x500 | 320.890 ms | 322.700 ms | +0.71% | -2.20% to +4.19% |
| identifier split x500 | 310.010 ms | 311.755 ms | +0.45% | -0.87% to +3.97% |

Every final interval crosses zero. The original repeatable 4-7% matching
regression is no longer present.

## Regex-path follow-up

Profiling and size-scaling probes found four costs outside generated regex
execution:

- Sticky matching, used by `RegExp.prototype[@@split]`, asked PCRE2 to validate
  the complete UTF-8 subject again at every candidate offset. Ant now reuses
  the flat string's cached UTF-8 validity and sets `PCRE2_NO_UTF_CHECK` only
  for the non-JIT matching route when the subject is known valid.
- The plain-literal RegExp fast path used a first-byte scan plus `memcmp`, which
  becomes quadratic on long repeated-prefix near misses. Patterns longer than
  32 bytes now use compiled PCRE2 instead.
- String `replace` and `replaceAll` used the same naive byte search, and
  `replaceAll` advanced one byte at a time between misses. Long needles now use
  a linear KMP search, while short needles retain the small inline scan, and
  `replaceAll` appends unmatched spans in bulk.
- String-pattern `match` and `search` compiled PCRE2 code on every call. They
  now acquire the existing compiled-pattern cache, with an active reference
  that protects a cache entry across result allocation and GC.

Matched non-PGO candidate and pre-follow-up binaries, both using the same
PCRE2 `-Oz` patch and hot-function alignment, measured:

| Probe | Baseline | Candidate | Paired change |
| --- | ---: | ---: | ---: |
| absent sticky split, 16K bytes | 326.481 ms | 12.268 ms | -96.25% |
| long literal RegExp near miss, 16K bytes | 19.413 ms | 0.007 ms | -99.96% |
| string replace near miss, 16K bytes | 19.404 ms | 0.333 ms | -98.29% |
| repeated string-pattern match, 20K calls | 10.852 ms | 3.649 ms | -66.51% |
| repeated string-pattern search, 20K calls | 7.651 ms | 0.637 ms | -91.60% |

The final 20-pair normal RegExp benchmark remained flat: compile `+0.15%`,
route matching `+0.33%`, token scanning `-1.05%`, and identifier splitting
`-2.25%`. The p10-p90 interval crossed zero for compile, route matching, and
token scanning; identifier splitting's interval was `-7.76%` to `-0.33%`.
Common short literal RegExp hit and miss probes were also flat, while ordinary
string replacement improved.

The follow-up runtime code adds 16,432 bytes (16.0 KiB) between matched,
stripped, ad-hoc-signed non-PGO artifacts. Applied to the isolated PCRE2
saving, the combined net reduction remains about 271.9 KiB.

## RegExp split batch follow-up

The remaining absent-split route was still quadratic because
`RegExp.prototype[@@split]` invoked a sticky match at every candidate byte,
updated `lastIndex` through generic property operations, and allocated a
temporary exec-result array for every successful match. The split algorithm
now has a guarded ASCII built-in path that performs one unanchored PCRE2/JIT
search for the next match and materializes split pieces and captures directly
from the match ovector. An unmatched input reuses the original string value
instead of copying it.

The fast path runs only after the specified species construction has occurred,
and only when the resulting splitter has genuine RegExp internal slots, the
exact built-in data-property `exec`, sticky matching, and a guarded writable
own `lastIndex`. Custom species, observable `exec`, read-only `lastIndex`, and
non-ASCII subjects retain the generic algorithm.

The exact non-PGO baseline and candidate use the same PCRE2 `-Oz` build and
settings. Their SHA-256 hashes are
`bc996bab8465ea3b7072c3ef28d09a1117a687d960c07b687ebb5716fd1b3145`
and
`bbfd5b316fb5043d065df5d804b507db09fb31d36cc7fd4eafe1f55ed133cf8e`.
Fifteen serial alternating AB/BA pairs measured:

| 16,000-byte split shape | Baseline | Candidate | Speedup | Candidate / Node 26.5.1 |
| --- | ---: | ---: | ---: | ---: |
| absent delimiter | 1,178.989 us/op | 1.379 us/op | 854.7x | 3.93x |
| one sparse delimiter | 1,231.760 us/op | 6.321 us/op | 194.9x | 12.82x |
| 8,000 dense delimiters | 2,091.699 us/op | 175.806 us/op | 11.9x | 1.49x |
| 8,000 captured delimiters | 2,310.742 us/op | 298.206 us/op | 7.7x | 2.44x |
| empty matcher | 5,738.403 us/op | 598.901 us/op | 9.6x | 1.96x |

The isolated 8,000-byte absent case measured 588.845 us/op for the baseline,
0.840 us/op for the candidate, and 0.179 us/op for Node across 15 pairs: a
700.6x Ant speedup and a remaining 4.70x Node gap. The combined multi-shape
probe's 8,000-byte absent row coincided with a deterministic collection phase, so
the isolated measurement is used for that size.

Twelve final AB/BA pairs of the normal RegExp benchmark stayed flat: compile
`-1.33%`, route matching `-0.58%`, token scanning `-0.01%`, and identifier
splitting `-0.56%`; every paired p10-p90 interval crossed zero. The split
change adds 2,232 bytes to Mach-O `__text` and four bytes to unwind metadata,
while both stripped artifacts remain 9,520,416 bytes because the containing
segments do not grow.

## Validation status

- Confirmed the isolated JIT compiler command uses `-Oz` while the surrounding
  PCRE2 target remains at `-O3`.
- Confirmed the isolated PCRE2 change retains a 287.9 KiB signed-binary saving;
  the regex-path follow-up consumes 16.0 KiB of it.
- Passed 11 focused RegExp correctness tests after the split batch follow-up.
- Passed the full spec suite: 3,886 tests across 100 files, zero failures.
- Byte-for-byte differential probes match Node for 104 deterministic long-byte
  search cases and 1,827 split cases, and match the exact pre-change Ant binary
  for valid UTF-8 and lone-surrogate RegExp behavior.
- Added regression coverage for long repeated-prefix searches, cached string
  patterns, absent sticky splits, captures, limits, empty matches, legacy
  RegExp statics, final `lastIndex`, non-ASCII fallback, and observable custom
  split species behavior.
- Passed `maid preflight` and `maid knowledge`.
