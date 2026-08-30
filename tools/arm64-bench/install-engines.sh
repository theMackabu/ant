#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
ENGINE_ROOT=${ANT_BENCH_ENGINE_ROOT:-"$REPO_ROOT/.cache/arm64-bench"}

if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
  echo "arm64-bench engine provisioning requires ARM64 macOS" >&2
  exit 1
fi
command -v brew >/dev/null || { echo "Homebrew is required for host provisioning" >&2; exit 1; }

export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_ENV_HINTS=1
export HOMEBREW_NO_INSTALL_CLEANUP=1
export NONINTERACTIVE=1

brew install autoconf autoconf-archive automake ccache cmake jq libtool meson nasm ninja pkg-config rustup
ANT_BENCH_ENGINE_ROOT="$ENGINE_ROOT" "$SCRIPT_DIR/sync-engines.sh"
