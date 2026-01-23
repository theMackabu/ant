#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$SCRIPT_DIR/dist"
SOURCE="$SCRIPT_DIR/../examples/embed/embed.c"
OUTPUT="$SCRIPT_DIR/dist/embed"

if [ ! -f "$DIST_DIR/libant.a" ]; then
  echo "Error: libant.a not found in $DIST_DIR"
  echo "Run ./build.sh first"
  exit 1
fi

case "$(uname -s)" in
  Darwin)
    LIBS="-framework Security -framework CoreFoundation -lpthread"
    ;;
  Linux)
    LIBS="-lpthread -ldl -lm"
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    LIBS="-lws2_32 -lrpcrt4 -lsecur32 -lntdll -lcrypt32 -luserenv"
    ;;
  *)
    LIBS="-lpthread"
    ;;
esac

CC="${CC:-clang}"

echo "Compiling embed example..."
$CC "$SOURCE" -I"$DIST_DIR" "$DIST_DIR/libant.a" $LIBS -o "$OUTPUT"
echo "Done: $OUTPUT"