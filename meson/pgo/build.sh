#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

FORCE_NO_NIX=0
for arg in "$@"; do
  case "$arg" in
    --force-no-nix) FORCE_NO_NIX=1 ;;
  esac
done

if [ "$FORCE_NO_NIX" -eq 0 ] && [ "${ANT_PGO_IN_NIX_SHELL:-0}" != "1" ]; then
  if ! command -v nix >/dev/null 2>&1; then
    echo "error: nix is required to enter the project devShell" >&2
    echo "hint: pass --force-no-nix to use the current shell toolchain" >&2
    exit 1
  fi
  echo "==> Entering 'nix develop' for the project toolchain"
  exec env ANT_PGO_IN_NIX_SHELL=1 nix develop "$ROOT" -c "$0" "$@"
elif [ "$FORCE_NO_NIX" -eq 1 ] && [ "${ANT_PGO_IN_NIX_SHELL:-0}" != "1" ]; then
  echo "==> Skipping 'nix develop'; using current shell toolchain"
fi

unset NIX_ENFORCE_NO_NATIVE
PGO_DIR="$ROOT/meson/pgo"
PROFILE_DIR="$PGO_DIR/profiles"
BUILD_DIR="$ROOT/build"
RAW_DIR="$BUILD_DIR/pgo-raw"

case "$(uname -s)" in
  Darwin) KERNEL=darwin ;;
  Linux)  KERNEL=linux ;;
  *) echo "error: unsupported OS $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  arm64|aarch64) CPU=aarch64 ;;
  x86_64|amd64)  CPU=x86_64 ;;
  *) echo "error: unsupported arch $(uname -m)" >&2; exit 1 ;;
esac
PROFDATA="$PROFILE_DIR/ant-$KERNEL-$CPU.profdata"

LLVM_PROFDATA=""
for cand in \
  "$(command -v llvm-profdata 2>/dev/null || true)" \
  /Library/Developer/CommandLineTools/usr/bin/llvm-profdata \
  /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-profdata
do
  if [ -n "$cand" ] && [ -x "$cand" ]; then
    LLVM_PROFDATA="$cand"
    break
  fi
done
if [ -z "$LLVM_PROFDATA" ]; then
  echo "error: llvm-profdata not found (install Xcode CLT or add llvm to PATH)" >&2
  exit 1
fi

case "$(uname -m)" in
  x86_64|i386|i686)
    CPU_TUNE_FLAG="-march=native"
    ;;
  *)
    CPU_TUNE_FLAG="-mcpu=native"
    ;;
esac

EXTRA_FLAGS="$CPU_TUNE_FLAG -Qunused-arguments -fvisibility=hidden -fvisibility-inlines-hidden -fno-math-errno -fno-trapping-math -fno-stack-protector"

SKIP_TRAIN=0
for arg in "$@"; do
  case "$arg" in
    --skip-train) SKIP_TRAIN=1 ;;
    --force-no-nix) ;;
    --help|-h)
      echo "usage: $0 [--skip-train] [--force-no-nix]"
      echo "  --skip-train     reuse the existing merged .profdata"
      echo "  --force-no-nix   use the current shell toolchain instead of nix develop"
      exit 0
      ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

