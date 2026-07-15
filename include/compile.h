#ifndef ANT_COMPILE_H
#define ANT_COMPILE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Single-file executables ("ant compile") — the prebuilt-runtime + append-payload
 * model that deno compile, bun build --compile, and Node's Single Executable
 * Applications use. `ant compile app.js -o app` copies the running `ant` runtime,
 * appends an application archive (the same ANTAPP01 container packages/desktop
 * writes) plus a fixed trailer, and marks it executable. At startup a fused
 * binary detects the trailer, extracts the embedded app, and runs its entry
 * module instead of entering the CLI/REPL.
 *
 * Spike scope (issue #54): host-target output only. Cross-compilation
 * (--target <triple> fetching a prebuilt base) and a stripped runtime base are
 * deliberate follow-ups; the trailer/archive format is target-independent so
 * append-based cross-compilation slots in without a format change.
 */

/* True when this executable carries an appended fused payload. */
bool ant_compile_has_payload(void);

/*
 * Extract the fused payload to a fresh temporary directory and return the
 * absolute path of the application entry module (heap-allocated; caller frees).
 * On success *out_root receives the temp root (heap-allocated) so it can be
 * removed with ant_compile_cleanup() at exit. Returns NULL on failure.
 */
char *ant_compile_extract_payload(char **out_root);

/* Best-effort recursive removal of a directory created during extraction. */
void ant_compile_cleanup(const char *root);

/* `ant compile <entry> [-o out] [--root dir] [--runtime base]` subcommand. */
int ant_compile_cmd(int argc, char **argv);

#endif
