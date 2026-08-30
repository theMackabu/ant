#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
VERSIONS_FILE="$SCRIPT_DIR/versions.json"
RUNNER_DIR=${ANT_BENCH_RUNNER_DIR:-/Users/remote/actions-runner}

for command in curl jq shasum tar; do
  command -v "$command" >/dev/null || { echo "missing required command: $command" >&2; exit 1; }
done

version=$(jq -r '.githubRunner.version' "$VERSIONS_FILE")
url=$(jq -r '.githubRunner.url' "$VERSIONS_FILE")
expected=$(jq -r '.githubRunner.sha256' "$VERSIONS_FILE")

if [[ -x "$RUNNER_DIR/bin/Runner.Listener" ]]; then
  actual=$("$RUNNER_DIR/bin/Runner.Listener" --version)
  [[ "$actual" == "$version" ]] || {
    echo "runner version mismatch: expected $version, found $actual" >&2
    exit 1
  }
  echo "GitHub Actions runner $actual is already installed in $RUNNER_DIR"
  exit 0
fi
if [[ -e "$RUNNER_DIR" ]]; then
  echo "refusing to overwrite existing path: $RUNNER_DIR" >&2
  exit 1
fi

stage=$(mktemp -d "${TMPDIR:-/tmp}/ant-actions-runner.XXXXXX")
archive="$stage/actions-runner.tar.gz"
trap 'rm -rf "$stage"' EXIT

curl -fL --retry 3 -o "$archive" "$url"
echo "$expected  $archive" | shasum -a 256 -c -
mkdir -p "$RUNNER_DIR"
tar -xzf "$archive" -C "$RUNNER_DIR"
"$RUNNER_DIR/bin/Runner.Listener" --version
