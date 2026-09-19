# Native profile of the GC experiment branch

Status: completed
Date: 2026-09-17
Owner: theMackabu

## Outcome

Profile the actual benchmark workloads before choosing another optimization.
The evidence supports generated-code work for Richards, DeltaBlue, Crypto and
NavierStokes. RegExp, Earley, Boyer, RayTrace and Splay also have substantial
native runtime costs that need different approaches. No optimization was made.

The profiled source is `a1ba1567d96733c4e00cee97829995d2a352a136`, the
`gc-experiment-tracing` tip, not current master or the experimental SSA tier.
Production binary SHA-256:
`9d1967f042ce844189e10eb93dddd35fea6f197f0433b167736156d7c00c134c`.
Committed PGO SHA-256:
`646200e587bedd95873fc1c920979026b258687c74eb47336b9e1f0caa394616`.

## Method and artifacts

Artifacts, scripts, pinned binaries/profile, native code bytes, disassembly,
Speedscope exports and machine-readable results are preserved locally in
`.cache/bench-v8-native-profile-20260917/` at the repository root. This location
survives normal reboot cleanup of `/tmp`; it is ignored by Git. `manifest.json`
records identities and `summary.json` records the per-workload results.

Used `/usr/bin/sample` with a requested 1 ms interval for five seconds after
the benchmark's one-second warmup. The profiling fixture extends the measured
window to eight seconds. Original workload bodies and their correctness checks
are unchanged. These extended runs are diagnostic fixtures, not canonical
benchmark scores. `examples/bench-v8/score.json` was not written.

All eight workloads were covered, splitting Encrypt/Decrypt and Earley/Boyer
into ten profiling targets. Decrypt retains the preceding Encrypt phase; Boyer
retains the preceding Earley phase. Only the selected component is sampled.
An initial standalone Decrypt fixture omitted encryption and failed its output
check; it was discarded, corrected, and rerun. No failed sample enters results.

Two serial production sweeps use opposite workload order. Ten additional
diagnostic runs resolve JIT code addresses: 30 successful profiles and 121,625
main-thread samples in total. A diagnostic-only copy at
`/tmp/ant/gc-native-profile` logs published machine-code ranges/bytes in MIR and
function source offsets/compile times in Ant. Its exact patch is in
`diagnostic.patch`. The production binary, original GC checkout and master
runtime/compiler sources are unchanged. No PGO training was performed.

Native stack trees supply exclusive main-thread samples by subtracting direct
child counts, with conservation assertions. GC and compilation ancestry take
priority in classification. Remaining native samples are grouped by responsible
runtime path; generated code is separate. A generated getter/constructor body
is counted as generated code, not as native property/construction overhead.
The two production sweeps supply percentages; diagnostic runs supply function
names and instruction addresses. This avoids presenting an instrumented build
as an exact production timing control.

## Production distributions

Ranges span the two untouched-binary profiles. Percentages are samples, not
hardware cycle counts. The generated-code column contains anonymous native PCs;
the mapped diagnostic resolves them to JS functions except for 1.9 percentage
points remaining unassigned in RegExp. Recognized PCRE matching ancestry is
counted separately, including its anonymous generated matcher code.

| Component | Generated/unassigned PCs | GC | Other important native paths |
| --- | ---: | ---: | --- |
| Richards | 92.2–93.2% | ~0.1% | Property runtime 6.5–7.3% |
| DeltaBlue | 76.1–76.3% | 2.5–2.9% | Construction 5.9–6.6%; properties 5.5–5.7%; call/interpreter dispatch 2.5–2.8% |
| Encrypt | 86.5–86.9% | 0.4–0.5% | Construction 4.2–4.5%; properties 2.7–3.1% |
| Decrypt | 98.8–99.0% | <0.1% | Small native remainder |
| RayTrace | 63.7–67.4% | 2.9–3.3% | Construction 22.1–24.0%; properties 5.0–6.3% |
| Earley | 35.2–35.4% | 13.7–13.8% | Closures/upvalues 20.7–21.0%; arguments 11.9–12.0%; other construction 12.2–12.9% |
| Boyer | 43.6–44.3% | 21.8–25.7% | Construction 22.0–23.1% |
| RegExp | 5.6–6.4% | 6.0–6.3% | Matching 20.9–21.0%; string/regexp runtime 46.9–47.4%; properties 11.3–11.4% |
| Splay | 30.5–30.7% | 30.8–30.9% | Construction 12.5–12.8%; properties 17.7–18.1% |
| NavierStokes | 99.8% | 0 observed | Compilation ~0.2% |

