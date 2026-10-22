# Replace libpkg with upm

Status: active
Last reviewed: 2026-10-21
Owner: theMackabu

## Goal

Ant's package manager is JavaScript instead of Zig. `src/pkg/` (libpkg, about
14k lines of Zig built into `libpkg.a`) and the Zig toolchain leave the build.
The resolver, registry client, tarball pipeline and store come from
[unjs/upm](https://github.com/unjs/upm) (MIT), vendored and bundled as the
builtin `ant:internal/pkg`. Ant owns the command layer, its defaults, and the
features upm does not have.

The motivation is extensibility (contributors can change the package manager
in JS) and removing the Zig toolchain, which has repeatedly broken builds
(Aro cImport against new macOS SDKs, `libpkg.a` against Apple ld).

## Evidence (2026-10-09 to 2026-10-20)

Running stock upm on Ant surfaced and fixed runtime gaps:
`process.getBuiltinModule`, `fs.link`/`opendir`, `Buffer` `base64url`,
`digest('latin1')`, dynamic `import()` default for C builtins, worker
`data:` URLs and `postMessage`, `Readable[Symbol.asyncIterator]`,
`monitorEventLoopDelay`, a JIT crash on multiple `return await` sites,
`fetch` connection pooling, typed-array element fast paths, a connect and
handshake timeout, and a sequential happy-eyeballs connector.

Measurements on macOS arm64 (`large` is 672 packages, `medium` 141):

| Case | libpkg | upm on Ant | upm on Node |
|---|---|---|---|
| medium cold | 5.1 s | 5.0 s | 4.2 s |
| large cold | 12.4 s | 9–12 s | 8.4–9.1 s |
| medium warm | 277 ms | 799 ms | 584 ms |
| large warm | 3.58 s | 3.22 s | 3.33 s |
| medium no-op | 14.7 ms | 20.9 ms | 32.4 ms |
| large no-op | 141 ms | 28.8 ms | 42.1 ms |

The medium warm gap is layout: libpkg symlinks one directory per package into
its cache, upm hardlinks every file into `node_modules/.upm`.

## Decisions

- upm is an upstream dependency, not a copy: `vendor/upm.wrap` fetches the
  release tag, and Ant's changes are readable patches against upm's
  TypeScript source in `vendor/packagefiles/patches/upm-*.patch`
  (`diff_files`, as for tlsuv). `vendor/packagefiles/upm/meson.build` exports
  the source directory and file list, which the builtins step depends on and
  passes to `src/tools/gen_builtins.js` as `ANT_UPM_SRC`. There, `upm/...`
  imports resolve into the subproject (`upm-sources`), and
  `upm-inline-workers` replaces `src/workers.ts` with each worker bundled whole
  and started from a `data:` URL, as upm's own build does. Builtins bundle at
  es2022 (workers use top-level await) from the source root, so module keys
  are repository-relative and no absolute path lands in the binary.
  Patches carry no comments of ours. npm's `dist/` is minified and its git dependency ships no
  `dist/`, so neither can carry patches. Generic hooks are offered upstream so
  patches can be dropped release by release.
- libpkg's behaviour is kept, without its bugs: registry fallback (with a
  24-hour miss cache), `land:` and `npm:` prefixes, `.npmrc` scope mapping and
  Ant's directories are upm patches or hooks. A prefix applies to its package
  only: a scoped package gets an `.npmrc` scope line, an unscoped one is saved
  as its tarball url, and the rest of the tree keeps the default registry and
  its fallback (`src/pkg/land.ts`). A prefix used to switch the registry for
  the whole install, so `ant add npm:foo` dropped ants.land for every package.
  The fallback no longer remembers misses for names already in the lockfile
  (a 404 from an expired token is asked again next run, not routed to npm for
  a day), and a remembered miss expires after 24 hours in every mode but
  `--offline`; locked installs stay protected by integrity checks. commands upm does not have (trust and lifecycle
  scripts, global installs, `why`, `info`, `ls`, `cache`, `config`, `init`,
  `create`) live in Ant's own `src/pkg/`.
- Commands backed by JS start the normal runtime and evaluate an ESM entry
  that calls `main(argv)` from `ant:internal/pkg`. That entry is a small
  bundle (`src/pkg/quick-run.ts`, 42 KB): for `ant run <script>` it repeats
  upm's no-op install check (lock, manifest and settings stamps, workspace
  proof, local tarball and `link:` stamps, `treeStanding`) and, when the tree
  is fresh, runs the script through upm's `run.ts` without loading the rest.
  Anything else, or any doubt, imports the full bundle
  `ant:internal/pkg_main`. esbuild code splitting was tried first and
  dropped: it splits per file, and `cli.ts` statically reaches all of upm, so
  the entry still loaded 263 KB.
