Node-Ant differential fuzzer. Node is the oracle; a mismatch is a finding to
classify, not automatically an Ant bug.

Run (npm install once, for fast-check):
  node tests/differential/runner.mjs --family NAME --cases N --seed S --minimize
  node tests/differential/runner.mjs --list-families

--minimize shrinks the first mismatch per family and writes a standalone
repro to differential-output/<family>-seed<S>.cjs. Exit 0 = match, 1 = mismatch.

Check a new or changed family is not flaky by running Node against itself:
  node tests/differential/runner.mjs --ant "$(command -v node)" --family NAME --cases 20

Add a family: an object with name, a fast-check arbitrary, build(params)
returning probe source, and optional Ant variants; register it in
families/index.mjs. Probes record [label, value] pairs with __record /
__recordName (prelude.mjs). Ant-only paths (Ant.serve) record what a
reference expects under Node and what Ant does under Ant.

families/seeds.mjs is the old tests/fuzz corpus, decoded.
