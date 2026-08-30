# Node.js compatibility suite

Run a focused group first:

```sh
node tests/node-compat/runner.mjs --match 'test-path-' --jobs 4
```

Run the complete parallel corpus and save a machine-readable report:

```sh
node tests/node-compat/runner.mjs \
  --jobs 8 \
  --timeout 10000 \
  --json node-compat-output/report.json
```

The runner preserves upstream `// Flags:` directives, isolates every test in a
fresh Ant process, caps captured output, and classifies pass, failure, crash,
timeout, and spawn errors. Use `--verbose` to print captured output for
unexpected results and `--list` to inspect a selection without executing it.
On Unix, every test owns a process group so timeout and completion cleanup also
terminate fixture subprocesses that would otherwise outlive a broken test.

Tests enter through `bootstrap.cjs`. It supplies the minimal `process.config`
shape expected by Node's shared harness when the runtime does not expose that
Node build artifact, then restores the target test as `process.argv[1]`. Keep
this compatibility layer runtime-neutral; behavior under test belongs in Ant.

## Expectations

`expectations.json` contains ordered glob patterns. An entry is either an
expected failure or a skip and must explain why:

```json
{
  "pattern": "parallel/test-example-*.js",
  "expectation": "fail",
  "reason": "node:example is not implemented"
}
```

Keep expectations narrow. Unexpected passes intentionally fail the run so a
stale expected failure is removed when Ant gains the behavior.