- upm's command line is the UI: install, add, remove, update, run and exec go
  through upm's `main`, so its progress bar, `✓` summaries and `ant:` notes are
  what users see. A patch exports upm's `note`/`fail`/`paint` and `setUsage`;
  Ant's own commands (trust, why, info, ls, cache, config, init, create,
  global installs, lifecycle notices) print in the same voice, and the usage
  page lists Ant's commands, sources, config and paths. Argv building, the
  `antx` rewrite, bare-script detection and the environment handed to JS live
  in `src/cli/pkg.c`; the command table (`PKG_COMMAND`/`NATIVE_COMMAND`
  entries) and routing live in `src/cli/commands.c` (an X-macro header was
  tried and dropped: clangd flags its include as unused).
- Lockfile: read `ant.lockb` (v4, spec in the removed `src/pkg/lockfile.zig`)
  for frozen installs and migration; write upm's JSON format as `ant.lock`.
- Store: `~/.cache/ant/store` (or `$XDG_CACHE_HOME/ant/store`). The old LMDB
  cache is not imported.
- Dependency lifecycle scripts stay opt-in through `ant trust` and
  `trustedDependencies`.
- Registry fallback (`ants.land` first, npm for misses) is a resolver patch,
  not a custom `fetch`, which would disable upm's worker pools.

## Phases

1. Vendor and bundle upm as `ant:internal/pkg`.
2. Route `install`, `add`, `remove`, `update`, `run`, `exec`/`x`, `antx`
   and bare `ant <script>` into JS while the other commands keep libpkg.
3. Ant defaults and the `ant.lockb` reader.
4. Port `trust`, global installs, `why`, `info`, `ls`, `cache`, `config`,
   `init`, `create`, and the registry fallback.
5. Close the warm-install gap (directory clone or symlink materialization).
6. Delete libpkg (`src/pkg/*.zig`, `include/pkg.h`) and Zig from meson, nix
   and CI.

## Gates before deleting libpkg

- Correctness: the three fixtures, workspaces, `--frozen-lockfile` against
  `ant.lockb` and the new lockfile, migration on first `add`, global installs,
  `trust`, registry fallback, offline install with a warm store.
- Performance (hyperfine): cold, warm and no-op at or below libpkg.
- Reliability: 100 consecutive large cold installs without a stall.
- Platforms: macOS, Linux glibc and musl, Windows.

## Risks

- Fork drift from upstream while patches are carried.
- Installs depend on Ant's Node compatibility; CI must run real installs.
- Windows links, junctions and long paths.
- Package command startup. Measured 2026-10-21 (hyperfine, M-series):
  `ant -e 0` 3 ms; `ant run` on a fresh tree 15 ms (was 29 ms before the
  light entry; 28 ms when it falls back), `ant x --help` 20 ms, `ant ls`
  25 ms. Parsing and compiling the 424 KB full bundle is about 12 ms of a
  full-path command. Of the light `run`'s 15 ms, about 6 ms is spawning the
  shell and about 6 ms is one JIT compile (see action items); under
  `--jitless` it is 9.6 ms.

## Checkpoint

Branch `feat/upm-package-manager`, 2026-10-21. Phases 1 to 4 and 6 are done;
phase 5 and the performance, reliability and platform gates are open.

- Every package command except `login` and `publish` runs in
  `ant:internal/pkg`. The new lockfile is `ant.lock`; `ant.lockb` migrates on
  the first install, add, remove or update. Edges are resolved Node-style from
  each package's install path, because libpkg's edges point at the hoisted
  copy. On the three fixtures, the migrated lock matches libpkg's tree 71/71,
  140/140 and 709/710; the extra package is an optional peer that libpkg
  skipped.
- Seven upm patches, regenerated without comments and checked by replaying
  them onto a fresh `meson subprojects download upm`: a home hook
  (`setHome`), a foreign `readSource` split, layout names (`ant.lock`,
  `node_modules/.ant` via `ENTRIES_DIR`), registry fallback with a miss cache,
  a fallback mirror for tarballs, CLI hooks (program name and colour, usage,
  script banner) and shared leaves. The message-prefix patch is gone; the
  program name is a CLI hook. The mirror retries a
  404 from the land base under the npm base, because derived lockfile URLs
  drop explicit npm hosts. Every caller that misses on the base registry
  retries on the fallback; letting only the first one move a name made
  concurrent lookups fail with E404 on warm installs.
- Output follows upm's CLI rather than libpkg's; colours follow the stream
  (TTY, `NO_COLOR`, `FORCE_COLOR`, `--no-color`).
- `cache prune` is upm's prune (unreferenced store content), not an age in
  days.
