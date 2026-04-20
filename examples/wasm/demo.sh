#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/hello.c"
BASE="${SRC%.c}"

emcc -O3 "$SRC" -o "${BASE}.cjs"
ant "${BASE}.cjs"
rm -f "${BASE}.cjs" "${BASE}.wasm"
