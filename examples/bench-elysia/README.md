# Elysia performance fixtures

These fixtures isolate the request-path work used by the Elysia throughput
plan. They are checked in with their lockfiles and exact framework versions:

- `elysia1`: Elysia 1.4.28
- `elysia2`: Elysia 2.0.0-exp.38

The no-server benchmarks default to 50,000 iterations after 2,000 warmup
iterations. Correctness checks run after the timed loop. The stage benchmark
also has a `compact string response + headers` case so lazy-header changes are
measured both before and after materialization.

Run the focused fixtures with a pinned binary:

```sh
/tmp/ant-elysia-base-aebd47e4-nopgo examples/bench-elysia/elysia1/bench-no-server.ts
/tmp/ant-elysia-base-aebd47e4-nopgo examples/bench-elysia/elysia2/bench-no-server.ts
/tmp/ant-elysia-base-aebd47e4-nopgo examples/bench-elysia/elysia2/bench-stages.ts
/tmp/ant-elysia-base-aebd47e4-nopgo examples/bench-elysia/bench-string-intrinsic.js
```

Do not compare results across different PGO profiles. For optimization
attribution, use serial AB/BA runs of pinned base and candidate binaries built
with the same no-PGO release configuration. Final throughput claims require
revision-matched PGO profiles for both sides.

Pass `--stats` after the iteration count to the Elysia no-server fixtures to
print Ant's live object, sidecar, and exotic-object populations. Hono has the
matching opt-in fixture at `examples/npm/hono/bench-no-server.js`.
