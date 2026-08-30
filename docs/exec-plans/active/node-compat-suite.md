# Vendored Node.js compatibility suite

Status: active
Last reviewed: 2026-08-29
Owner: theMackabu

## Goal

Run a pinned external Node.js compatibility corpus directly under Ant, with
reproducible provenance and explicit expectations instead of copying failures
into Ant-owned tests.

## Scope

- Bun's portability-adjusted copy of Node's `parallel` tests.
- The matching `common` helpers and fixtures required by those tests.
- A Node-hosted process runner with selection, concurrency, timeouts, output
  capture, expectations, and JSON reports.

Node's `sequential`, native-addon, V8-internal, pseudo-TTY, pummel, and other
specialized groups remain out of scope until the parallel corpus has a useful
baseline and their scheduling or build contracts can be represented honestly.

## Decisions

- Pin Bun commit `ed950b88ab2ec6b58bccdfe7d310731b8ca13c4d`.
- Preserve the imported files rather than patching them for Ant.
- Treat success as process exit 0, matching the upstream direct-execution
  contract.
- Pass upstream `// Flags:` directives through to Ant and record unsupported
  CLI behavior as a result rather than silently discarding it.
- Enter through an Ant-owned bootstrap that supplies Node harness metadata such
  as `process.config`; do not patch the imported `common` implementation.
- Keep generated reports outside version control.

## Tasks

1. [x] Inspect and pin the portable upstream subtree.
2. [x] Vendor `parallel`, `common`, fixtures, licenses, and provenance.
3. [x] Add the Ant runner and expectations format.
4. [x] Run a focused initial corpus and classify harness blockers.
5. [ ] Establish a reviewed baseline before enabling broad CI coverage.

## Initial results

- The first direct run exposed two harness prerequisites: the imported `.js`
  files need CommonJS package scope, and Node's shared harness expects the
  build-artifact property `process.config`. Both are now supplied outside the
  imported files.
- A 12-test `node:path` selection reached real assertions: 2 passed and 10
  failed on behavior including Win32 paths, `extname('..')`, module identity,
  and missing `matchesGlob()` / `toNamespacedPath()`.
- A sorted 100-test smoke selection produced 19 passes, 79 ordinary failures,
  one crash, and two timeouts. Results are intentionally not copied into the
  expectations file until each family is classified; the current empty
  baseline keeps every failure visible.
- The complete 3,236-test parallel corpus produced 1,410 passes (43.57%),
  1,439 ordinary failures, 383 timeouts, and 4 crashes. All 330 tests carrying
  upstream Node CLI flags failed before execution because Ant does not accept
  those flags; excluding that harness/CLI bucket, the observed pass rate is
  1,410 / 2,906 (48.52%). Strong large families include HTTPS (56/59), Web
  Crypto (36/38), crypto (112/120), TLS (173/186), HTTP/2 (235/256), and
  diagnostics_channel (32/35). Weak large families include dgram (0/75), VM
  (3/95), cluster (3/80), DNS (1/24), Buffer (7/63), HTTP (49/386, including
  244 timeouts), and path (2/15).

## Validation status

- `node tests/node-compat/test.mjs`: passed.
- JSON report generation, concurrency, upstream flags, timeout handling, and
  crash classification were exercised by the initial samples.
- A full-run attempt exposed orphaned long-lived fixture subprocesses after
  timeout. The runner now gives each test a Unix process group and terminates
  that group on timeout or completion; the invalid partial run was discarded.
