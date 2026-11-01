# Node–Ant Differential Runner

This tool generates JavaScript probes with
[fast-check](https://fast-check.dev), runs each probe under Node and Ant, and
compares the complete process result: exit status, signal, timeout state,
stdout, and stderr. It is TypeScript run directly by Node (22.18 or later
strips the types); install its dev dependencies once with `npm install`.

Probes record `[label, value]` pairs. Values JSON loses (`undefined`,
non-finite numbers, `-0`, BigInts, symbols, functions, errors, cycles) are
encoded as `$`-prefixed strings. The encoder runs hot, and it avoids object
literals so that JIT bugs show up in the `jit` family rather than in every
family's bookkeeping.

## Families

- `property`, `regexp`, `promise`, `stream-shape`, `path`
  (`families/core.ts`): property semantics, RegExp execution and replacement,
  Promise and microtask ordering, stream property shape, and POSIX and Win32
  path resolution. Stream cases are atomic, one property or one target's own
  keys each, so a broad mismatch does not contaminate unrelated cases.
- `json`, `number`, `string`, `structured-clone`, `textcodec`, `url`,
  `buffer-base64` (`families/oracles.ts`): random inputs for built-ins with
  Node as the oracle. They record only error names, since messages are
  engine-specific.
- `jit` (`families/jit.ts`): one to four arithmetic and control-flow kernels
  per probe, each called 3000 times to reach the template JIT and the
  optimizing tier, then with boundary and mistyped arguments that force
  bailouts. Shrinking drops the kernels, statements and calls a mismatch
  does not need. Each probe runs under five Ant variants (default,
  `--jitless`, `ANT_DEBUG=opt:off`, `opt:force,verify`, `opt:verify`) and
  every variant must match Node. A mismatch only the default and `opt:*`
  variants show points at the optimizing tier. These variants trace to
  stderr, so only stderr lines naming a codegen, verifier, or operand depth
  failure count.

## Running

One case per family:

```sh
node tests/differential/runner.ts
```

More cases for selected families, shrinking each mismatch:

```sh
node tests/differential/runner.ts --family jit --family string --cases 50 --seed 42 --minimize
```

The runner reports the first mismatch per family, with the labels that
differ. With `--minimize`, fast-check shrinks it to a minimal case (fewer
records, shorter strings, smaller kernel expressions) and the standalone
`.cjs` repro goes to `differential-output/<family>-seed<N>.cjs` (`--output`
picks another directory). The same `--seed` and `--cases` reproduce a run;
`--json` prints one record per family.

The runner exits 0 when every family matches, 1 on a mismatch, and 2 for
invalid configuration or a missing executable. A mismatch is a finding, not
necessarily an Ant bug: Node version differences and intentionally
unsupported surfaces still need classifying.

## Changing the runner

Add a family by defining a fast-check arbitrary for its params and a `build`
that turns them into a probe (see `families/family.ts`), then register it in
`families/index.ts`. Check the tool with:

```sh
node tests/differential/runner.test.ts
npx tsc -p tests/differential
```
