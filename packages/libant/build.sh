#!/bin/bash
set -e

SCRIPTS_DIR="$(cd "$(dirname "$0")/scripts" && pwd)"

"$SCRIPTS_DIR/setup.sh"
"$SCRIPTS_DIR/deps.sh"
"$SCRIPTS_DIR/compile.sh" "$@"
"$SCRIPTS_DIR/bundle.sh"
