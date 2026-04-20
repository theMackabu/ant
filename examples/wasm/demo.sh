#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/hello.c"
BASE="${SRC%.c}"

emcc -O3 "$SRC" -o "${BASE}.cjs"
./build/ant -pe "\`running ant \${Ant.version}\`"
./build/ant "${BASE}.cjs"
rm -f "${BASE}.cjs" "${BASE}.wasm"