- libpkg, Zig, the nghttp2 wrap and the Zig CI, nix, Docker and setup steps
  are removed; `build/ant` shrank from 10.70 MB to 10.16 MB. The Zig-era
  `src/pkg/` path now holds the TypeScript layer.
- Shared leaves: a registry package with no dependencies, no peers and no
  lifecycle scripts is filled once under the store's `shared/` directory and
  symlinked into each project entry; `.links` files record users so
  `cache prune` keeps live ones. Two kinds are kept in the tree instead:
  packages bound to a platform (`os`/`cpu`, or shipping `.node` files, like
  `@esbuild/*` and `@rollup/*`), and packages whose scripts import a package
  they do not declare (a static scan of `require`/`import` specifiers, minus
  Node builtins and their own devDependencies), since resolution from the
  shared directory cannot reach the project's hoisted packages. Paths outside
  the project are inherent; `ant config set install.sharedLeaves off` turns
  sharing off for tools that need every file inside the project. Warm
  link-only install of 72 packages (42 shared): 0.29 s, 0.55 s with sharing
  off.
- Lifecycle discovery and `why` resolve store entries through the lock
  entry's version, so `file:`, VCS and tarball dependencies with install
  scripts are found; the discovery cache is versioned so stale empty caches
  rescan. Global installs link bins by the global manifest's change, so
  `file:` and alias specs link too.
- Runtime fixes made along the way:
  - fs `writeFile` options: Node's full flag table (`as`, `xa+`, ...),
    `ERR_INVALID_ARG_VALUE` for unknown flags and bad modes (NaN, negative,
    non-octal), close errors reported; `copyFileSync` mode.
  - `util.styleText` stream colour detection shares `io_color_override()`
    with console output (`--no-color`, sandbox, `FORCE_COLOR`, `NO_COLOR`).
  - Map/Set iterators survive mutation. Iterators parked at the same entry
    share one cursor; an entry carries a flag instead of a pointer, so entry
    size is unchanged. Delete, add-at-end and clear move or merge cursors in
    O(1) amortized, without allocating. Each heap iterator embeds its own
    cursor, used while it is alone at its position; the iterator's block
    lives until both the iterator is collected and no other cursor forwards
    into it. Stack iterators (`forEach`, set methods, inspect) take pooled
    heap cursors, since stack memory cannot be a forward target. A first
    registry design walked every live iterator on delete and made an LRU that
    leaks `keys().next()` iterators 57x slower.
  - Set methods follow the spec (`difference` keeps a copy of `this`),
    clone entries without rehashing, and use direct lookups when `other` is a
    Set with the builtin `has`.
  - Collections speedups over master, all measured against master's unsafe
    iteration: entry pairs are one dense two-slot array; `forEach` uses
    prepared callbacks and accepts any callable (TypeError otherwise); entries,
    uthash tables, cursors and iterator states come from a thread-local
    16-byte size-class pool (cap 128 per class); uthash starts at 8 buckets
    and expands at chain length 6 (threshold 2 trips uthash's
    inefficient-expansion stop and was 5x slower; 10 kept long chains); the
    for-of slot holds the iterator state pointer, so a step never decodes the
    iterator object; JIT Map/Set steps skip the GC root scope. Cycles vs
    master (no-PGO, 7 rounds): clear -37%, churn -36%, churn with live
    iterators -40%, set intersection -33%, difference -36%, big numeric map
    -29%, forEach -23%, create -11%, big string set -12%, Map entries
    iteration -11%, spread -4%, LRU -3%; Set/keys iteration, short iterators
    and small lookups within +-1% (15% fewer instructions, bound by load
    latency over linked entries). Bench: `.cache/collbench-20261021`
    (`run.py`, `bench.js`).

Open:

- Warm-install gap for packages that cannot be shared leaves (phase 5).
- Hyperfine and 100-install reliability gates; Linux, musl and Windows runs.
- Offer the generic patches (fallback registry, foreign `readSource`, store
  mirror, CLI hooks) upstream.

Action items:

- Lazy function compilation in Silver: parse a function body for errors and
  scopes only, and compile its bytecode on first call (V8's pre-parser
  model). Every full-path package command spends about 12 ms parsing and
  compiling a 424 KB bundle that mostly never runs; any large script or
  builtin pays the same way. Alternative or complement: cached bytecode for
  builtins at build time.
- OSR tiering counts loop back-edges across calls. `node:path`'s
  `normalizeString` (603 B) crosses its 588 back-edge threshold after ~16
  short `path.join` calls and takes a 4.6 ms cold compile in the middle of
  startup (`ANT_DEBUG='dump/vm:op-warn'`: `jit: osr compiled
  func=normalizeString`). That is about 6 ms of `ant run`'s 15 ms. OSR should
  fire on a long-running activation, as V8's per-invocation budget does;
  short loops called often belong to the call-count tier.
