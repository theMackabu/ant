#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

cd "$SCRIPT_DIR"

export PKG_CONFIG_PATH="$DEPS_DIR/lib/pkgconfig:$PKG_CONFIG_PATH"

meson setup build --prefer-static -Dtls_library=mbedtls -Ddeps_prefix_cmake="$DEPS_DIR" "$@"
meson compile -C build
