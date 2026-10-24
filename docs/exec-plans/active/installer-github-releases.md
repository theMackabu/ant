# Installer GitHub Releases Source

Status: active
Last reviewed: 2026-10-24
Owner: pgray

## Goal

Let the `antjs.org/install` script and `ant upgrade` install Ant straight from
GitHub releases when `manifest.antjs.org` is unreachable, or when asked to.
The manifest used to be their only source, so a network that blocks antjs.org
subdomains could not install or upgrade Ant at all. One ISP intercepts TLS for
`manifest.antjs.org` and `npm.ants.land` (`curl: (35) ... wrong version
number`) while `github.com`, `api.github.com`, and the release asset host
work, which still reproduced on 2026-10-24.

## Starting point

- The installer is generated: `packages/installer/render-install.js` fills
  `install.template.sh` and `templates/*.template.sh` from `install.json` and
  writes the gitignored `packages/installer/dist/install`. Contributors render
  with `--no-deploy`, which skips `ant deploy` but still refreshes the
  gitignored `docs/private/landing/public/install`.
- The installer downloaded `https://manifest.antjs.org/v1/download/ant/{target}`
  (`ANT_DOWNLOAD_URL`) straight to `$exe`. The manifest worker unzips the
  release on the server, so only `curl` is needed. On a terminal,
  `run_download` empties its output before curl starts, so a failed download
  left a 0-byte binary over an existing install.
- `ant upgrade` (`src/cli/version.c`) looks up `ant_manifest_url()`
  (`ANT_MANIFEST_URL`), downloads to `<install_path>.tmp.<pid>`, and swaps it
  in with `ant_install_downloaded`. The install path comes from
  `ant_user_bin_path`, not `ANT_INSTALL`.
- `ant_download_get` (`src/download.c`) took no request headers, followed no
  redirects, and sent no `User-Agent`. GitHub's API requires one, and asset
  downloads answer with a 302.
- Each GitHub release carries `ant-{target}.zip` for every installer target,
  with a `sha256:<hex>` `digest` in the API. Each zip holds one `ant`
  (`ant.exe` on Windows), written with data descriptors (flag 0x08). There
  are no canary tags; canary builds exist only on the manifest.
- Prior art: pgray.dev's `scripts/install-ant.sh` does the GitHub path with
  `jq` and `unzip`.

## Scope

- `ANT_INSTALL_SOURCE=auto|manifest|github` selects the source for both the
  installer and `ant upgrade`. The default is `auto`, except that an explicit
  `ANT_DOWNLOAD_URL` or `ANT_MANIFEST_URL` implies `manifest` unless
  `ANT_INSTALL_SOURCE` is also set.
  - `auto` tries the manifest, and if the lookup or download fails, says why
    and retries from GitHub.
  - `manifest` never falls back. `github` skips the manifest.
- The GitHub path resolves the latest release, downloads `ant-{target}.zip`,
  verifies it against the API digest, extracts the binary, and installs it
  where the manifest path would.
- `ANT_GITHUB_API_URL` (default `https://api.github.com`) points `ant upgrade`
  at a fake API in tests.
- Out of scope: see Follow-ups.

## Constraints

- Installer:
  - The manifest path still needs only `curl`, and changes only by the source
    wrapper, staging, and timeouts.
  - No `jq`. Zip extraction probes `unzip`, `bsdtar` (or a bsdtar `tar`),
    `python3 -m zipfile`, then `busybox unzip`, and fails before any request
    if none exists.
  - bash 3.2 (macOS `/bin/bash`), the existing template helpers and style,
    no new colors.
  - Stage downloads in a temp dir inside `$ANT_INSTALL/bin`, then `mv` into
    place. That needs write access to `bin/` only, keeps the rename on one
    filesystem, and leaves `$exe` untouched on failure.
  - curl gets `--connect-timeout 10 --speed-limit 1 --speed-time 30`, so a
    stalled manifest falls back within about 30 seconds.
