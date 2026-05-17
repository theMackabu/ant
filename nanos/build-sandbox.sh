#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: nanos/build-sandbox.sh [options]

build a local Linux musl Ant binary in Alpine Docker, then build a Nanos
sandbox image with ops and a patched local Nanos kernel.

options:
  --arch <native|x64|amd64|aarch64|arm64>
      target architecture. defaults to native.
  --no-cache
      pass --no-cache to docker build.
  -h, --help
      show this help.
USAGE
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
nanos_cache_dir="$script_dir/.cache"
sandbox_config_dir=
real_home=${HOME:?}

cleanup() {
  if [[ -n "${sandbox_config_dir:-}" ]]; then rm -rf "$sandbox_config_dir"; fi
}
trap cleanup EXIT

arch=native
docker_no_cache=()
nanos_src="$nanos_cache_dir/nanos"
nanos_url=${NANOS_URL:-https://github.com/nanovms/nanos.git}

while (($#)); do
  case "$1" in
    --arch)
      arch=${2:-}
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
    nanos_platform=pc
    nanos_arch=x86_64
    image=ant-sandbox-x64
    ;;
  aarch64|arm64)
    arch=aarch64
    docker_platform=linux/arm64
    ops_arch=arm64
    nanos_platform=virt
    nanos_arch=aarch64
    image=ant-sandbox-aarch64
    ;;
  *)
    echo "unsupported architecture: $arch" >&2
    exit 1
    ;;
esac

out_dir="$repo_root/nanos/out/$arch"
binary_dir="$out_dir/binary"
image_out="$out_dir/ant-sandbox.img"

case "$arch" in
  aarch64) cache_arch=aarch64 ;;
  x64) cache_arch=x64 ;;
esac
if [[ -n "${BUILD_TIMESTAMP:-}" ]]; then
  build_timestamp=$BUILD_TIMESTAMP
else
  build_timestamp=$(git -C "$repo_root" log -1 --format=%ct)
fi

if [[ -n "${BUILD_GIT_HASH:-}" ]]; then
  build_git_hash=$BUILD_GIT_HASH
else
  build_git_hash=$(git -C "$repo_root" rev-parse --short HEAD)
fi

ant_base_version=$(tr -d '[:space:]' < "$repo_root/meson/ant.version")
if [[ -z "$ant_base_version" ]]; then
  echo "meson/ant.version is empty" >&2
  exit 1
fi

ant_version="${ant_base_version}.${build_timestamp}-g${build_git_hash}"
safe_version=${ant_version//[^A-Za-z0-9._-]/-}

if [[ -d "$HOME/.ant" ]]; then
  sandbox_cache_root="$HOME/.ant/sandbox"
else
  sandbox_cache_base="${XDG_CACHE_HOME:-$HOME/.cache}"
  sandbox_cache_root="$sandbox_cache_base/ant/sandbox"
fi

sandbox_cache_dir="$sandbox_cache_root/$safe_version"
sandbox_cache_image="$sandbox_cache_dir/ant-sandbox-${cache_arch}.img"
sandbox_cache_kernel="$sandbox_cache_dir/ant-kernel-${cache_arch}.img"

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
elif [[ -x "$real_home/.ops/bin/ops" ]]; then
  ops_cmd="$real_home/.ops/bin/ops"
else
  echo "ops not found. Install ops first, then rerun this script." >&2
  exit 1
fi

if [[ "$(uname -s)" == "Darwin" && -x /usr/bin/clang ]]; then
  nanos_cc=${NANOS_CC:-/usr/bin/clang}
elif command -v clang >/dev/null 2>&1; then
  nanos_cc=${NANOS_CC:-$(command -v clang)}
else
  nanos_cc=${NANOS_CC:-cc}
fi

if docker buildx version >/dev/null 2>&1; then
  docker_build=(docker buildx build)
else
  docker_build=(docker build)
fi

mkdir -p "$nanos_cache_dir" "$binary_dir" "$out_dir"
rm -rf "$binary_dir"
mkdir -p "$binary_dir"

echo "==> building Ant musl binary for $arch with Docker ($docker_platform)"
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

versioned_image_out="$out_dir/ant-sandbox-${safe_version}.img"
kernel_out="$out_dir/nanos-kernel.img"
versioned_kernel_out="$out_dir/nanos-kernel-${safe_version}.img"

echo "==> ant version $ant_version"
if command -v file >/dev/null 2>&1; then
  file "$binary_dir/ant"
else
  ls -lh "$binary_dir/ant"
fi

echo "==> building patched Nanos kernel ($nanos_platform/$nanos_arch)"

if [[ ! -d "$nanos_src/.git" ]]; then
  mkdir -p "$(dirname "$nanos_src")"
  git clone "$nanos_url" "$nanos_src"
fi

for patch in "$repo_root"/nanos/patches/*.patch; do
  if git -C "$nanos_src" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$nanos_src" apply "$patch"
  elif git -C "$nanos_src" apply --check -R "$patch" >/dev/null 2>&1; then
    echo "==> Patch already applied: $(basename "$patch")"
  else
    echo "failed to apply or verify Nanos patch: $patch" >&2
    exit 1
  fi
done

make -C "$nanos_src" PLATFORM="$nanos_platform" ARCH="$nanos_arch" CC="$nanos_cc" kernel
nanos_kernel="$nanos_src/output/platform/$nanos_platform/bin/kernel.img"
if [[ ! -f "$nanos_kernel" ]]; then
  echo "Nanos kernel build did not create $nanos_kernel" >&2
  exit 1
fi

echo "==> building Nanos image $image"
sandbox_config_dir=$(mktemp -d "$nanos_cache_dir/ops-config.XXXXXX")
ops_state="$sandbox_config_dir/.ops"
mkdir -p "$ops_state/images"

sandbox_ops_config="$sandbox_config_dir/ops-sandbox.json"
cat > "$sandbox_ops_config" <<JSON
{
  "Kernel": "$nanos_kernel",
  "NanosVersion": "$ant_version"
}
JSON

(
  cd "$binary_dir"
  OPS_HOME="$sandbox_config_dir" "$ops_cmd" build ./ant \
    -c "$sandbox_ops_config" \
    -i "$image" \
    --arch="$ops_arch" \
    --consoles +pl011 \
    --consoles +16550 \
    --mounts "%0:/workspace:ro" \
    -a "--sandbox-daemon"
)

if [[ -f "$ops_state/images/${image}.img" ]]; then
  cp "$ops_state/images/${image}.img" "$image_out"
elif [[ -f "$ops_state/images/${image}" ]]; then
  cp "$ops_state/images/${image}" "$image_out"
else
  echo "ops did not create $image under temporary ops image dir" >&2
  ls -la "$ops_state/images" >&2 || true
  exit 1
fi

cp "$image_out" "$versioned_image_out"
cp "$nanos_kernel" "$kernel_out"
cp "$kernel_out" "$versioned_kernel_out"

echo "==> wrote:"
ls -lh "$binary_dir/ant" "$image_out" "$versioned_image_out" "$kernel_out" "$versioned_kernel_out"

mkdir -p "$sandbox_cache_dir"
cp "$image_out" "$sandbox_cache_image"
cp "$nanos_kernel" "$sandbox_cache_kernel"

echo "==> cached sandbox assets:"
ls -lh "$sandbox_cache_image" "$sandbox_cache_kernel"
