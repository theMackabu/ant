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

## Validation status

- Confirmed the isolated JIT compiler command uses `-Oz` while the surrounding
  PCRE2 target remains at `-O3`.
- Confirmed the isolated PCRE2 change retains a 287.9 KiB signed-binary saving;
  the regex-path follow-up consumes 16.0 KiB of it.
- Passed 11 focused RegExp correctness tests after the regex-path follow-up.
- Passed the full spec suite: 3,886 tests across 100 files, zero failures.
- Byte-for-byte differential probes match Node for 104 deterministic long-byte
  search cases and match the exact pre-change Ant binary for valid UTF-8 and
  lone-surrogate RegExp behavior.
- Added regression coverage for long repeated-prefix searches, cached string
  patterns, and absent sticky splits.
- Passed `maid preflight` and `maid knowledge`.
