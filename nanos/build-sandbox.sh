#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: nanos/build-sandbox.sh [options]

build a local Linux musl Ant binary in Alpine Docker, then build a Nanos
sandbox image with ops.

options:
  --arch <native|x64|amd64|aarch64|arm64>
      target architecture. defaults to native.
  --image <name>
      ops image name. defaults to ant-sandbox-x64 or ant-sandbox-aarch64.
  --out <dir>
      output directory. defaults to nanos/out/<arch>.
  --no-cache
      pass --no-cache to docker build.
  -h, --help
      show this help.
USAGE
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

arch=native
image=
out_dir=
docker_no_cache=()

while (($#)); do
  case "$1" in
    --arch)
      arch=${2:-}
      shift 2
      ;;
    --image)
      image=${2:-}
      shift 2
      ;;
    --out)
      out_dir=${2:-}
      shift 2
      ;;
    --no-cache)
      docker_no_cache=(--no-cache)
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$arch" || "$arch" == "native" ]]; then
  case "$(uname -m)" in
    x86_64|amd64) arch=x64 ;;
    arm64|aarch64) arch=aarch64 ;;
    *)
      echo "unsupported host architecture: $(uname -m)" >&2
      exit 1
      ;;
  esac
fi

case "$arch" in
  x64|amd64)
    arch=x64
    docker_platform=linux/amd64
    ops_arch=amd64
    image=${image:-ant-sandbox-x64}
    ;;
  aarch64|arm64)
    arch=aarch64
    docker_platform=linux/arm64
    ops_arch=arm64
    image=${image:-ant-sandbox-aarch64}
    ;;
  *)
    echo "unsupported architecture: $arch" >&2
    exit 1
    ;;
esac

out_dir=${out_dir:-"$repo_root/nanos/out/$arch"}
binary_dir="$out_dir/binary"
image_out="$out_dir/ant-sandbox.img"
if [[ -n "${BUILD_TIMESTAMP:-}" ]]; then
  build_timestamp=$BUILD_TIMESTAMP
elif git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  build_timestamp=$(git -C "$repo_root" log -1 --format=%ct)
else
  build_timestamp=0
fi

if [[ -n "${BUILD_GIT_HASH:-}" ]]; then
  build_git_hash=$BUILD_GIT_HASH
elif git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  build_git_hash=$(git -C "$repo_root" rev-parse --short HEAD)
else
  build_git_hash=unknown
fi

zig_version=${ZIG_VERSION:-}
zlib_version=${ZLIB_VERSION:-}

if [[ -z "$zig_version" || -z "$zlib_version" ]]; then
  if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to read .github/versions.json" >&2
    exit 1
  fi

  zig_version=${zig_version:-$(jq -r '.tools.zig' "$repo_root/.github/versions.json")}
  zlib_version=${zlib_version:-$(jq -r '.dependencies.zlib' "$repo_root/.github/versions.json")}
fi

if command -v ops >/dev/null 2>&1; then
  ops_cmd=$(command -v ops)
elif [[ -x "$HOME/.ops/bin/ops" ]]; then
  ops_cmd="$HOME/.ops/bin/ops"
else
  echo "ops not found. Install ops first, then rerun this script." >&2
  exit 1
fi

if docker buildx version >/dev/null 2>&1; then
  docker_build=(docker buildx build)
else
  docker_build=(docker build)
fi

mkdir -p "$binary_dir" "$out_dir"
rm -rf "$binary_dir"
mkdir -p "$binary_dir"

echo "==> Building Ant musl binary for $arch with Docker ($docker_platform)"
"${docker_build[@]}" \
  --platform "$docker_platform" \
  "${docker_no_cache[@]}" \
  --build-arg "BUILD_TIMESTAMP=$build_timestamp" \
  --build-arg "BUILD_GIT_HASH=$build_git_hash" \
  --build-arg "ZIG_VERSION=$zig_version" \
  --build-arg "ZLIB_VERSION=$zlib_version" \
  --output "type=local,dest=$binary_dir" \
  -f "$repo_root/nanos/Dockerfile" \
  "$repo_root"

chmod +x "$binary_dir/ant"
if command -v file >/dev/null 2>&1; then
  file "$binary_dir/ant"
else
  ls -lh "$binary_dir/ant"
fi

echo "==> Ensuring ops runtime for $ops_arch"
shopt -s nullglob
if [[ "$ops_arch" == "arm64" ]]; then
  ops_kernels=("$HOME"/.ops/*-arm/kernel.img)
else
  ops_kernels=("$HOME"/.ops/[0-9]*/kernel.img)
fi
shopt -u nullglob

if (( ${#ops_kernels[@]} > 0 )); then
  echo "==> Using cached ops runtime ${ops_kernels[0]}"
elif [[ "$ops_arch" == "arm64" ]]; then
  "$ops_cmd" update --arm
else
  "$ops_cmd" update
fi

echo "==> Building Nanos image $image"
(
  cd "$binary_dir"
  "$ops_cmd" build ./ant -i "$image" --arch="$ops_arch"
)

if [[ -f "$HOME/.ops/images/${image}.img" ]]; then
  cp "$HOME/.ops/images/${image}.img" "$image_out"
elif [[ -f "$HOME/.ops/images/${image}" ]]; then
  cp "$HOME/.ops/images/${image}" "$image_out"
else
  echo "ops did not create $image under $HOME/.ops/images" >&2
  ls -la "$HOME/.ops/images" >&2 || true
  exit 1
fi

echo "==> Wrote:"
ls -lh "$binary_dir/ant" "$image_out"
