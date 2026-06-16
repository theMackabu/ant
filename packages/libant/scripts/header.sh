#!/bin/bash
set -e
. "$(dirname "$0")/common.sh"

OUTPUT="$1"
ANT_VERSION="$2"
ANT_BUILD_TIMESTAMP="$3"
ANT_GIT_HASH="$4"
ANT_GIT_LONGHASH="$5"
ANT_TARGET_TRIPLE="$6"
INCLUDE_DIR="$ROOT_DIR/include"

if [ -z "$OUTPUT" ] || [ -z "$ANT_VERSION" ]; then
  echo "Usage: $0 <output.h> <version> <timestamp> <git_hash> <git_longhash> <target_triple>"
  exit 1
fi

VENDOR_DIR="$SCRIPT_DIR/vendor"
LIBUV_INCLUDE_DIR="$VENDOR_DIR/libuv-v1.52.0/include"

HEADERS=(
  "uthash.h:$VENDOR_DIR/uthash-2.3.0/src/uthash.h"
  "utarray.h:$VENDOR_DIR/uthash-2.3.0/src/utarray.h"
  "types.h:$INCLUDE_DIR/types.h"
  "debug.h:$INCLUDE_DIR/debug.h"
  "common.h:$INCLUDE_DIR/common.h"
  "shapes.h:$INCLUDE_DIR/shapes.h"
  "object.h:$INCLUDE_DIR/object.h"
  "uv.h:$LIBUV_INCLUDE_DIR/uv.h"
  "errors.h:$INCLUDE_DIR/errors.h"
  "ant.h:$INCLUDE_DIR/ant.h"
  "arena.h:$INCLUDE_DIR/arena.h"
  "pool.h:$INCLUDE_DIR/pool.h"
  "minicoro.h:$VENDOR_DIR/minicoro/minicoro.h"
  "esm/loader.h:$INCLUDE_DIR/esm/loader.h"
  "sugar.h:$INCLUDE_DIR/sugar.h"
  "descriptors.h:$INCLUDE_DIR/descriptors.h"
  "internal.h:$INCLUDE_DIR/internal.h"
  "gc/objects.h:$INCLUDE_DIR/gc/objects.h"
  "gc/modules.h:$INCLUDE_DIR/gc/modules.h"
  "runtime.h:$INCLUDE_DIR/runtime.h"
  "modules/symbol.h:$INCLUDE_DIR/modules/symbol.h"
  "modules/timer.h:$INCLUDE_DIR/modules/timer.h"
  "silver/vm.h:$INCLUDE_DIR/silver/vm.h"
  "silver/engine.h:$INCLUDE_DIR/silver/engine.h"
  "modules/url.h:$INCLUDE_DIR/modules/url.h"
  "net/listener.h:$INCLUDE_DIR/net/listener.h"
  "net/connection.h:$INCLUDE_DIR/net/connection.h"
  "reactor.h:$INCLUDE_DIR/reactor.h"
  "tokens.h:$INCLUDE_DIR/tokens.h"
  "compat.h:$INCLUDE_DIR/compat.h"
  "utils.h:$INCLUDE_DIR/utils.h"
  "esm/remote.h:$INCLUDE_DIR/esm/remote.h"
)

for f in "$INCLUDE_DIR"/modules/*.h; do
  name="modules/$(basename "$f")"
  HEADERS+=("$name:$f")
done

DEDUPED_HEADERS=()
seen_headers=" "
for entry in "${HEADERS[@]}"; do
  name="${entry%%:*}"
  if [[ "$seen_headers" == *" $name "* ]]; then
    continue
  fi
  DEDUPED_HEADERS+=("$entry")
  seen_headers="$seen_headers$name "
done
HEADERS=("${DEDUPED_HEADERS[@]}")

emit_header_file() {
  local path="$1"

  while IFS= read -r line || [ -n "$line" ]; do
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*pragma[[:space:]]+once ]]; then
      continue
    fi

    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\"silver/opcode\.h\" ]]; then
      cat "$INCLUDE_DIR/silver/opcode.h" >> "$OUTPUT"
      continue
    fi

    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\"(uv/[^\"[:space:]]+\.h)\" ]]; then
      uv_path="$LIBUV_INCLUDE_DIR/${BASH_REMATCH[1]}"
      if [ -f "$uv_path" ]; then
        echo "/* === ${BASH_REMATCH[1]} === */" >> "$OUTPUT"
        emit_header_file "$uv_path"
        echo "" >> "$OUTPUT"
      fi
      continue
    fi

    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\" ]]; then
      continue
    fi
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\<(metadata|common|types|uthash|utarray|minicoro|uv)\.h\> ]]; then
      continue
    fi
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\<uv/ ]]; then
      continue
    fi

    echo "$line" >> "$OUTPUT"
  done < "$path"
}

cat > "$OUTPUT" << EOF
/*
 * Ant JavaScript Engine
 * https://github.com/themackabu/ant
 * 
 * The MIT License (MIT)
 * 
 * Copyright (c) 2026 theMackabu (me@themackabu.dev)
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * ---
 * 
 * This is an auto-generated amalgamated header containing all public APIs.
 * Link with libant.a and required system libraries:
 *   macOS:   -framework Security -framework CoreFoundation -lpthread
 *   Linux:   -lpthread -ldl -lm
 *   Windows: -lws2_32 -lrpcrt4 -lsecur32 -lntdll -lcrypt32 -luserenv
 */
#ifndef LIBANT_H
#define LIBANT_H

/* forward declarations */
struct arg_file;

/* === metadata === */
#define ANT_VERSION "$ANT_VERSION"
#define ANT_BUILD_TIMESTAMP $ANT_BUILD_TIMESTAMP
#define ANT_GIT_HASH "$ANT_GIT_HASH"
#define ANT_GIT_LONGHASH "$ANT_GIT_LONGHASH"
#define ANT_TARGET_TRIPLE "$ANT_TARGET_TRIPLE"

EOF

for entry in "${HEADERS[@]}"; do
  name="${entry%%:*}"
  path="${entry##*:}"
  
  if [ ! -f "$path" ]; then
    echo "Warning: $path not found, skipping" >&2
    continue
  fi
  
  echo "/* === $name === */" >> "$OUTPUT"
  
  emit_header_file "$path"
  
  echo "" >> "$OUTPUT"
done

echo "#endif /* LIBANT_H */" >> "$OUTPUT"

echo "Generated $OUTPUT"
