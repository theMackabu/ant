#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
ENGINE_ROOT=${ANT_BENCH_ENGINE_ROOT:-"$REPO_ROOT/.cache/arm64-bench"}
BUILD_DIR=${ANT_BENCH_BUILD_DIR:-"$ENGINE_ROOT/ant-bench-build"}
CACHE_ROOT=${ANT_BENCH_BUILD_CACHE_ROOT:-"$ENGINE_ROOT/build-cache"}

if [[ "${ANT_BENCH_IN_NIX_DEVELOP:-0}" != 1 ]]; then
  command -v nix >/dev/null || { echo "Nix is required to build benchmark Ant" >&2; exit 1; }
  echo "entering the repository's pinned Nix development shell"
  exec env ANT_BENCH_IN_NIX_DEVELOP=1 \
    nix develop "$REPO_ROOT" --command "$SCRIPT_DIR/build-ant.sh"
fi

cd "$REPO_ROOT"
if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
  echo "arm64-bench requires an ARM64 macOS host" >&2
  exit 1
fi

for command in ccache jq meson ninja shasum llvm-nm; do
  command -v "$command" >/dev/null || { echo "missing Nix dev-shell command: $command" >&2; exit 1; }
done

nix_cc=${CC:?Nix dev shell did not set CC}
nix_cxx=${CXX:?Nix dev shell did not set CXX}
export CC="ccache $nix_cc"
export CXX="ccache $nix_cxx"
export CCACHE_DIR="$CACHE_ROOT/ccache"
export CCACHE_BASEDIR="$REPO_ROOT"
export CCACHE_COMPILERCHECK=content
export MESON_PACKAGE_CACHE_DIR="$CACHE_ROOT/meson-packages"
mkdir -p "$BUILD_DIR" "$CCACHE_DIR" "$MESON_PACKAGE_CACHE_DIR"
ccache --max-size "${ANT_BENCH_CCACHE_SIZE:-20G}" >/dev/null
ccache --zero-stats >/dev/null

profile_path="$REPO_ROOT/meson/pgo/profiles/ant-darwin-aarch64.profdata"
[[ -f "$profile_path" ]] || { echo "missing PGO profile: $profile_path" >&2; exit 1; }
meson subprojects download >/dev/null 2>&1 || true

build_timestamp=${SOURCE_DATE_EPOCH:-$(git show -s --format=%ct HEAD)}
meson_setup=(setup "$BUILD_DIR")
if [[ -f "$BUILD_DIR/meson-private/coredata.dat" ]]; then
  meson_setup+=(--reconfigure)
fi
meson "${meson_setup[@]}" \
  --buildtype=release \
  -Db_lto=true \
  -Db_lto_mode=default \
  -Dcodesign=false \
  -Dembed_example=disabled \
  -Dpgo=enabled \
  -Druntime_binary=disabled \
  "-Dbuild_timestamp=$build_timestamp" \
  "-Dllvm_nm=$(command -v llvm-nm)"

meson compile -C "$BUILD_DIR"

compiler_version=$($nix_cc --version | sed -n '1p')
profile_sha=$(shasum -a 256 "$profile_path" | awk '{print $1}')
jq -n \
  --arg type nix-develop-release-pgo-lto \
  --arg compiler "$compiler_version" \
  --arg tuning native-arm64 \
  --arg pgoProfileSha256 "$profile_sha" \
  '{type: $type, compiler: $compiler, tuning: $tuning, pgoProfileSha256: $pgoProfileSha256}' \
  > "$BUILD_DIR/arm64-bench-build.json"

ccache --show-stats
"$BUILD_DIR/ant" --version
