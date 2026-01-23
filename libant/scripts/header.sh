#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

CONFIG_H="$1"
OUTPUT="$2"
INCLUDE_DIR="$ROOT_DIR/include"

if [ -z "$CONFIG_H" ] || [ -z "$OUTPUT" ]; then
  echo "Usage: $0 <config.h> <output.h>"
  exit 1
fi

VENDOR_DIR="$SCRIPT_DIR/vendor"

HEADERS=(
  "config.h:$CONFIG_H"
  "common.h:$INCLUDE_DIR/common.h"
  "compat.h:$INCLUDE_DIR/compat.h"
  "ant.h:$INCLUDE_DIR/ant.h"
  "utils.h:$INCLUDE_DIR/utils.h"
  "minicoro.h:$VENDOR_DIR/minicoro/minicoro.h"
  "runtime.h:$INCLUDE_DIR/runtime.h"
  "esm/remote.h:$INCLUDE_DIR/esm/remote.h"
  "argtable3.h:$VENDOR_DIR/argtable-v3.3.0.116da6c/src/argtable3.h"
)

for f in "$INCLUDE_DIR"/modules/*.h; do
  name="modules/$(basename "$f")"
  HEADERS+=("$name:$f")
done

cat > "$OUTPUT" << 'EOF'
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

EOF

for entry in "${HEADERS[@]}"; do
  name="${entry%%:*}"
  path="${entry##*:}"
  
  if [ ! -f "$path" ]; then
    echo "Warning: $path not found, skipping" >&2
    continue
  fi
  
  echo "/* === $name === */" >> "$OUTPUT"
  
  while IFS= read -r line || [ -n "$line" ]; do
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*pragma[[:space:]]+once ]]; then
      continue
    fi
    
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\"(config\.h|compat\.h|ant\.h|utils\.h|arena\.h|runtime\.h|internal\.h)\" ]]; then
      continue
    fi
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\"esm/ ]]; then
      continue
    fi
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\"modules/ ]]; then
      continue
    fi
    
    if [[ "$line" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]+\<(config|common|argtable3)\.h\> ]]; then
      continue
    fi
    
    echo "$line" >> "$OUTPUT"
  done < "$path"
  
  echo "" >> "$OUTPUT"
done

echo "#endif /* LIBANT_H */" >> "$OUTPUT"

echo "Generated $OUTPUT"
