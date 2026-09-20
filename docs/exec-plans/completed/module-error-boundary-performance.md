# Module Error Boundary Performance

Status: completed
Last reviewed: 2026-09-18
Owner: theMackabu

## Comparison

Benchmark every changed module against the installed executable requested by
the user: `/Users/themackabu/.ant/bin/ant`, embedded revision `59a2d6b3`.
Candidate source is `238738bf`; all 34 changed files under `src/modules/`
are included in the coverage matrix. Related stream and promise controls are
included. This is a whole-binary comparison; profile differences prevent
attributing small deltas to the source patch alone.

Pinned binaries, fixture sources, build options, profile hashes, source diff,
individual samples, and analysis are under
`.cache/module-perf-20260918-111408/`.
The candidate was rebuilt from current source, but its configured embedded
version still says `34d08d0c`; provenance records both fields explicitly.

## Method

- Successful-operation workloads with exact checked outputs and fixed work.
- Fresh processes, untimed warmup, monotonic elapsed time, serial ABBA/BAAB
  blocks, rotating workload order. Child processes use the pinned parent binary.
- Calibrate workload lengths before measurements; repeat larger or noisy
  differences rather than infer a regression from one sample.
- Roughly 1% is inconclusive given PGO/build differences and host noise.
  Larger changes require repeatable evidence and source-backed interpretation.
- Track unavailable backend coverage explicitly. Sandbox lifecycle coverage
  does not imply guest VM performance coverage.

## Outcome and validation

All 34 changed modules were covered by 51 workloads. There were 1,328 valid
primary measured processes and 48 longer diagnostic processes. Accepted
baseline/candidate checksums match, and both pinned executable hashes were
verified unchanged after measurement. No runtime changes or PGO regeneration
were made for this task.

The assertion workload is the clear cost: `assert.doesNotReject` is +170.07%
(about 2.70x). Its corrected implementation waits for promise settlement and
allocates continuation state; the old implementation only inspected the current
state. Filesystem callbacks are +5.75% in the primary comparison, +3.54% in
longer checks. Events.once is +3.61%, confirmed at +3.02%. Process spawning is
+3.35% (+6.44% in longer checks), with substantial variability. JSON replacer
arrays are +2.70%. Other sampled operations are mostly near baseline or faster.

The unchanged await control is +2.02% in the main matrix and +1.37% in the
longer check. Do not attribute roughly 1-2% differences to the patch or claim
that PGO is their proven cause. The installed baseline's full flags/profile
provenance is unknown, and desktop applications remained active.

One HTTP repeat failed with `address not available` after connection churn.
The incomplete pair was excluded; a cooled ABBA repeat completed. HTTP has
14 accepted samples per binary; other rows have 10 or 20. Sandbox coverage is
host construction/close using the same cached assets, without VM execution.
TLS coverage is context lifecycle, without handshakes. Cron covers parsing,
without OS scheduler registration. Error paths that behave differently on the
baseline are correctness coverage, not comparable performance samples.

The full report, iteration counts, elapsed-time medians, ranges, individual
samples, and reproducible fixtures are in the artifact directory's `README.md`.
Build, checksum, coverage, and repository preflight checks passed.

## Per-module results

Positive means longer elapsed time. Each delta is the median ratio of paired
ABBA/BAAB block means, with equal work within a pair.

| Module | Measured workload deltas |
| --- | --- |
| assert | assert-promises: +170.07% |
| blob | blob-bytes: +0.40% |
| buffer | buffer-from-iterator: -4.35%; buffer-from-arraylike: -2.43% |
| child_process | child-spawn: +3.35% |
| collections | collections-set: -4.44% |
| cron | cron-parse: -0.32% |
| crypto | crypto-pbkdf2: -0.86%; crypto-scrypt: -0.24% |
| dns | dns-lookup: +0.39% |
| events | events-once: +3.61% |
| fetch | fetch-data: -1.06%; server-http: +1.32% |
| ffi | ffi-callback: -0.68% |
| fs | fs-read-callback: +5.75%; fs-read-promise: +0.13% |
| generator | generator-sync: +0.10%; generator-async: -0.08% |
| iterator | iterator-map-data: -0.85%; iterator-map-getters: -2.90%; iterator-reduce-data: -11.15%; iterator-reduce-getters: -7.55% |
| json | json-stringify: -0.73%; json-replacer: +2.70% |
| navigator | locks-value: +0.20%; locks-promise: -1.82% |
| net | net-echo: -0.57% |
| observable | observable: -7.31% |
| process | process-stdin: -0.74% |
| request | request-body: +0.45% |
| response | response-body: -0.07% |
| rpc | rpc-call: -1.96% |
| sandbox | sandbox-lifecycle: -2.09% |
| server | server-http: +1.32% |
| stream | writable-size-number: -2.83%; writable-size-object: -3.50%; readable-size-number: +2.06%; readable-size-object: +1.45%; node-write-callback: -4.10%; stream-pipeline: -3.32% |
| symbol | symbol-iterator-close: -0.45% |
| timer | timer-immediate: +0.56% |
| tls | tls-context: -1.57% |
| tty | tty-write-callback: -0.75% |
| url | url-format: -3.15%; url-searchparams: -0.68% |
| util | util-promisify: -0.36%; util-parseargs: -6.97% |
| wasm | wasm-instantiate: +0.49% |
| worker_threads | worker-messages: +1.01% |
| zlib | zlib-callback: +1.76% |

## Longer diagnostic runs

These runs used four times the work and three additional ABBA/BAAB blocks;
they are not mixed with the shorter-run matrix.

| Workload | Delta | Block range |
| --- | ---: | ---: |
| Filesystem read callback | +3.54% | -0.69% to +5.17% |
| Events.once | +3.02% | +0.72% to +7.14% |
| Child spawn | +6.44% | +1.20% to +14.62% |
| Promise await control | +1.37% | +1.06% to +2.00% |
