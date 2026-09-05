# Crypto import performance

Status: complete. Final verification below supersedes the intermediate checkpoints.

Target: 20–30 ms for each of 100,000 AES-256 and HMAC-SHA256 raw imports,
using the existing 5,000-import warmup and serial alternating binary comparisons.

Preserve validation, rejected-Promise behavior, native usage enforcement,
extractability, independent metadata objects, raw byte snapshots and cleansing.
Do not change shared iterator behavior or benchmark workloads to meet the target.

Baseline: regenerated-PGO binary SHA-256
`1f4e2ef146261e8bc63dfa707faa64ed5d7abe4618d7f3ebdfff9c3a0e5e36c3`.
Most recent medians were 82.7 ms AES and 90.3 ms HMAC. These do not meet the target.
Saved baseline and profile artifacts: `/tmp/ant-crypto-goal.7cySux/`.
Fixture: `/tmp/ant-crypto-perf.uALpeb/import.js`.

Plan:

1. Profile the current import path.
2. Reduce native allocation and metadata construction overhead locally.
3. Build and run focused crypto regressions and crypto spec checks.
4. Compare identical workloads against the saved baseline; record results.
5. Continue profiling remaining costs until the target is verified.

## First measurements

- AES sampling: 252/758 samples in usage iteration, 324/758 in key construction
  (inclusive, including allocation and collection).
- Combined the native key header and bytes into one allocation, keeping byte
  cleansing before free and checking allocation-size overflow. This alone was
  marginal: approximately 79–90 ms.
- Added a crypto-local dense-string-array usage path. It requires the original
  array iterator and next method, excludes holes and exotic arrays, and falls
  back for conversions or invalid usage strings so IteratorClose still occurs.
  Shared iterator functions are unchanged. The original next function is held
  in the existing per-isolate mutable-root inventory, so GC marks it normally.
- Four serial alternating baseline/candidate runs (100,000 imports, 5,000
  warmup): AES baseline 81.9 ms, candidate 50.6 ms; HMAC baseline 90.5 ms,
  candidate 58.3 ms. Same regenerated profile and build configuration; changed
  functions emitted profile-mismatch warnings. Target remains unmet.
- Build, preflight, generate/export regression, extended crypto regression,
  and all 123 crypto spec checks pass. Added tests for array iterator overrides,
  overridden next, conversion callbacks, and invalid-usage IteratorClose.

## Property construction and metadata reads

- Private per-isolate record templates now retain constant strings and shared
  shapes. Each key still gets independent key/algorithm/hash/usages objects.
  Fresh records copy template slots with GC barriers and then initialize their
  dynamic slots; no user-visible object is reused. Templates remain GC-rooted.
- This reduced four-run medians from 51.8/59.5 ms to 36.6/43.9 ms (AES/HMAC).
- Cached usage strings plus `js_mkarr_dense_literal` avoid repeated string
  creation and pushes. Fresh-record initialization no longer invalidates ICs
  that cannot exist before the object escapes. Write barriers remain intact.
- Added guarded direct algorithm-member reads for plain data properties and
  ordinary missing members; getters/proxies/unusual cases use `js_get`.
  Primitive string conversion returns directly and HMAC digest matching stops
  on its first match.
- Latest four-run medians: AES 32.4 ms, HMAC 37.5 ms. Previous-step medians in
  that same comparison were 32.4/39.8 ms. The target is still unmet.
- Build, preflight, focused generate/export and extended crypto regressions,
  and all 123 crypto spec checks pass. New coverage checks independent metadata,
  shape/descriptor mutation isolation, inherited getters, and proxy trap order.
- Saved successive binaries: `dense-baseline`, `template-baseline`, and
  `strings-baseline` in the artifact directory. HMAC profile after templates:
  `template-profile.txt`. Profile-mismatch warnings remain for changed code.

Next: profile remaining HMAC and object-allocation costs. Do not call the goal
achieved until both import workloads reach the original target with correctness
preserved.

## Allocation follow-up

- Expanded the private hash templates to cache each of the four supported hash
  names. Simplified pristine iterator checks to inspect the located shape
  properties directly, still excluding accessors and exotic objects.
- Added the internal, opt-in `js_mkobj_from_template` allocator in `src/ant.c`.
  Existing object constructors and shared iterators are unchanged. The helper
  accepts only plain record templates, roots the source across collection,
  independently allocates overflow property storage, retains the shared shape,
  copies initialized slots, resets GC state and IC identity, and links the
  newly allocated object into the normal object list. Native/hidden state is
  rejected rather than cloned. Newly allocated objects are young, so copied
  references do not require an old-to-young write barrier.
- Preliminary template-allocation timings were 28.5 ms AES / 31.8 ms HMAC;
  these were not final because an incremental build was still finishing.
- The pending build also avoids temporary-root overhead when the allocator
  cannot collect and reuses existing interned name/length keys for algorithm
  lookup. Final correctness and performance validation remain pending.
- Added a retained-key regression across 10,000 imports to exercise metadata
  and native byte ownership through allocation churn and collection.

## Final verification

Both workloads meet the original 20–30 ms target without changing the fixture.
Eight measured runs per final/installed binary, serial forward/reverse ordering,
each with 5,000 warmup imports and 100,000 timed imports:

| Import | Final median | Installed median |
| --- | ---: | ---: |
| AES-256 | 26.4 ms | 30.9 ms |
| HMAC-SHA256 | 29.0 ms | 32.9 ms |

Final AES samples (ms): 26.522, 26.662, 27.233, 26.832, 26.343, 26.362,
26.108, 26.168. Final HMAC samples: 28.689, 29.062, 29.204, 29.381, 28.528,
28.981, 29.021, 28.887. The first series also reran the original regenerated-PGO
baseline: medians 75.8 ms AES and 84.8 ms HMAC.

Final binary SHA-256:
`d4ec6832092dfd214b3c2a74f542d5e01047c2accb457bd06d714c4aec89ac53`.
Saved copy: `/tmp/ant-crypto-goal.7cySux/final`.
Installed binary SHA-256:
`31405c425ee05438f898a12bf7eb514af50aca792841c41bbc1b1b7ec3048d77`.
Installed build settings are not a controlled compiler baseline; the saved
original candidate uses the same configured tree/profile as the final build.
The final build still emitted stale-profile warnings for changed functions.

Validation passed:

- `meson compile -C build`
- `maid preflight`, `maid knowledge`, `git diff --check`
- `./build/ant --no-color tests/test_webcrypto_generate_export.cjs`
- `./build/ant --no-color tests/test_crypto_extended.mjs`
- `./build/ant --no-color examples/spec/run.js --all`: 4,135 tests passed,
  zero failed; 102 files passed. Log: artifact directory `final-spec.log`.

Correctness coverage includes algorithm normalization, imported/generated
extractability, native usage enforcement, raw snapshot/export ownership,
partial-byte HMAC keys, rejected promises and exception identity, iterator
overrides/conversions/closing, inherited getters and proxy trap order, independent
metadata and descriptor mutation, and retained keys through allocation churn.
Native key bytes are still cleansed before the single allocation is freed.
The existing shared iterator functions and generic object constructors were
not modified. Previously accepted exotic iterator conformance gaps and existing
Reflect return-value issues are outside this optimization and remain unchanged.
