# Node–Ant Differential Runner

This tool generates deterministic JavaScript probes, runs each probe under
Node and Ant, and compares the complete process result: exit status, signal,
timeout state, stdout, and stderr. Probe output uses a canonical JSON encoding
for values that JSON normally loses, including `undefined`, non-finite numbers,
BigInts, errors, functions, and cycles.

The initial families cover property semantics, RegExp behavior, Promise and
microtask timing, and readable/writable stream property shape. Stream probes
are atomic: each generated case checks one property or one target's own keys,
so a broad mismatch does not contaminate unrelated generated cases.

Run one deterministic pass across every family:

```sh
node tests/differential/runner.mjs
```

Generate more cases for selected families and minimize mismatches:

```sh
node tests/differential/runner.mjs \
  --family property \
  --family regexp \
  --seed 42 \
  --cases 20 \
  --minimize
```

Minimized standalone `.cjs` repros are written to `differential-output/` by
default. Use `--output` to choose another location and `--json` for
newline-delimited machine-readable output.

The runner exits 0 when all probes match, 1 when it finds a mismatch, and 2
for invalid configuration or a missing executable. A mismatch is a finding,
not necessarily an Ant bug: Node version differences and intentionally
unsupported surfaces still require classification.

Run its Node-side unit checks with `node tests/differential/test.mjs`.
