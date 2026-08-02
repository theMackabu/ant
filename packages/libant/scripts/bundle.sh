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

  LIBS=$(find "$BUILD_DIR" -name '*.a' \
    ! -name 'libant.a' \
    ! -name 'libant-lto.a' \
    ! -name 'libpkg.a' \
    ! -path '*/.external/*' \
    2>/dev/null | grep -E -v "$EXCLUDE" | sort -u)

  if [ -z "$LIBS" ]; then
    echo "No libraries found, skipping $NAME"
    return
  fi

  if command -v llvm-ar >/dev/null 2>&1; then
    AR=llvm-ar
  elif ar --version 2>/dev/null | head -n 1 | grep -q GNU; then
    AR=ar
  else
    AR=""
  fi

  if [ -n "$AR" ]; then
    if command -v cygpath >/dev/null 2>&1; then
      mri_path() { cygpath -m "$1"; }
    else
      mri_path() { printf '%s\n' "$1"; }
    fi
    {
      echo "CREATE $(mri_path "$OUTPUT.tmp")"
      if [ -f "$OUTPUT" ]; then
        echo "ADDLIB $(mri_path "$OUTPUT")"
      fi
      for lib in $LIBS; do
        echo "ADDLIB $(mri_path "$lib")"
      done
      echo "SAVE"
      echo "END"
    } | "$AR" -M
    mv "$OUTPUT.tmp" "$OUTPUT"
  else
    temp_dir=$(mktemp -d)
    cd "$temp_dir"
    for lib in $LIBS; do
      libname=$(basename "$lib" .a)
      mkdir -p "$libname"
      (cd "$libname" && ar x "$lib")
    done
    find . -name '*.o' > objects.txt
    ar rcs "$OUTPUT" $(cat objects.txt)
    rm -rf "$temp_dir"
    cd "$SCRIPT_DIR"
  fi

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

pkg_lib_path=$(find "$BUILD_DIR" -name 'libpkg.a' -print | head -n 1)
if [ -n "$pkg_lib_path" ] && [ -f "$pkg_lib_path" ]; then
  cp "$pkg_lib_path" "$DIST_DIR/libpkg.a"
  echo "Created: $DIST_DIR/libpkg.a ($(du -h "$pkg_lib_path" | cut -f1))"
fi

if [ -f "$ROOT_DIR/include/pkg.h" ]; then
  cp "$ROOT_DIR/include/pkg.h" "$DIST_DIR/pkg.h"
  echo "Created: $DIST_DIR/pkg.h"
fi

echo ""
echo "Done! Distribution files in $DIST_DIR:"
ls -lh "$DIST_DIR"/ 2>/dev/null || echo "No files found"
