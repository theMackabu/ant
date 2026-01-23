#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

cd "$SCRIPT_DIR"

export PKG_CONFIG_PATH="$DEPS_DIR/lib/pkgconfig:$PKG_CONFIG_PATH"

mkdir -p "$BUILD_DIR/vendor/tlsuv"
rm -rf "$BUILD_DIR/vendor/tlsuv/deps"
cp -RL "$BUILD_DIR/deps" "$BUILD_DIR/vendor/tlsuv/deps"

meson setup build --prefer-static -Dtls_library=mbedtls -Ddeps_prefix_cmake="$BUILD_DIR/vendor/tlsuv/deps" "$@"
meson compile -C build