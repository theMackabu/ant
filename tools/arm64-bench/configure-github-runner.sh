#!/usr/bin/env bash
set -euo pipefail

RUNNER_DIR=${ANT_BENCH_RUNNER_DIR:-/Users/remote/actions-runner}
REPOSITORY_URL=${ANT_BENCH_REPOSITORY_URL:-https://github.com/theMackabu/ant}

if [[ -z "${GITHUB_RUNNER_TOKEN:-}" ]]; then
  echo "GITHUB_RUNNER_TOKEN must contain a short-lived repository runner token" >&2
  exit 1
fi
if [[ ! -x "$RUNNER_DIR/config.sh" ]]; then
  echo "GitHub Actions runner is not installed in $RUNNER_DIR" >&2
  exit 1
fi
if [[ -f "$RUNNER_DIR/.runner" ]]; then
  echo "runner is already configured in $RUNNER_DIR" >&2
  exit 1
fi

cd "$RUNNER_DIR"
./config.sh \
  --url "$REPOSITORY_URL" \
  --token "$GITHUB_RUNNER_TOKEN" \
  --name arm64-bench \
  --labels arm64-bench \
  --work _work \
  --unattended
./svc.sh install
./svc.sh start
./svc.sh status
