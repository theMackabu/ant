# Silver Header Boundaries

Status: completed
Last reviewed: 2026-09-07
Owner: theMackabu

## Goal

Separate VM definitions from inline call dispatch so async operations do not
form an include cycle with `engine.h`. Preserve the measured inline behavior
of async and TLA entry, VM layouts, and runtime semantics.

## Structure

- `include/silver/engine.h`: VM, function, closure, frame, and metadata layouts;
  basic accessors and runtime entry declarations. No opcode implementation or
  inline call-dispatch dependency.
- `include/silver/feedback.h`: type-feedback and JIT tiering helpers, depending
  on the engine definitions.
- `src/silver/ops/async.h`: async/TLA entry and await operations, depending on
  the engine definitions.
- `include/silver/call.h`: call context, preparation, dispatch, cleanup, and
  native invocation helpers, depending on engine, feedback, and async headers.

Each consumer includes the header for the operations it uses. Runtime modules
that call arbitrary JS need `call.h`; data-only VM consumers need `engine.h`.
Headers must compile independently and in either include order.

## Work

- [x] Extract the existing implementations without changing their behavior.
- [x] Update core, module, opcode, embedding, and Wasm consumers explicitly.
- [x] Check header independence and absence of the old include cycle.
- [x] Rebuild, run focused tests, spec/JIT suites, and repo preflight.
- [x] Compare call/async/construction performance with the saved baseline.

`engine.h` shrank from 1,951 to 935 lines. The extracted function bodies are
unchanged apart from whitespace. No VM layout or call ABI changed. Public
embedding consumers still include the generated `ant.h`; its generator emits
engine, feedback, async, and call headers in dependency order.

## Validation

Local validation artifacts are under `/tmp/ant-header-cleanup`; the initial
source and executable are in `before/`, and the candidate executable is in
`after/`.

- Configured native build passed using
  `nix develop --command bash -c 'meson compile -C build -j8'`.
- All four headers compile independently, and all 24 include permutations
  pass with implicit-function and undefined-inline diagnostics treated as
  errors. Including `engine.h` alone does not define the call, feedback, or
  async header guards.
- Focused async dead-await, TLA dead-await, async reentry, JS-entry TLA resume,
  entry for-await, dynamic-import TLA context, new-target frame, and WebSocket
  buffered-frame regressions passed.
- Full spec suite: 4,221 tests across 102 files passed. JIT suite: all 10 files
  passed. The async benchmark also passed with its default arguments.
- Repo-wide moved-symbol scan found explicit includes for every internal
  consumer. The embedding example and desktop window-state consumer passed
  syntax checks against the source headers.
- `maid preflight` and `git diff --check` passed.

Packaging validation has existing blockers:

- `npm --prefix packages/wasm test` fails the unchanged 32-bit map-template
  table size assertion in `engine.h`. This prevents a full Wasm validation;
  host compilation cannot substitute for that target. The recommended
  `npm pack --dry-run` was not repeated because its prepack hook invokes the
  same failing build.
- Both baseline and candidate generated embedding headers fail standalone
  compilation on missing prerequisite declarations, beginning with the cage
  allocation API and value type constants. The new header emission order is
  updated, but a full libant package build is not claimed.
- The optional CEF consumer check cannot compile because its existing
  `../../app/runtime/ant_runtime.h` include target is absent.

Meson reconfiguration was not needed: build definitions and options did not
change. The packaging script change only updates amalgamated header inputs.

## Performance

Keep the existing `static inline` behavior. The preceding inlining experiment
did not justify forcing either async entry function out of line.

The cleanup comparison uses the configured Clang 21.1.8 Darwin ARM64 build
with `-O3`, LTO, and its existing PGO profile on an Apple M5 Pro. Ten process
rounds alternate baseline/candidate order for each workload. Workloads cover
ordinary and native calls, async entry without await, dead await, actual
suspension, Headers/Response construction, and imports of 1,500 distinct
modules with suspending or dead TLA. No builds or CPU-heavy tests ran during
measurement. Raw samples and the runner are in `perf.json` and `bench.py`.

Timing medians fluctuate in both directions: direct calls -0.12%, no-await
entry -0.35%, dead-await entry +0.65%, suspension +1.22%; native-call cases
range from -2.15% to +2.66%, and construction cases from -4.01% to +0.76%.
The noisier TLA import cases measured +13.37% and -8.81%. These timings alone
do not establish a cleanup regression or a speedup.

The machine-code comparison is stronger evidence for this structural change:

- Both executables are 10,457,776 bytes. The text section is 7,619,992 bytes
  at the same address and file offset in both builds.
- Every changed ARM64 instruction is the same `mov` instruction with a
  diagnostic source-line immediate increased by one or two: 1,067 feed
  `__assert_rtn`, and 32 feed fatal-error reporting through `fprintf`.
- All other text bytes are identical. Instructions, branches, call targets,
  inlining, and layout are preserved. All initialized data sections, constant
  sections, stubs, and unwind metadata are byte-identical too; zero-filled
  sections retain their addresses and sizes.

This cleanup adds no generated execution overhead on the tested build. The
timing swings are consistent with environmental variation. This result does
not extend the earlier constructor-context migration's performance claims
or establish results for other compilers, profiles, or targets.
