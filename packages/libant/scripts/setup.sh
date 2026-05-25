#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

cd "$ROOT_DIR"
meson subprojects download

if [ ! -d "$SCRIPT_DIR/vendor" ]; then
  cp -r "$ROOT_DIR/vendor" "$SCRIPT_DIR/vendor"
fi

mkdir -p "$BUILD_DIR"
