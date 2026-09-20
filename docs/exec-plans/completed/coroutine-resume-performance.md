# Coroutine resume performance recovery

Status: completed
Last reviewed: 2026-09-19
Owner: theMackabu

## Outcome

The coroutine keeps its resume value and error flag in call arguments rather
than persistent fields. It remains 136 bytes versus 144 before the refactor;
native/JIT returns remain eight bytes. The original matched-profile refactor
had suspended-entry +5.88% and tight fulfilled-await +3.94% regressions. Removing
redundant ownership work recovered those costs. A follow-up addressed the small
remaining synchronous-result and regex losses with focused shared-helper changes.

Final serial comparisons against exact pre-refactor
`0e9b2a639445e1ae53e5e15301352f2ad0d61c0f` show faster suspended entry and
synchronous iterator results, with ASCII/replacement regex flat. This is measured
recovery on the tested host and profile, not an exact zero-cost guarantee for
every workload.

## Decisions

- Each registration has one owner: the direct queued job or a promise reaction's
  await hold. Generic settlement transfers the detached hold, or acquires a
  private reference if none exists. Inner resume borrows that reference; legacy
  native wrappers own their own. Cancellation releases the detached hold.
- Direct dispatch validates the current job identity, then clears the
  registration directly. It borrows the dispatching job's existing reference
  and GC root. It has no promise reaction or extra await hold to remove.
  Generic settlement cannot borrow a merely queued job's reference because
  nested draining could dispatch that job before the outer resume returns.
- Iterator unpack reuses the decoded result while `done` is an own data
  property. After getter/proxy fallback it decodes again for `value`, preserving
  read order and descriptor changes. Regression tests cover both loop forms.
- Own-property reads reuse the first object decode for ordinary-object proxy
  rejection. The literal-regex guard calls this helper several times per match.

The final property helper has 265 ARM64 instructions versus 281 in the baseline;
direct-job resume has 213 versus 236 in the previous ownership candidate.
Static instruction counts support removal of work, but do not attribute exact
cycle savings. Native samples identified property-guard work before regex
matching and interpreter/call work in synchronous iteration. The original
small losses had identical code in several inspected helpers and are not fully
causally attributed to argument passing.

Outlining activation installation and relocating the job pointer did not help
in earlier pilots and were rejected. Pilots discarded changed-function PGO
counts; all final results below use freshly matched counts instead.

## Final measurements

Apple M4 Pro, macOS, release/O3/native/LTO8. Both arms use one merged profile from
the same 57 standard workloads plus 50,000 HTTP requests per variant. The
unchanged baseline's training data was reused. Neither release runtime build
discards mismatched counts; all 937 vendor compile commands exclude PGO.

Four processes per arm in alternating order, each async/fixed-work process
reporting the median of nine rounds. Negative time change is faster:

| Workload | Unit | Baseline | Candidate | Time change |
| --- | --- | ---: | ---: | ---: |
| Suspended entry, 1M calls | ms | 180.902 | 171.735 | -5.07% |
| Synchronous iterator results, 1M | ms | 23.387 | 22.081 | -5.58% |
| ASCII regex, standard microbench | ns/op | 86.815 | 86.520 | -0.34% |
| UTF-16 regex, standard microbench | ns/op | 94.815 | 94.055 | -0.80% |
| Replacement regex, standard microbench | ns/op | 127.265 | 127.155 | -0.09% |
| ASCII regex, fixed 2M operations | ms | 178.135 | 177.819 | -0.18% |
| Replacement regex, fixed 2M operations | ms | 261.781 | 256.754 | -1.92% |

Broader controls retain the fulfilled-await gains; Math.min, push, pop and
generator churn are flat or slightly faster. Array/Map iteration and Life are
mostly within about 1%. Pooled V8 score changes range from -0.70% to +1.82%,
with two or four processes per arm and substantial variance in some cases.
The short synchronous control's +1.92% becomes -0.21% at four times the work;
no-await entry's +0.99% becomes +0.36% in the longer repeat. All observations,
including outliers, are retained. The full microbench was not run; six selected
native/regex rows and all eight V8 cases were checked.

## Validation and artifacts

All 4,240 specs, 21 focused files, and native error-handoff and exact-GC-edge
tests pass. Added iterator getter-order tests also pass Node. Native coverage
includes cancellation/replacement, queued heap values through minor/major GC
with C-stack roots disabled, and explicit settlement after cancellation.
Preflight, knowledge, and whitespace checks pass. Isolated builds/tests replace
recommended main-tree commands; the main binary/profile and index are preserved.

The operand-depth sweep finds no analysis rejection or JIT stack overflow, but
exits 2 for four unsuccessful commands also reproduced with the baseline:
missing generated Hono dist, events.once prototype-spoof timeout, and the
throw-stack/strict-with negative cases. These are outside this change.

Final artifacts: `.cache/coroutine-recovery-z3mnn50z/residual/`, including
`report.md`, `results.json`, `results.csv`, source patch/hashes and snapshots.
`qualified/` contains the final binaries, profile, assembly, and validation logs;
raw comparisons live under the parent's `comparisons/residual-qualified-*`.
The preceding argument and ownership comparisons remain in
`.cache/coroutine-bench-xz26tztb/` and the recovery directory's `queue-owner/`.
