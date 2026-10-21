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
  imports resolve into the subproject, and a plugin replaces `src/workers.ts`
  with each worker bundled whole and started from a `data:` URL, as upm's own
  build does. npm's `dist/` is minified and its git dependency ships no
  `dist/`, so neither can carry patches. Generic hooks are offered upstream so
  patches can be dropped release by release.
- libpkg's behaviour is kept, without its bugs: registry fallback, `land:`
  and `npm:` prefixes, `.npmrc` scope mapping, Ant's directories and message
  prefix are upm patches; commands upm does not have (trust and lifecycle
  scripts, global installs, `why`, `info`, `ls`, `cache`, `config`, `init`,
  `create`) live in Ant's own `src/pkg/`.
- Commands backed by JS start the normal runtime and evaluate an ESM entry
  that calls `main(argv)` from `ant:internal/pkg`.
- upm's command line is the UI: install, add, remove, update, run and exec go
  through upm's `main`, so its progress bar, `✓` summaries and `ant:` notes are
  what users see. A patch exports upm's `note`/`fail`/`paint` and `setUsage`;
  Ant's own commands (trust, why, info, ls, cache, config, init, create,
  global installs, lifecycle notices) print in the same voice, and the usage
  page lists Ant's commands, sources, config and paths. `src/main.c` only
  routes: argv building, the `antx` rewrite, bare-script detection and the
  environment handed to JS live in `src/cli/pkg.c`, and the command table is
  the X-macro list `include/cli/commands.h`.
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
- `ant run` and `ant x` startup cost if they load all of upm.

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
- Seven upm patches: Ant directories, message prefix, a foreign `readSource`
  split, layout names (`ant.lock`, `node_modules/.ant`), registry fallback,
  a fallback mirror for tarballs, and CLI hooks. The mirror retries a
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
- Runtime fixes made along the way: fs `writeFile` flag/mode and
  `copyFileSync` mode, `util.styleText` stream colour detection, and Map/Set
  iterators that survive mutation.

Open:

- Warm-install gap: upm hardlinks per file, libpkg symlinked per package
  (phase 5).
- Hyperfine and 100-install reliability gates; Linux, musl and Windows runs.
- Offer the generic patches (fallback registry, foreign `readSource`, store
  mirror, CLI hooks) upstream.
