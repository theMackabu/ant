#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

cd "$ROOT_DIR"
meson subprojects download

RSYNC_OPTS=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|CLANG*|UCRT*) RSYNC_OPTS="--copy-links" ;;
esac

mkdir -p "$SCRIPT_DIR/vendor"
rsync -a $RSYNC_OPTS --exclude='.git/' "$ROOT_DIR/vendor/" "$SCRIPT_DIR/vendor/"

mkdir -p "$BUILD_DIR"
