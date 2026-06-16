#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$SCRIPT_DIR/dist"
SOURCE="$SCRIPT_DIR/../../examples/embed/embed.c"
OUTPUT="$SCRIPT_DIR/dist/embed"

if [ ! -f "$DIST_DIR/libant.a" ]; then
  echo "Error: libant.a not found in $DIST_DIR"
  echo "Run ./build.sh first"
  exit 1
fi

case "$(uname -s)" in
  Darwin)
    LIBS="-framework Security -framework CoreFoundation -lpthread"
    CFLAGS="${CFLAGS:-} -mmacosx-version-min=15.0"
    LDFLAGS="${LDFLAGS:-} -mmacosx-version-min=15.0"
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
CXX="${CXX:-clang++}"
OBJ="$DIST_DIR/embed.o"

echo "Compiling embed example..."
$CC $CFLAGS -c "$SOURCE" -I"$DIST_DIR" -o "$OBJ"
$CXX $LDFLAGS "$OBJ" "$DIST_DIR/libant.a" $LIBS -o "$OUTPUT"
echo "Done: $OUTPUT"