Compilation is below 0.3% of the production steady-state samples in every
component. This does not characterize startup. Diagnostic logs record zero
compilations in the selected measured windows for Richards, Encrypt, Decrypt
and Splay; the other components record approximately 9–19 ms across their
eight-second windows. The preceding warmup/setup phases have additional work.

Background threads are excluded from main-thread percentages. They mostly
waited, with active samples equivalent to about 4.2–4.6% of the main-thread
sample count for Earley and 1.0–1.3% for DeltaBlue. Those are overlapping worker
observations, not a direct measurement of total process CPU time.

## Resolved hot functions and native code

The following self percentages are from the separate mapped diagnostic, not the
production percentages above. Anonymous functions were identified using logged
source offsets and verified against the fixture text.

- Richards: `Scheduler.schedule` 31.9%, `TaskControlBlock.run` 17.2%,
  `HandlerTask.run` 14.1%. Their emitted functions are 14,400, 16,784 and
  34,992 bytes respectively, including cold paths.
- DeltaBlue: `Plan.execute` 49.7% and 34,080 generated bytes. Hot windows contain
  repeated tag extraction, metadata loads, stack loads and call transitions.
  Function size alone does not establish an instruction-cache bottleneck.
- Crypto: `am3` 70.3% of Encrypt and 79.0% of Decrypt. A sampled hot load reads
  array flags and is followed by masking/comparison and a bailout branch.
  Other sampled locations include floating-register moves and stack traffic.
- NavierStokes: `lin_solve` 73.3%, `project` 11.3%, `advect` 8.1%.
  A hot `lin_solve` window performs a required floating addition, then a boxed
  Number guard on a multiplication operand. Guard elimination needs a proof;
  the useful floating-point work is not removable overhead.
- Splay: `splay_` 20.3%, `GeneratePayloadTree` 7.7%. Native GC self samples
  concentrate in scanning, collection and object freeing. Literal creation,
  field stores, slot definition and property invalidation are also visible.
- Earley: native arguments creation and closure/upvalue initialization and
  closing are large costs. Boyer's mapped bodies include `one_way_unify1_nboyer`,
  `rewrite_nboyer` and `sc_Pair`, alongside pair construction and GC.
- RegExp: native replacement, split, result/property handling, string creation
  and copying dominate outside matching. `builtin_regexp_symbol_replace` has
  about 46% inclusive samples in one production profile; this includes matching
  and allocation and must not be added to those categories.

`disassemble.py` pairs sampled PCs with the captured instructions using LLVM
tools. PC-at-instruction frequencies are not instruction retirement counts and
do not establish cache misses, branch misses or stall-cycle attribution. A
sample on a load does not imply that all its elapsed cost is removable.

## Implications and limits

The strongest bounded compiler experiments are `am3` and `lin_solve`, followed
by sustained, simplified specialization of `Plan.execute` and the Richards
scheduler chain. In parallel as a research direction, RegExp bookkeeping and
Earley's arguments/closure lifetime are concrete runtime targets. Splay needs
allocation/property initialization and GC work as well as generated-code work.

This profile selects where to investigate. It does not prove that a particular
optimization is correct, profitable, or the best possible transformation.
Generated-code percentages include necessary application work. Inlining more
code without eliminating redundant operations is not itself an acceptance gate.
Any implementation needs generated-code evidence and repeated uninstrumented
workload comparisons, including startup and unrelated workloads.

The diagnostic build's Earley/Boyer GC shares differ by several percentage
points from production, reinforcing the separation of production attribution
and diagnostic function mapping. Measurements cover extended steady-state
windows on macOS 27.0 build 26A428, Clang 21.1.8, ARM64, release/LTO with the
committed PGO profile. They do not measure an OS regression or V8 performance.
Instruments was unavailable because only Command Line Tools are selected;
LLDB was not used. No hardware performance counters were collected.

## Validation

- All 30 accepted benchmark processes and sampler processes exited successfully.
- Output checks reject benchmark errors, missing results and NaN results.
- Sample-tree counts reconcile exactly with each main-thread sample total.
- Binary, PGO and fixture hashes were checked; resumed completed fixtures match
  the initial manifest. The failed Decrypt pilot is preserved separately.
- Production and diagnostic executables, the PGO file and reproducible scripts
  are copied into the persistent artifact directory.
- No optimization, main-checkout runtime change, PGO regeneration or commit.