- `ant upgrade`:
  - `ant_download_get` keeps its signature for existing callers.
  - Redirects are followed for asset downloads only, at most 5 hops,
    https never to http, and never with an `Authorization` header.
  - Downloads stop at the API's asset `size` and after an idle timeout.
  - The zip reader accepts one stored or deflated entry named `ant` or
    `ant.exe`, non-empty, without zip64 or encryption, and checks its CRC-32
    and size.
  - Tests set `HOME` and `USERPROFILE` to temp dirs; `ant upgrade` otherwise
    replaces the real install.
- Both: `GITHUB_TOKEN` goes only to the API host, only when set, and never on
  a command line. An empty or null digest warns and continues. Any other
  digest must be exactly `sha256:` plus 64 lowercase hex characters, and
  must match.

## Design

- Installer: `install.json` gains `"github": { "repo", "asset" }`, rendered as
  `{{githubRepo}}` and `{{githubAsset}}`. `templates/github.template.sh` looks
  up `releases/latest` into a file (the token reaches curl through
  `--config -`), reads the asset's `browser_download_url` and `digest` with a
  small awk JSON tokenizer, downloads with `run_download`, hashes with
  `sha256sum`, `shasum`, or `openssl`, and extracts into the stage.
  `main.template.sh` picks the source, falls back, then runs `chmod` and `mv`.
  In `auto` the header shows the manifest URL; after a fallback, a fallback
  line and a `release:` line follow.
- `ant upgrade`: `ant_download_request` in `src/download.c` adds headers,
  redirect following through `parse_url_to_state`, a size cap, and an idle
  timeout. `version.c` looks up the release with `User-Agent: ant/<version>`,
  `X-GitHub-Api-Version`, and an optional bearer token. It downloads the zip
  into memory, hashes it with EVP, extracts it with `src/zip.c` to the
  existing temp path, and swaps it in. `build_timestamp` stays 0, so version
  comparison alone decides.

## Decision log

- Accepted (2026-10-24, pgray): stage downloads from both installer sources.
  A failed download used to empty `$exe` first, which an `auto` fallback
  would hit on every reinstall behind a block.
- Changed (2026-10-24, review): stage inside `bin/`, not `$ANT_INSTALL`.
  Staging in `$ANT_INSTALL` broke installs where only `bin/` is writable, such
  as `ANT_INSTALL=/usr/local` on Intel Homebrew.
- Accepted (2026-10-24, pgray): the `ant upgrade` fallback is in this plan,
  because upgrading behind the block was the case that was actually stuck.
- Accepted (2026-10-24, pgray): an explicit URL override implies `manifest`.
  Falling back would replace a pinned build or mirror with GitHub's latest.
- Accepted (2026-10-24, review): when a failed manifest download triggers the
  fallback, GitHub's latest must be newer than the running build, or
  `ant upgrade` fails. Otherwise a failed download, and every failed canary
  download, ended as "already up to date" with exit 0.
- Accepted (2026-10-24, review): the 30-second idle timeout lives in
  `ant_download_request`, so it also covers `ant_download_get`'s other
  callers (the manifest lookup, the update hint, and `ant compile` runtime
  downloads), which could previously hang forever.
- Accepted: `build_timestamp` stays 0 for GitHub releases, which carry none.
  All 44 published tags have distinct major.minor.patch values, so no upgrade
  is missed.
- Proposed: fall back automatically by default, because people behind a
  block find out through a failed install, not through a flag. Still to
  confirm with theMackabu.
- Open: should a missing digest fail instead of warning? Older releases have
  none.
- Not built: falling back to the unverified `releases/latest/download/<asset>`
  redirect when the API is rate-limited.
- Rejected: a separate `install-github.sh`, which would duplicate the target
  table and lose Windows support, profile setup, and the installer UI.

## Status

The installer and `ant upgrade` changes are implemented and have been
through an adversarial review and a succinctness review, with fixes applied.
Remaining: the unchecked validation items below (an unblocked manifest,
macOS, Windows), removing the local `src/tools/package-lock.json` build
workaround (see tech-debt), and a PR from `pgray/ant`.

