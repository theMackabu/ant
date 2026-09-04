#!/usr/bin/env bash

if [ -z "${ANT_PGO_TUNE_FLAG:-}" ]; then
  case "$(uname -s)-$(uname -m)" in
    Linux-x86_64|Linux-amd64)
      ANT_PGO_TUNE_FLAG="-march=x86-64"
      ;;
    Linux-arm64|Linux-aarch64)
      ANT_PGO_TUNE_FLAG="-march=armv8-a"
      ;;
    Darwin-x86_64|Darwin-i386|Darwin-i686)
      ANT_PGO_TUNE_FLAG="-march=native"
      ;;
    Darwin-arm64|Darwin-aarch64)
      ANT_PGO_TUNE_FLAG="-mcpu=native"
      ;;
    *)
      echo "error: unsupported PGO platform $(uname -s)-$(uname -m)" >&2
      return 1 2>/dev/null || exit 1
      ;;
  esac
fi

ANT_PGO_FLAGS="$ANT_PGO_TUNE_FLAG -Qunused-arguments -fvisibility=hidden -fvisibility-inlines-hidden -fno-math-errno -fno-trapping-math -fno-stack-protector"
export ANT_PGO_FLAGS
