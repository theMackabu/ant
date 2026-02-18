#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

cd "$SCRIPT_DIR"
export PKG_CONFIG_PATH="$DEPS_DIR/lib/pkgconfig:$PKG_CONFIG_PATH"

tlsuv_wrap="$SCRIPT_DIR/vendor/tlsuv.wrap"
tlsuv_dir="$(awk -F '=' '/^directory[[:space:]]*=/{gsub(/[[:space:]]/, "", $2); print $2; exit}' "$tlsuv_wrap")"

if [ -z "$tlsuv_dir" ]; then
  echo "Failed to resolve tlsuv directory from $tlsuv_wrap" >&2
  exit 1
fi

tlsuv_prefix="$BUILD_DIR/vendor/$tlsuv_dir/deps"
mkdir -p "$BUILD_DIR/vendor/$tlsuv_dir"

rm -rf "$tlsuv_prefix"
cp -RL "$BUILD_DIR/deps" "$tlsuv_prefix"

meson setup build --prefer-static -Dtls_library=mbedtls -Ddeps_prefix_cmake="$tlsuv_prefix" "$@"
meson compile -C build
