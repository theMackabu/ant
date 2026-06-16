#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

cd "$SCRIPT_DIR"
export PKG_CONFIG_PATH="$DEPS_DIR/lib/pkgconfig:$PKG_CONFIG_PATH"
export CC="${LIBANT_CC:-ccache clang}"
export CXX="${LIBANT_CXX:-ccache clang++}"
export CCACHE_DIR="${CCACHE_DIR:-$BUILD_DIR/.ccache}"

tlsuv_wrap="$SCRIPT_DIR/vendor/tlsuv.wrap"
tlsuv_dir="$(awk -F '=' '/^directory[[:space:]]*=/{gsub(/[[:space:]]/, "", $2); print $2; exit}' "$tlsuv_wrap")"

if [ -z "$tlsuv_dir" ]; then
  tlsuv_dir="$(basename "$tlsuv_wrap" .wrap)"
fi

tlsuv_prefix="$BUILD_DIR/vendor/$tlsuv_dir/deps"
mkdir -p "$BUILD_DIR/vendor/$tlsuv_dir"

rm -rf "$tlsuv_prefix"
cp -RL "$BUILD_DIR/deps" "$tlsuv_prefix"

setup_args=()
if [ ! -f build/meson-private/coredata.dat ]; then
  setup_args=(build --prefer-static -Ddeps_prefix_cmake="$tlsuv_prefix")
elif [ "$#" -gt 0 ]; then
  setup_args=(build --reconfigure --prefer-static -Ddeps_prefix_cmake="$tlsuv_prefix")
else
  coredata=build/meson-private/coredata.dat
  for config_input in meson.build meson_options.txt ../../sources.json ../../meson/meson.build ../../meson/deps/meson.build; do
    if [ "$config_input" -nt "$coredata" ]; then
      setup_args=(build --reconfigure --prefer-static -Ddeps_prefix_cmake="$tlsuv_prefix")
      break
    fi
  done
fi

if [ "${#setup_args[@]}" -gt 0 ]; then
  meson setup "${setup_args[@]}" "$@"
fi
meson compile -C build
