#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: packages/sandbox/build-sandbox.sh [options]

build a local Linux musl Ant binary in Alpine Docker, then build a patched
local Nanos kernel and write the sandbox image with Nanos mkfs.

environment:
  NANOS_REF
      Nanos git ref to fetch and patch. defaults to the CI-pinned revision.
  NANOS_URL
      Nanos git repository URL. defaults to https://github.com/nanovms/nanos.git.

options:
  --arch <native|x64|amd64|aarch64|arm64>
      target architecture. defaults to native.
  --no-cache
      pass --no-cache to docker build.
  --skip-docker
      reuse packages/sandbox/out/<arch>/binary/{ant,ca-certificates.crt} and only
      rebuild the Nanos kernel and image.
  -h, --help
      show this help.
USAGE
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
nanos_cache_dir="$script_dir/.cache"

arch=native
docker_no_cache=()
skip_docker=false
nanos_src="$nanos_cache_dir/nanos"
nanos_ref=${NANOS_REF:-ab823eb4bbcaf0874dc5e15568938bc02346d3d3}
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
    --skip-docker)
      skip_docker=true
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
    nanos_platform=pc
    nanos_arch=x86_64
    ;;
  aarch64|arm64)
    arch=aarch64
    docker_platform=linux/arm64
    nanos_platform=virt
    nanos_arch=aarch64
    ;;
  *)
    echo "unsupported architecture: $arch" >&2
    exit 1
    ;;
esac

out_dir="$repo_root/packages/sandbox/out/$arch"
binary_dir="$out_dir/binary"

case "$arch" in
  aarch64) cache_arch=aarch64 ;;
  x64) cache_arch=x64 ;;
esac

image_out="$out_dir/ant-sandbox-${cache_arch}.img"
kernel_out="$out_dir/ant-kernel-${cache_arch}.img"
if [[ -n "${BUILD_TIMESTAMP:-}" ]]; then
  build_timestamp=$BUILD_TIMESTAMP
else
  build_timestamp=$(git -C "$repo_root" log -1 --format=%ct)
fi

if [[ -n "${BUILD_GIT_HASH:-}" ]]; then
  build_git_hash=$BUILD_GIT_HASH
else
  build_git_hash=$(git -C "$repo_root" rev-parse HEAD)
fi

if [[ ! "$build_git_hash" =~ ^[0-9a-fA-F]{40,64}$ ]]; then
  echo "BUILD_GIT_HASH must be a full git hash: $build_git_hash" >&2
  exit 1
fi

if [[ -d "$HOME/.ant" ]]; then
  sandbox_cache_root="$HOME/.ant/sandbox"
else
  sandbox_cache_base="${XDG_CACHE_HOME:-$HOME/.cache}"
  sandbox_cache_root="$sandbox_cache_base/ant/sandbox"
fi

sandbox_cache_dir="$sandbox_cache_root/$build_git_hash"
sandbox_cache_image="$sandbox_cache_dir/ant-sandbox-${cache_arch}.img"
sandbox_cache_kernel="$sandbox_cache_dir/ant-kernel-${cache_arch}.img"

if [[ "$(uname -s)" == "Darwin" && -x /usr/bin/clang ]]; then
  nanos_cc=${NANOS_CC:-/usr/bin/clang}
elif command -v clang >/dev/null 2>&1; then
  nanos_cc=${NANOS_CC:-$(command -v clang)}
else
  nanos_cc=${NANOS_CC:-cc}
fi

nanos_cross_compile=${NANOS_CROSS_COMPILE:-}
if [[ -z "$nanos_cross_compile" ]]; then
  case "$nanos_arch" in
    aarch64)
      nanos_cross_candidates=(aarch64-elf- aarch64-none-elf- aarch64-linux-gnu-)
      ;;
    x86_64)
      nanos_cross_candidates=(x86_64-elf- x86_64-none-elf- x86_64-linux-gnu-)
      ;;
    *)
      nanos_cross_candidates=("${nanos_arch}-elf-" "${nanos_arch}-none-elf-" "${nanos_arch}-linux-gnu-")
      ;;
  esac

  for prefix in "${nanos_cross_candidates[@]}"; do
    if command -v "${prefix}ld" >/dev/null 2>&1; then
      nanos_cross_compile=$prefix
      break
    fi
  done
fi

if [[ -z "$nanos_cross_compile" ]]; then
  echo "Nanos cross linker not found. Set NANOS_CROSS_COMPILE to the tool prefix, e.g. aarch64-none-elf-." >&2
  exit 1
fi

mkdir -p "$nanos_cache_dir" "$binary_dir" "$out_dir"

if [[ "$skip_docker" == true ]]; then
  echo "==> reusing Ant musl binary for $arch"
  for exported in ant ca-certificates.crt; do
    if [[ ! -f "$binary_dir/$exported" ]]; then
      echo "missing $binary_dir/$exported; rerun without --skip-docker first" >&2
      exit 1
    fi
  done
else
  if docker buildx version >/dev/null 2>&1; then
    docker_build=(docker buildx build)
  else
    docker_build=(docker build)
  fi

  rm -rf "$binary_dir"
  mkdir -p "$binary_dir"

  echo "==> building Ant musl binary for $arch with Docker ($docker_platform)"
  "${docker_build[@]}" \
    --platform "$docker_platform" \
    "${docker_no_cache[@]}" \
    --build-arg "BUILD_TIMESTAMP=$build_timestamp" \
    --build-arg "BUILD_GIT_HASH=$build_git_hash" \
    --target export \
    --output "type=local,dest=$binary_dir" \
    -f "$repo_root/Dockerfile.musl" \
    "$repo_root"