## Validation status

Installer runs use temp `ANT_INSTALL` and `HOME`; upgrade runs use a temp
`HOME`.

Installer:

- [x] `bash -n` and shellcheck 0.11.0: only the two intentional SC2088
      warnings that were already in `shell.template.sh`.
- [ ] `manifest` on an unblocked network. The manifest is blocked from the
      dev machine; against a local stand-in, output and installed bytes match
      the old script's.
- [x] `github`: digest checked; `ant --version-raw` prints `16.5.c41757b4.0`.
- [x] `auto` behind the real block falls back and installs.
- [x] `ANT_DOWNLOAD_URL` alone, or with an empty `ANT_INSTALL_SOURCE`, fails
      with no fallback; with `ANT_INSTALL_SOURCE=auto` it falls back. Same
      under bash 3.2.
- [x] Target detection for real `uname -ms` strings: Git Bash's
      `MINGW64_NT-10.0-22631 x86_64` now selects `windows-x64` instead of
      `linux-x64`; the Darwin, Linux, and riscv64 strings are unchanged.
- [x] A failed install leaves an existing `$exe` byte-identical. The old
      script left 0 bytes.
- [x] A tampered digest aborts with `Checksum mismatch`; uppercase and
      `sha512:` digests fail too; an empty digest warns and installs.
- [x] Alpine: `linux-x64-musl`, busybox `unzip`.
- [x] `debian:stable-slim`: the `bsdtar`, `tar`, and `python3` extractors
      install; with none, it fails before any request.
- [x] `$ANT_INSTALL` read-only with a writable `bin/` installs.
- [x] A TLS-handshake stall and a no-response stall both fall back, after 11
      and 31 seconds.
- [ ] macOS under `/bin/bash` 3.2. No Mac; the `bash:3.2` image and BWK awk
      pass.
- [ ] Windows Git Bash. Not available; with `uname` faked, `ant.exe` lands
      at `bin/ant`.

The `patternFor` change in `render-install.js` fixes the Git Bash target on
both sources and is meant to be committed on its own.

`ant upgrade`:

- [x] `tests/test_upgrade_github.cjs` (25 cases, including real-layout zips
      with data descriptors, a redirect chain, and the review's failure
      cases), `tests/test_upgrade_replace.cjs`, and
      `tests/test_version_check_cache.cjs`.
- [x] Behind the real block, `auto` falls back to GitHub and reports up to
      date.
- [x] The real asset under a bumped `tag_name` (local fake API) follows the
      real 302, verifies, and installs the same bytes as `unzip -p`.
- [x] `manifest`, and an `ANT_MANIFEST_URL` override, fail with no fallback.
- [x] A stalled manifest times out after 30 seconds and falls back.

Both:

- [x] `maid preflight` (`ant .github/agents/check_all.js`): knowledge and
      structure checks pass; its recommended `meson compile -C build` and
      `tests/test_upgrade_github.cjs` pass.

## Follow-ups

- `ant compile` runtime downloads (`src/cli/compile.c`), sandbox assets
  (`src/sandbox/assets.c`), and the background update hint
  (`ant_version_print_update_hint`) still use only the manifest.
- Install paths disagree: on Windows the installer writes `bin/ant` and
  `ant upgrade` writes `bin/ant.exe`, and `ant upgrade` ignores a custom
  `ANT_INSTALL`.
- 403 is reported as a possible rate limit without reading
  `X-RateLimit-Remaining`, and a rejected `GITHUB_TOKEN` is not retried
  anonymously.
- `ant_download_request` skips a redirect response's body but reads it to the
  end instead of cancelling the request, so an endless 3xx body is bounded
  only by the server.
- Pin a version with `ANT_VERSION=<tag>` through `releases/tags/<tag>`.
- Append `$ANT_INSTALL/bin` to `$GITHUB_PATH` when it is set.
- Unrelated issues found during this work are in
  [tech-debt.md](../tech-debt.md).
