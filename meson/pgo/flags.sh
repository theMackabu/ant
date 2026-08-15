#!/usr/bin/env bash

case "$(uname -m)" in
  x86_64|i386|i686)
    ANT_PGO_TUNE_FLAG="-march=native"
    ;;
  arm64|aarch64)
    ANT_PGO_TUNE_FLAG="-mcpu=native"
    ;;
  *)
    echo "error: unsupported PGO architecture $(uname -m)" >&2
    return 1 2>/dev/null || exit 1
    ;;
esac

ANT_PGO_FLAGS="$ANT_PGO_TUNE_FLAG -Qunused-arguments -fvisibility=hidden -fvisibility-inlines-hidden -fno-math-errno -fno-trapping-math -fno-stack-protector"
export ANT_PGO_FLAGS
