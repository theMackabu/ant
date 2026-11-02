# ant:shell compatibility tools

Differential tools for `ant:shell`. Each file in `cases/` holds shell
scripts separated by `---` lines.

- `./build/ant tests/shell_compat/diff_bash.mjs [--posix]` runs every case
  under the local `bash` (or `BASH_BIN`) and under `ant:shell`, then prints
  each mismatch in stdout, exit status, or stderr presence.
- `node tests/shell_compat/gen_fixture.mjs` regenerates
  `tests/fixtures/shell_bash_compat.json`, the expected output that
  `tests/test_shell_compat.js` replays. Generate it with a standard Bash 5.3
  build (with programmable completion) on Linux or macOS. Do not hand-edit
  the fixture.
- `bun tests/shell_compat/bun_compare.mjs` and
  `./build/ant tests/shell_compat/bun_compare.mjs` print the same API probes
  as JSON under Bun and Ant for a side-by-side diff.