if [ "$SKIP_TRAIN" -eq 0 ]; then
  echo "==> [1/3] Configuring instrumented build at $BUILD_DIR"
  rm -rf "$BUILD_DIR"
  mkdir -p "$PROFILE_DIR"
  (cd "$ROOT" && meson subprojects download >/dev/null 2>&1 || true)
  meson setup "$BUILD_DIR" \
    --buildtype=release \
    -Dpgo=disabled \
    "-Dpgo_generate_dir=$RAW_DIR" \
    -Db_lto=false \
    -Dstrip=false \
    "-Dc_args=$EXTRA_FLAGS" \
    "-Dcpp_args=$EXTRA_FLAGS"
  meson compile -C "$BUILD_DIR"

  echo "==> [2/3] Training (writes profraw to $RAW_DIR)"
  mkdir -p "$RAW_DIR"
  export LLVM_PROFILE_FILE="$RAW_DIR/profile-%p-%m.profraw"

  now_ms() {
    perl -MTime::HiRes=time -e 'printf "%d", time()*1000' 2>/dev/null || echo $(( $(date +%s) * 1000 ))
  }
  fmt_secs() {
    awk "BEGIN{printf \"%.1fs\", $1/1000}"
  }

  if [ -f "$ROOT/examples/spec/run.js" ]; then
    start=$(now_ms)
    "$BUILD_DIR/ant" "$ROOT/examples/spec/run.js" --all >/dev/null 2>&1 || true
    echo "    - spec suite ($(fmt_secs $(( $(now_ms) - start ))))"
  fi

  echo "    - bench files in tests/"
  while IFS= read -r -d '' bench; do
    start=$(now_ms)
    rc=0
    timeout 60s "$BUILD_DIR/ant" "$bench" >/dev/null 2>&1 || rc=$?
    elapsed=$(fmt_secs $(( $(now_ms) - start )))
    note=""
    if [ "$rc" -eq 124 ]; then
      note="  [TIMEOUT — killed, no profile data from this run]"
    elif [ "$rc" -ne 0 ]; then
      note="  [exit $rc]"
    fi
    printf '      %-40s %8s%s\n' "$(basename "$bench")" "$elapsed" "$note"
  done < <(find "$ROOT/tests" -maxdepth 2 -type f \( -name 'bench_*.js' -o -name 'bench_*.cjs' -o -name 'bench_*.mjs' \) ! -name 'bench_server.*' -print0)

  if [ -f "$PGO_DIR/train_server.js" ] && command -v curl >/dev/null 2>&1; then
    SERVER_PORT=34117
    start=$(now_ms)
    "$BUILD_DIR/ant" "$PGO_DIR/train_server.js" >/dev/null 2>&1 &
    SERVER_PID=$!
    ready=0
    for _ in $(seq 1 100); do
      if curl -sf --max-time 2 -o /dev/null "http://127.0.0.1:$SERVER_PORT/"; then ready=1; break; fi
      kill -0 "$SERVER_PID" 2>/dev/null || break
      sleep 0.1
    done
    if [ "$ready" -eq 1 ]; then
      if command -v oha >/dev/null 2>&1; then
        req_label="50000 requests via oha"
        oha -n 50000 --no-tui -H "Accept-Encoding: identity" "http://127.0.0.1:$SERVER_PORT/hot" >/dev/null 2>&1 || true
      else
        req_label="8000 requests via curl"
        curl_pids=()
        for _ in 1 2 3 4; do
          curl -s --max-time 60 -o /dev/null "http://127.0.0.1:$SERVER_PORT/hot/[1-2000]" &
          curl_pids+=($!)
        done
        wait "${curl_pids[@]}" 2>/dev/null || true
      fi
      curl -s --max-time 5 -o /dev/null "http://127.0.0.1:$SERVER_PORT/quit" || true
      for _ in $(seq 1 50); do
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.1
      done
      note=""
      if kill -0 "$SERVER_PID" 2>/dev/null; then
        note="  [did not exit on /quit — killed, no profile data from this run]"
        kill -9 "$SERVER_PID" 2>/dev/null || true
      fi
      wait "$SERVER_PID" 2>/dev/null || true
      printf '      %-40s %8s  [%s]%s\n' "bench_server.js" "$(fmt_secs $(( $(now_ms) - start )))" "$req_label" "$note"
    else
      kill -9 "$SERVER_PID" 2>/dev/null || true
      wait "$SERVER_PID" 2>/dev/null || true
      printf '      %-40s %8s  [server never became ready — skipped]\n' "bench_server.js" "$(fmt_secs $(( $(now_ms) - start )))"
    fi
  fi

  echo "==> Merging profiles -> $PROFDATA"
  shopt -s nullglob
  raw=( "$RAW_DIR"/*.profraw )
  if [ ${#raw[@]} -eq 0 ]; then
    echo "error: no .profraw files were produced; training crashed or wrote nothing" >&2
    exit 1
  fi
  "$LLVM_PROFDATA" merge -output="$PROFDATA" "${raw[@]}"
  echo "    $(ls -lh "$PROFDATA" | awk '{print $5}') of profile data"
else
  mkdir -p "$PROFILE_DIR"
  if [ ! -f "$PROFDATA" ]; then
    echo "error: --skip-train but $PROFDATA does not exist" >&2
    exit 1
  fi
  echo "==> Reusing existing profile $PROFDATA"
fi

echo "==> [3/3] Final PGO build at $BUILD_DIR"
rm -rf "$BUILD_DIR"
meson setup "$BUILD_DIR" \
  --buildtype=release \
  -Dpgo=enabled \
  -Db_lto=true \
  -Db_lto_mode=default \
  "-Dc_args=$EXTRA_FLAGS" \
  "-Dcpp_args=$EXTRA_FLAGS"
meson compile -C "$BUILD_DIR"

echo
echo "PGO build complete."
echo "  binary:  $BUILD_DIR/ant"
echo "  profile: $PROFDATA"
