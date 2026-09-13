# bench-v8 regression recovery

Status: active
Owner: theMackabu

## Objective

Recover measured regressions after 33d3a51a and target a 9000 geometric mean without sacrificing runtime correctness. Preserve /tmp/ant/baseline. Current starting point 0c62ef72, regenerated PGO. Exact binary and review artifacts: /tmp/ant-v8-regression-20260912.

## Evidence and decisions

Initial serial full-suite ABBA: baseline geometric mean 8186.58; current 8084.62. RegExp -9.91%, Splay -3.61%; Crypto -0.49% and -1.47% in a separate BAAB. Background host load limits small differences.

5ccc664c fixes inherited lookup but thereby activates scalar plain-literal regex replacement. A same-baseline own-exec probe reproduces the slower route. Keep lookup correctness, use bounded memchr search, and avoid repeated writability checks where the existing retained shape/slot guard already proves them. Literal routes still validate writability once.

## Validation and checkpoints

- Focused JIT/RegExp/upvalue tests and full specs passed before final PGO; repeat against the final build.
- Next: investigate remaining Crypto/Splay gaps from fresh results. Keep only measured useful changes.
- Final: maid preflight and recommended validation; report actual scores and any unmet target.

## Current experiments

- Stage 1: RegExp recovered; full ABBA 8497 baseline / 8480 candidate, with host-load drift relative to initial scores. Splay remained about 4% behind.
- Stage 2: defer upvalue-chain budget division until arena membership succeeds (verified in assembly); reuse cached lastIndex store during global replacement setup. Focused 22 RegExp/upvalue tests pass. Fix UTF-16 search position on newly activated literal path.
- Stage 3: numeric array elements still call the out-of-line generic marker. Skip those calls with the same tagged-value predicate the marker already uses, preserving all reference marking.
- Extended 4-second-phase Splay profiling crashed both pinned baseline and stage-1 candidate after ~8s with >5GB RSS. Those runs are excluded; investigate separately from new source attribution. Standard fixtures pass.

- Stage 4: box singleton integer ranges directly as constants, eliminating I2D plus NaN-boxing checks. Fresh MIR confirms MOV immediates for integer array elements; fractional and negative-zero cases keep normal numeric boxing. All 91 focused JIT/RegExp/upvalue tests and 4229 specs across 102 files pass; maid preflight passes.
- PGO: preserved original profile (961bad24f10ca764126db1daaf6edcd99945c5b845f1757ec400b3dfddccbeb6) before retraining. Source-only results use that profile with changed-function counts discarded, so final retraining must be reported separately.

- Stage 5 (fresh PGO): full ABBA 8438.83 baseline / 8481.98 candidate (+0.51%). Crypto -1.03%, RegExp -0.89%; Splay +0.93%, EarleyBoyer +0.32%. This did not establish a 5% overall gain.
- Stage 6: share the existing side-effect-free builtin-exec property guard between batch and literal probes. Reject non-literal patterns before the exec lookup. Getter-count tests pass in Ant and Node. Full ABBA 8441.90 / 8491.43 (+0.59%), RegExp +3.25%. Retrain once more because three changed RegExp functions discarded their old PGO counts.
- Crypto follow-up: two-second native samples after four-second warmup show generated code dominating both endpoints. Final am3 MIR keeps the same arithmetic; differences concentrate in entry checks and shared bailout spills. No source-level cause for the remaining small throughput gap has been proven. Samples and normalized MIR diff: /tmp/ant-v8-recovery-stage5.

- Final-PGO pre-increment checkpoint: 93 focused tests, 4229 specs across 102 files and preflight pass. No profile hash-mismatch warnings. Four-process-per-binary suite results are under /tmp/ant-v8-recovery-final (this checkpoint predates the increment experiment below).
- Additional Crypto experiment: OP_POST_INC boxed/unboxed known numeric inputs, unlike OP_POST_DEC. Share the existing numeric post-update emitter, preserving old/new numeric slots and its bailout. Add signed-zero, integer-boundary, coercion, BigInt and indexed-use coverage. This removes 23 lines rather than adding a new fast path. ABBA failed the performance gate: 10527.91 before / 10252.32 after (-2.62%). Reverted the emitter and unlanded test; preserved the experiment under /tmp/ant-v8-increment-abba. The expanded coercion probe also fails on the pre-experiment binary because the existing interpreter post-update operations do not implement ToNumeric; this is a separate pre-existing correctness issue, not introduced by retained changes.

## Validated checkpoint

Retained binary SHA256: 72709d831e64f6a8a8d83b7679bbcbe8f2eecabf0e3a0c578632ce22f07d2a75. Restoring the rejected post-increment experiment rebuilt byte-identically to the tested candidate. Baseline SHA256 remains c6eb09293457d785a6150223dd2181870e463a3bcf6270a50c4e5ecb95359bf0.

Final ABBA+BAAB, four processes per binary per workload: geometric mean 8417.07 baseline / 8495.94 candidate (+0.94%). Richards +1.01%, DeltaBlue +1.39%, Crypto -0.15%, RayTrace +1.32%, EarleyBoyer +1.94%, RegExp +2.18%, Splay -0.11%, NavierStokes -0.05%. These data support recovery of the larger endpoint losses, not a blanket 5% improvement. Background-load outliers remain visible in the raw ranges. The broader optimization target remains open.