fi

chmod +x "$binary_dir/ant"

echo "==> ant revision $build_git_hash"
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

if [[ ! "$nanos_ref" =~ ^[A-Za-z0-9._/-]+$ ]]; then
  echo "invalid NANOS_REF: $nanos_ref" >&2
  exit 1
fi

git -C "$nanos_src" remote set-url origin "$nanos_url"
git -C "$nanos_src" fetch --depth 1 origin "$nanos_ref"
nanos_revision=$(git -C "$nanos_src" rev-parse FETCH_HEAD)
echo "==> using Nanos $nanos_revision"

if command -v shasum >/dev/null 2>&1; then
  sha256_files=(shasum -a 256)
elif command -v sha256sum >/dev/null 2>&1; then
  sha256_files=(sha256sum)
else
  echo "shasum or sha256sum is required to hash Nanos patches" >&2
  exit 1
fi

patch_signature=$(cd "$repo_root" && "${sha256_files[@]}" packages/sandbox/patches/*.patch | "${sha256_files[@]}" | awk '{print $1}')
patched_source_signature=$(
  printf 'nanos=%s\n' "$nanos_revision"
  printf 'patches=%s\n' "$patch_signature"
)
nanos_patch_stamp="$nanos_cache_dir/patches-${cache_arch}.stamp"

if [[ -f "$nanos_patch_stamp" &&
      "$(cat "$nanos_patch_stamp")" == "$patched_source_signature" &&
      "$(git -C "$nanos_src" rev-parse HEAD)" == "$nanos_revision" ]]; then
  echo "==> reusing patched Nanos source"
else
  git -C "$nanos_src" reset --hard FETCH_HEAD >/dev/null
  for patch in "$repo_root"/packages/sandbox/patches/*.patch; do
    if git -C "$nanos_src" apply --check "$patch" >/dev/null 2>&1; then
      git -C "$nanos_src" apply "$patch"
    else
      echo "failed to apply Nanos patch: $patch" >&2
      exit 1
    fi
  done
  printf '%s\n' "$patched_source_signature" > "$nanos_patch_stamp"
fi

nanos_kernel="$nanos_src/output/platform/$nanos_platform/bin/kernel.img"
nanos_kernel_stamp="$nanos_cache_dir/kernel-${cache_arch}.stamp"

compiler_signature=$("$nanos_cc" --version 2>/dev/null | head -n 1 || printf '%s' "$nanos_cc")
linker_signature=$("${nanos_cross_compile}ld" --version 2>/dev/null | head -n 1 || printf '%s' "${nanos_cross_compile}ld")

kernel_signature=$(
  printf 'nanos=%s\n' "$nanos_revision"
  printf 'patches=%s\n' "$patch_signature"
  printf 'platform=%s\n' "$nanos_platform"
  printf 'arch=%s\n' "$nanos_arch"
  printf 'cc=%s\n' "$compiler_signature"
  printf 'cross=%s\n' "$nanos_cross_compile"
  printf 'ld=%s\n' "$linker_signature"
)

if [[ -f "$nanos_kernel" && -f "$nanos_kernel_stamp" ]] &&
   [[ "$(cat "$nanos_kernel_stamp")" == "$kernel_signature" ]]; then
  echo "==> reusing patched Nanos kernel for $nanos_platform/$nanos_arch"
elif [[ -f "$nanos_kernel" && ! -f "$nanos_kernel_stamp" ]]; then
  echo "==> reusing existing patched Nanos kernel for $nanos_platform/$nanos_arch"
  printf '%s\n' "$kernel_signature" > "$nanos_kernel_stamp"
else
  env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS \
    make -C "$nanos_src" PLATFORM="$nanos_platform" ARCH="$nanos_arch" CC="$nanos_cc" CROSS_COMPILE="$nanos_cross_compile" kernel
  printf '%s\n' "$kernel_signature" > "$nanos_kernel_stamp"
fi

if [[ ! -f "$nanos_kernel" ]]; then
  echo "Nanos kernel build did not create $nanos_kernel" >&2
  exit 1
fi

cp "$nanos_kernel" "$kernel_out"

echo "==> building Nanos mkfs"
env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS make -C "$nanos_src" mkfs

echo "==> writing sandbox image"
"$script_dir/make-image.sh" \
  "$nanos_src/output/tools/bin/mkfs" \
  "$binary_dir/ant" \
  "$binary_dir/ca-certificates.crt" \
  "$image_out"

find "$out_dir" -maxdepth 1 -type f \( \
  -name 'ant-sandbox.img' -o \
  -name 'ant-sandbox-[0-9]*.img' -o \
  -name 'nanos-kernel*.img' \
\) -delete
rm -f "$nanos_cache_dir/ant-kernel-arm.img" "$nanos_cache_dir/ant-kernel-x64.img"

echo "==> wrote:"
ls -lh "$binary_dir/ant" "$image_out" "$kernel_out"

mkdir -p "$sandbox_cache_dir"
cp "$image_out" "$sandbox_cache_image"
cp "$kernel_out" "$sandbox_cache_kernel"

echo "==> cached sandbox assets:"
ls -lh "$sandbox_cache_image" "$sandbox_cache_kernel"
