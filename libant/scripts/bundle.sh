#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

DIST_DIR="$SCRIPT_DIR/dist"
mkdir -p "$DIST_DIR"

bundle_lib() {
  NAME="$1"
  EXCLUDE="$2"
  OUTPUT="$BUILD_DIR/$NAME"
  
  echo "Bundling $NAME..."
  
  TMPDIR=$(mktemp -d)
  
  LIBS=$(find "$BUILD_DIR" -name '*.a' \
    ! -name 'libant.a' \
    ! -name 'libant-lto.a' \
    ! -path '*/oxc-target/release/deps/*' \
    ! -path '*/.external/*' \
    2>/dev/null | grep -v "$EXCLUDE" | sort -u)
  
  cd "$TMPDIR"
  for lib in $LIBS; do
    libname=$(basename "$lib" .a)
    mkdir -p "$libname"
    (cd "$libname" && llvm-ar x "$lib" 2>/dev/null || ar x "$lib")
  done
  
  find . -name '*.o' > objects.txt
  
  if [ ! -s objects.txt ]; then
    echo "No objects found, skipping $NAME"
    rm -rf "$TMPDIR"
    cd "$SCRIPT_DIR"
    return
  fi
  
  if command -v llvm-ar >/dev/null 2>&1; then
    llvm-ar rcs "$OUTPUT" $(cat objects.txt)
  else
    ar rcs "$OUTPUT" $(cat objects.txt)
  fi
  
  rm -rf "$TMPDIR"
  cd "$SCRIPT_DIR"
  
  cp "$OUTPUT" "$DIST_DIR/"
  echo "Created: $DIST_DIR/$NAME ($(du -h "$OUTPUT" | cut -f1))"
}

bundle_lib "libant.a" "_lto"

if [ -f "$BUILD_DIR/libant_core_lto.a" ]; then
  bundle_lib "libant-lto.a" "libant_core.a"
fi

if [ -f "$BUILD_DIR/libant.h" ]; then
  cp "$BUILD_DIR/libant.h" "$DIST_DIR/ant.h"
  echo "Created: $DIST_DIR/ant.h"
fi

echo ""
echo "Done! Distribution files in $DIST_DIR:"
ls -lh "$DIST_DIR"/ 2>/dev/null || echo "No files found"