User reported possibly sleeping during the runs. Checked macOS pmset log: last recorded wake 2026-09-12 21:55:52 PDT; saved benchmark sessions ran 22:13:40 through 23:18:03 PDT with no logged overlapping system sleep. This does not eliminate background-load or thermal variation. Final suite ran 23:13:06–23:15:56 PDT with caffeinate -i.

Artifacts: /tmp/ant-v8-recovery-final/abba.json (raw samples, ranges, hashes, load); /tmp/ant-v8-regression-20260912/validation-final-pgo.log (93 focused tests and 4229 specs); /tmp/ant-v8-regression-20260912/pgo-final.log; /tmp/ant-v8-increment-abba (rejected experiment).

## 9000 target

User raised target to 9000 on 2026-09-12. Preserve the validated 8496 candidate and profile under /tmp/ant-9k-20260912. Profile lower-scoring workloads, isolate source changes against that binary, then validate the full suite. Existing /tmp/ant/baseline remains preserved.

- RegExp allocation: share VM/JIT literal construction with the module implementation; cache the ten flag-property descriptors after the source prefix. Retain and mark the prefix for GC transition discovery. Preserve per-instance values, prototype, descriptor isolation and compile() writes. Reuse canonical immutable flag strings, and reserve the three private flag slots together. Source-only RegExp ABBA: 4274 / 4871 (+13.97%).
- Arguments allocation: initialize the own dense prefix with the existing dense-literal allocator; retain parameter mapping, symbol writes and iterator lookup. Capacity-boundary and object-identity tests pass alongside async/escaped-arguments tests.
- Fresh PGO checkpoint: 94 focused tests and 4229 specs across 102 files pass, no profile hash-mismatch warnings. RegExp 4129 / 4762 (+15.33%). First suite mean 8324 / 8322 was affected by a candidate RayTrace outlier (9057 versus 11004). Eight-run recheck: DeltaBlue -0.83%, RayTrace -2.05%, Splay -1.70% versus the saved 8496 candidate. Keep these small endpoint losses under investigation; do not claim the target reached. Artifacts: /tmp/ant-9k-layout-pgo-abba and /tmp/ant-9k-layout-recheck.
- Next experiment: enable ordinary guarded inlining at tail-call sites outside try regions. Preserve self-tail frame reuse and route inline returns through the caller's normal closure cleanup. Add IC-epoch availability for inlined field reads at tail-only sites. Require code-generation evidence, correctness tests and a full performance comparison before retaining.

- Tail-call inlining now records targets before frame reuse, excludes recursive self tails and active try regions, and returns through normal closure cleanup. Fixed direct inliner receiver selection (strict undefined versus sloppy global), and guard primitive/null method receivers before any effect. Allow post-effect property reads only through full helpers that cannot request replay of the whole callee. MIR tests prove both tail-call forms and the full post-effect read helpers emit. 96 focused tests and 4229 specs pass. Source-only full ABBA versus the layout candidate: 8496 / 8520 (+0.28%).
- PGO training experiment: standard workload set plus all eight bench-v8 fixtures, preserved in /tmp/ant-9k-pgo-v8/train.sh and raw-with-v8. No checked-in training script change. Fresh full ABBA versus the exact pre-training binary: 8505.95 / 8756.58 (+2.95%). RayTrace +6.04%, EarleyBoyer +5.73%, RegExp +7.74%, Splay +3.91%; other rows between -0.39% and +0.39%. Fresh binary passes 96 focused tests and all specs. Profile and binary provenance under /tmp/ant-9k-pgo-v8-abba.
- Rejected ASCII-only PCRE2 variant: bounded 256 KiB secondary code budget, cold/warm and Unicode fallback tests matched the pre-change binary. RegExp gained only 0.45% over eight alternating processes. Reverted the extra matcher, budget fields and test; preserved experiment under /tmp/ant-9k-ascii-abba. Tests also exposed existing Unicode lastIndex/indices and named-replacement differences versus Node, reproduced in the pre-experiment binary; these are not introduced regressions.
- Pattern-level diagnostic instrumentation (removed, timing results excluded from scoring) attributed 527 ms of 816 ms PCRE2 time to one unanchored pattern on long nonmatching subjects. Add a conservative required-literal rejection check for case-sensitive patterns without a PCRE2 start-character check. The scanner considers only top-level literal runs, removes quantified final atoms, rejects top-level alternatives, inline options, extended groups and unknown escapes, and never contributes literals from groups/classes. A hit always uses PCRE2. The filter runs only on subjects at least 128 bytes long. Eight-process RegExp comparison: 5262.71 / 6468.23 (+22.91%). Final source validation, full-suite comparison and fresh PGO are pending.

- Required-literal final source: 97 focused tests, 4229 specs / 102 files, Node regression assertions, preflight and knowledge checks pass. Full ABBA against the pre-filter enriched-PGO binary: 8721.62 / 8947.10 (+2.58%), RegExp +21.87%; small cross-workload changes still require final PGO/longer comparison. Raw results: /tmp/ant-9k-required-full-abba.
- Operand-stack sweep reported no rejected analysis and no JIT overflow. It exits 2 because test_throw_stack.cjs and test_with_strict.cjs intentionally exit 1; both do the same on /tmp/ant/baseline. No source change was made to those negative tests or the sweep script.
- Added the eight bench-v8 workloads to meson/pgo/build.sh using the existing portable harness. This makes the expanded PGO training reproducible without temporary fixtures or rewriting score.json. bash -n passes; final build uses a copy with only the root path pinned and an archive hook before profile merge. Artifacts: /tmp/ant-9k-final-pgo.
