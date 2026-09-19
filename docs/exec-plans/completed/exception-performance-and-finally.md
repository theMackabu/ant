# Exception performance and nested finally completions

Status: completed
Last reviewed: 2026-09-18
Owner: theMackabu

## Outcome

Fixed the existing loss of pending return/throw/jump completions inside nested
finally blocks and merged stash `4165fdd20ec9175066a8183814fefedc3579a659`.
Native/JIT results remain one 64-bit `ant_value_t`. The native error-handoff test
now uses `size_t` for `stack_len`. Removed branch-added trailing whitespace from
353 lines in 55 files; most of the wide touched-file count is mechanical.

Each active finally handler owns the completion it must resume. Entering it
clears the frame slot for nested execution; an inner catch no longer erases the
outer return. Abrupt replacement still discards the handlers it exits. GC traces
handler values in live VMs and suspended activations. The native regression also
checks a new pending return after the generator has survived earlier collections.

## Stash integration

Restored ordered upvalue cleanup, ordered rebasing/activation installation,
native/JS tests, the Meson target, and historical profile documentation. The
stash and index are preserved. Filesystem fixes already covered by central
normalization retain their current form; duplicate unwrap-before-reject calls
were not restored. The missing-callback path consumes the dropped error. The
early async-boundary plan restored from the stash is marked superseded.

ABBA on a checked, fixed-work probe with 30,000 short interpreted calls measured
capture-closing cost with the following outer capture counts. These are return
cleanup measurements, not measurements of collector mark/sweep time.

| Outer captures | Before stash ms | After stash ms |
| --- | ---: | ---: |
| 0 | 2.318 | 2.347 |
| 128 | 11.298 | 2.120 |
| 512 | 37.158 | 2.326 |
| 2048 | 131.649 | 2.345 |

## Performance findings and retained changes

- Regex literal guards had switched to non-invoking own-data reads for correctness.
  Each read repeated descriptor and value lookup. `js_try_get_own_data_prop` now
  checks a shape descriptor and reads its value in one lookup, retaining proxy,
  accessor, and exotic fallbacks. The isolated change reduced the three literal
  regex timings by 13–19%; native-call controls stayed within 0.5%.
- Ordinary native calls still call the native function directly. The new model
  checks existing pending state, the return completion, and any failure ignored
  by the native function. A direct comparison against undefined replaces general
  classification of the constrained pending handle and recovers about 2–3% in
  isolated native-call controls. Bootstrap initializes the handle before allocation.
- The original Map slowdown reproduced at 4.73% in a ten-times-longer run.
  Normalized MIR has the same loop body; cleanup paths differ, including removal
  of a redundant close after normal exhaustion. Native state access used general
  object decoding on every entry. An ordinary Map iterator now reads its primary
  native slot directly, retaining the general fallback for other layouts. The
  isolated improvement was 10.08%; after fresh PGO, Map is flat versus installed.
- Array push/pop source bodies are byte-for-byte identical between the installed
  revision and the final tree. Generated C code differs, including stack-protector
  code in the installed binary. Array-pop self-sample share grew while dispatch
  share stayed similar; the entire gap cannot be assigned to exception checks.
- Async control profiles are dominated by VM execution, coroutine resume, field
  access and microtask dispatch. The final async-next control is flat; the
  fulfilled-promise control retains a 4.42% gap. Ordinary successful calls do not
  allocate exception records.
- Broad error-tag and narrow pending-tag-bit experiments were rejected. Their
  benefits were small or mixed and some V8 rows declined. They also showed timing
  movement without adding loop work; no layout-specific tuning is retained.

## Fresh-profile qualification

The final main binary was trained with the repository workload selection using
a separate profile. The optional `pgo_profile` Meson setting selects that profile
without replacing the user-regenerated platform profile. Vendor compile flags
remain unchanged and no vendor compile command consumes the profile. The default
profile selection is unchanged when the option is empty.

Compared pinned installed `59a2d6b3` with the final working tree based on
`5903d763`: 104 serial processes in ABBA/BAAB order, four per binary per workload.
The original installed compiler/profile/configuration is not fully known; these
are whole-binary measurements, not source-only attribution of every small delta.

| bench-v8 case | Installed score | Final score | Score change |
| --- | ---: | ---: | ---: |
| richards | 6454.97 | 6444.38 | -0.16% |
| deltablue | 6387.00 | 6261.37 | -1.97% |
| crypto | 13459.13 | 13428.22 | -0.23% |
| raytrace | 12093.74 | 11951.97 | -1.17% |
| earley-boyer | 12037.00 | 12062.86 | +0.21% |
| regexp | 7396.12 | 7403.45 | +0.10% |
| splay | 5940.48 | 7456.43 | +25.52% |
| navier-stokes | 24337.60 | 24325.44 | -0.05% |
| Geometric mean | 9818.58 | 10060.00 | +2.46% |

| Timing | Final vs installed |
| --- | ---: |
| regex: regexp_ascii | -4.66% |
| regex: regexp_utf16 | -3.31% |
| regex: regexp_replace | -3.67% |
| native: math_min | +6.95% |
| native: array_push | +8.48% |
| native: array_pop | +9.78% |
| iteration-long: for..of map.values() | -0.27% |
| life: tick_ms | -1.55% |
| life: render_ms | -1.04% |

Negative timing change means faster. The native microbenchmarks remain 7–10%
slower; DeltaBlue and RayTrace retain smaller gaps. This is not a claim of
performance equivalence. Richards, Crypto, RegExp, and EarleyBoyer are effectively
flat in this sample. All requested per-case samples, ranges and explanations are
in the local report; no new aggregate is claimed for all 43 microbench rows.

## Validation and evidence

- All 4240 specs in 102 files and 19 focused JS tests pass.
- Native error handoff/diagnostic/GC, desktop details, upvalue order, and exact
  GC-edge tests pass. The return ABI and object-union size assertions still pass.
- The stack-depth sweep had no operand-depth rejection or JIT stack overflow.
  It remains incomplete because `test_node_events_once_prototype_spoof.cjs`,
  `test_throw_stack.cjs`, and `test_with_strict.cjs` fail. All three also fail
  on the saved starting binary.
- The retained main binary matches the benchmarked qualified binary byte for byte
  after the normal codesign target runs.
- Binary SHA256: `4eb7fb93600adc46c10299f607c7cb8e664e0d3446ae47db9d21a79210e19242`.
- Separate profile SHA256: `7942fcdfa8021271d6570de7c441ae0fb8c3ff4aeb4674b2a85cc2d6bc43f8a1`.
- Original platform profile SHA256, preserved:
  `0f17b3a7a3b79e273cabf96342ca83630101f4d4881dc9ecdabfd3ffab300c1e`.
- Artifacts: `.cache/exception-investigation-5r45vu9m/`, including `report.md`,
  `final-results.csv`, profiles, source/assembly comparisons, binaries, profile
  identities, raw runs, validation logs, and the pre-stash working-tree snapshot.

A separate Splay ABBA resource check measured median peak RSS of 1.147 GiB installed versus 2.310 GiB final. This remains a higher peak; the time-based benchmark does unequal work, so no equal-work memory or leak claim is made.

Final audit: 49 tracked files are whitespace-only changes. The only modules with functional edits in this turn are `src/modules/fs.c` and `src/modules/collections.c`. Exact large-iteration result counts also pass under Node and the final binary. Preflight, knowledge checks, and the branch whitespace check pass.
