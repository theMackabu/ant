# ARM64 Nightly Benchmarks

Status: active
Last reviewed: 2026-09-03
Owner: theMackabu

## Goal

Turn the dedicated macOS ARM64 host into a GitHub Actions self-hosted runner
that benchmarks Ant, Boa, V8 in jitless mode, SpiderMonkey in jitless mode,
Kiesel, LibJS, Duktape, QuickJS, and Porffor every day after cached builds and
updates at 00:20 America/Los_Angeles and a fixed five-minute cooldown, then
publishes durable, queryable results through a public API.

## Scope

- A cross-engine `bench-v8` driver under `tools/arm64-bench/`.
- Current-channel engine resolution, versioned caches, and explicit command
  adapters.
- A scheduled workflow routed only to the `arm64-bench` runner label.
- A Hono/Cloudflare Worker API backed by D1 under `docs/benchmarks/`.
- Host and runner provisioning documentation without checked-in credentials.

## Constraints

- The benchmark supervisor must not be one of the engines under test.
- Every engine/test/sample runs in a fresh process with the same generated
  benchmark source and a bounded timeout.
- Preserve raw samples. Published rollups use medians and report dispersion;
  unsupported tests and correctness failures are statuses, never zero scores.
- Resolve each comparator's declared upstream channel once before a run. Reuse
  immutable version-keyed installations when unchanged; install only a newly
  resolved release or commit. Record the channel, resolved version, source
  commit when the upstream artifact exposes one, retrieval time, executable
  hash, and reported version in the result.
- Rotate engine order between samples to reduce order and thermal bias.
- Configure and compile Ant through the repository's pinned `nix develop`
  environment with the checked-in Darwin ARM64 PGO profile and full LTO. Keep
  the incremental Meson tree, Meson package cache, and ccache outside the
  cleaned Actions checkout. Scheduled and manual runs use the same fixed
  five-minute cooldown after build and engine synchronization.
- The GitHub runner label is `arm64-bench`; hardware marketing names are not
  part of scheduling or public machine identity.
- The upload endpoint is authenticated. Read endpoints are public and CORS
  enabled. Tokens and runner registration material never enter the repository.
- The persistent runner must not expose personal files or credentials. For a
  public source repository, prefer attaching it to a private controller
  repository that checks out Ant's default branch.

## Result Contract

Schema version 1 contains a run identity, repository revision, suite/protocol
revision, UTC start/end timestamps, sanitized host metadata, and one entry per
engine. Each engine entry records its configured mode, executable identity,
flags, per-sample aggregate and per-test scores, median aggregate and per-test
scores, median absolute deviation, duration, and status.

The server derives the durable run ID from the runner key, repository revision,
suite revision, scheduled local date, and GitHub workflow execution (or local
invocation timestamp). Re-uploading the same document is idempotent; a
conflicting payload for that identity is rejected. Workflow reruns and manual
dispatches remain distinct measurements.

## Task List

- [x] Define and test the cross-engine result schema.
- [x] Implement source generation, adapters, timeouts, rotating sample order,
      aggregation, metadata capture, and local JSON output.
- [x] Add current-channel resolution, versioned engine caches, and reproducible
      host provisioning.
- [x] Implement authenticated create and public latest/history/detail/series
      API routes with D1 migrations.
- [x] Add the 00:20 America/Los_Angeles GitHub Actions workflow, fixed cooldown,
      and manual dispatch inputs.
- [x] Document runner installation, labels, power settings, secrets,
      deployment, recovery, and update cadence.
- [x] Run unit tests, API type checking, workflow validation, `maid preflight`,
      and the validation router's recommended checks.

## Decisions

- 2026-08-29: Use a custom `arm64-bench` label in addition to GitHub's default
  `self-hosted`, `macOS`, and `ARM64` labels.
- 2026-08-29: Start the cached build at 00:20 in `America/Los_Angeles` and gate
  measurement until 00:40, using GitHub's timezone-aware schedule syntax.
  Superseded by the fixed cooldown below.
- 2026-08-29: Keep the Ant Meson tree, Meson package cache, and ccache under the
  engine root so checkout cleanup does not discard nightly build work.
- 2026-08-29: Build Ant inside `nix develop` so the benchmark uses the pinned
  Nix compiler/linker/archive toolchain, Meson's native ARM64 tuning, the
  checked-in PGO profile, and full LTO without a host-specific archive patch.
- 2026-08-29: Keep V8 and SpiderMonkey in explicit jitless modes. Fully enabled
  optimizing engines are a separate future comparison cohort.
- 2026-08-29: Use Node as the neutral supervisor and the existing bench-v8
  source files as the only timed JavaScript workload.
- 2026-08-29: Use a dedicated Worker/D1 application instead of mixing durable
  benchmark history into the crash-report or release-manifest services.
- 2026-08-29: Run the complete current-engine cohort nightly. Resolve upstream
  versions first, reuse a version-keyed cache when unchanged, and install only
  on a new release or commit. Treat each version change as a time-series
  boundary rather than an Ant regression.
- 2026-08-29: Add Porffor to the cohort while retaining LibJS. Follow V8 Canary,
  SpiderMonkey development releases, Kiesel and LibJS main snapshots, and the
  latest releases of Boa, Duktape, QuickJS, and Porffor.
- 2026-08-29: Keep the 00:40 Pacific measurement gate exclusive to scheduled
  runs. Manual dispatches use a fixed five-minute cooldown so an operator can
  run the workflow at any time without waiting for the next clock gate.
  Superseded by the fixed cooldown below.
- 2026-09-03: Replace the scheduled clock gate and load-based extension with
  the same fixed five-minute cooldown for every run, regardless of when it
  starts.

## Validation Status

- Repository architecture, testing guidance, workflow conventions, current
  bench-v8 harness, and existing Worker applications reviewed.
- The pinned Nix development shell produced a release Ant binary with the
  checked-in Darwin ARM64 PGO profile and full LTO. An immediate unchanged
  compile completed in 2.96 seconds using the persistent Meson and ccache
  state.
- The current-channel resolver seeded all eight comparator caches, verified
  ARM64 Mach-O executables and hashes, and reused an unchanged set in 13.76
  seconds. A LibJS main advance reused its incremental source/vcpkg/Ninja tree
  and required no compilation work because the shell target was unchanged.
- A strict one-sample end-to-end run completed all eight bench-v8 programs on
  all nine engines and produced an API-valid result document.
- Runner unit tests passed 7/7. Worker schema tests passed 4/4 against the real
  result fixture; TypeScript type checking, local D1 migrations, and a
  Wrangler deployment dry run passed.
- ShellCheck, Actionlint, `nix flake check --no-build`, `git diff --check`, and
  the repository knowledge and structure checks passed. `maid` is not
  installed on this host, so the configured preflight command was run directly
  with the freshly built benchmark Ant binary. The validation router's extra
  Node-compat recommendations belong to unrelated pre-existing worktree
  changes.
- Deployment remains an operator step: register the installed Actions runner
  with a short-lived GitHub token, create/configure the Cloudflare D1 database
  and publish secret, deploy the Worker, and apply the password-protected
  `pmset ... autorestart 1` setting.

## Follow-ups

- Add an HTML trend dashboard after the ingestion and history APIs have
  accumulated enough valid samples.
- Consider a separate optimizing-engine cohort only after the interpreter and
  jitless protocol is stable.
