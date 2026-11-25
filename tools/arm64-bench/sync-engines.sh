#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
ENGINE_ROOT=${ANT_BENCH_ENGINE_ROOT:-"$REPO_ROOT/.cache/arm64-bench"}
ENGINES_ROOT="$ENGINE_ROOT/engines"
SOURCE_ROOT="$ENGINE_ROOT/src"
TEMP_ROOT="$ENGINE_ROOT/tmp"

if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
  echo "arm64-bench engine synchronization requires ARM64 macOS" >&2
  exit 1
fi

for command in cmake curl file git jq make ninja node shasum tar unzip; do
  command -v "$command" >/dev/null || { echo "missing engine update command: $command" >&2; exit 1; }
done

mkdir -p "$ENGINES_ROOT" "$SOURCE_ROOT" "$TEMP_ROOT"
work_dir=$(mktemp -d "$TEMP_ROOT/sync.XXXXXX")
cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

curl_args=(--fail --location --retry 3 --silent --show-error)
compiled=false

fetch() {
  curl "${curl_args[@]}" "$@"
}

github_fetch() {
  local github_args=("${curl_args[@]}" --header 'Accept: application/vnd.github+json')
  if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    github_args+=(--header "Authorization: Bearer $GITHUB_TOKEN")
  fi
  curl "${github_args[@]}" "$@"
}

safe_key() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_'
}

git_tag_revision() {
  local repository=$1 tag=$2 revision
  revision=$(git ls-remote "$repository" "refs/tags/$tag" "refs/tags/$tag^{}" 2>/dev/null \
    | awk 'END { print $1 }') || return 0
  if [[ "$revision" =~ ^[0-9a-f]{40}$ ]]; then
    printf '%s' "$revision"
  fi
}

activate() {
  local id=$1 install_dir=$2 executable_name=$3 channel=$4 version=$5 revision=${6:-}
  local executable="$install_dir/$executable_name"
  [[ -x "$executable" ]] || { echo "$id did not produce $executable" >&2; return 1; }
  file "$executable" | grep -Eq 'Mach-O.*(arm64|universal)' || {
    echo "$id executable is not ARM64 Mach-O: $(file "$executable")" >&2
    return 1
  }

  if [[ ! -f "$install_dir/metadata.json" ]]; then
    local executable_sha retrieved_at
    executable_sha=$(shasum -a 256 "$executable" | awk '{print $1}')
    retrieved_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    jq -n \
      --arg id "$id" \
      --arg channel "$channel" \
      --arg resolvedVersion "$version" \
      --arg sourceRevision "$revision" \
      --arg retrievedAt "$retrieved_at" \
      --arg executableSha256 "$executable_sha" \
      '{
        id: $id,
        channel: $channel,
        resolvedVersion: $resolvedVersion,
        sourceRevision: (if $sourceRevision == "" then null else $sourceRevision end),
        retrievedAt: $retrievedAt,
        executableSha256: $executableSha256
      }' > "$install_dir/metadata.json"
  elif [[ -n "$revision" ]] && [[ $(jq -r '.sourceRevision // ""' "$install_dir/metadata.json") != "$revision" ]]; then
    jq --arg sourceRevision "$revision" '.sourceRevision = $sourceRevision' \
      "$install_dir/metadata.json" > "$install_dir/.metadata.next"
    mv -f "$install_dir/.metadata.next" "$install_dir/metadata.json"
  fi

  local current_link="$ENGINES_ROOT/$id/current"
  local next_link="$ENGINES_ROOT/$id/.current.next"
  ln -sfn "$install_dir" "$next_link"
  mv -fh "$next_link" "$current_link"
  echo "$id: $version${revision:+ ($revision)}"
}

install_single_binary() {
  local id=$1 key=$2 url=$3 executable_name=$4 channel=$5 version=$6 revision=${7:-}
  local install_dir
  install_dir="$ENGINES_ROOT/$id/$(safe_key "$key")"
  if [[ ! -x "$install_dir/$executable_name" ]]; then
    local stage="$work_dir/$id"
    mkdir -p "$stage" "$ENGINES_ROOT/$id"
    fetch --output "$stage/$executable_name" "$url"
    chmod +x "$stage/$executable_name"
    mv "$stage" "$install_dir"
  fi
  activate "$id" "$install_dir" "$executable_name" "$channel" "$version" "$revision"
}

install_zip() {
  local id=$1 key=$2 url=$3 executable_name=$4 channel=$5 version=$6 revision=${7:-}
  local install_dir
  install_dir="$ENGINES_ROOT/$id/$(safe_key "$key")"
  if [[ ! -x "$install_dir/$executable_name" ]]; then
    local stage="$work_dir/$id"
    mkdir -p "$stage" "$ENGINES_ROOT/$id"
    fetch --output "$work_dir/$id.zip" "$url"
    unzip -q "$work_dir/$id.zip" -d "$stage"
    chmod +x "$stage/$executable_name"
    mv "$stage" "$install_dir"
  fi
  activate "$id" "$install_dir" "$executable_name" "$channel" "$version" "$revision"
}

warn() {
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "::warning::$*"
  else
    echo "warning: $*" >&2
  fi
}

sync_boa() {
  local release version url revision
  release=$(github_fetch https://api.github.com/repos/boa-dev/boa/releases/latest)
  version=$(jq -er '.tag_name' <<<"$release")
  url=$(jq -er '.assets[] | select(.name == "boa-aarch64-apple-darwin") | .browser_download_url' \
    <<<"$release")
  revision=$(git_tag_revision https://github.com/boa-dev/boa.git "$version")
  install_single_binary boa "$version" "$url" boa latest-release "$version" "$revision"
}

sync_v8() {
  local version url revision
  version=$(fetch https://storage.googleapis.com/chromium-v8/official/canary/v8-mac-arm64-rel-latest.json \
    | jq -er '.version')
  url="https://storage.googleapis.com/chromium-v8/official/canary/v8-mac-arm64-rel-$version.zip"
  revision=$(git_tag_revision https://chromium.googlesource.com/v8/v8.git "$version")
  install_zip v8 "$version" "$url" d8 canary "$version" "$revision"
}

sync_sm_baseline() {
  local version url
  version=$(fetch https://product-details.mozilla.org/1.0/firefox_history_development_releases.json \
    | jq -er 'to_entries | sort_by(.value) | last | .key')
  url="https://archive.mozilla.org/pub/firefox/releases/$version/jsshell/jsshell-mac.zip"
  install_zip sm-baseline "$version" "$url" js development-release "$version"
}

sync_kiesel() {
  local revision reported
  revision=$(git ls-remote https://codeberg.org/kiesel-js/kiesel.git HEAD | awk '{print $1}')
  [[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid Kiesel revision" >&2; return 1; }
  install_single_binary kiesel "$revision" \
    https://files.kiesel.dev/kiesel-macos-aarch64-releasefast kiesel main-snapshot \
    "$revision" "$revision"
  reported=$("$ENGINES_ROOT/kiesel/current/kiesel" --version 2>&1)
  [[ "$reported" == *"${revision:0:9}"* ]] || {
    echo "Kiesel snapshot does not match resolved HEAD $revision: $reported" >&2
    return 1
  }
}

libjs_runs() {
  [[ -x "$1/bin/js" ]] || return 1
  : > "$work_dir/libjs-probe.js"
  "$1/bin/js" "$work_dir/libjs-probe.js" >/dev/null 2>&1
}

sync_libjs() {
  local revision install source stage
  revision=$(git ls-remote https://github.com/LadybirdBrowser/ladybird.git HEAD | awk '{print $1}')
  [[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid LibJS revision" >&2; return 1; }
  install="$ENGINES_ROOT/libjs/$revision"
  if ! libjs_runs "$install"; then
    source="$SOURCE_ROOT/ladybird"
    stage="$work_dir/libjs"
    if [[ ! -d "$source/.git" ]]; then
      git clone --filter=blob:none https://github.com/LadybirdBrowser/ladybird.git "$source"
    fi
    git -C "$source" fetch --depth 1 origin "$revision"
    git -C "$source" checkout --detach "$revision"
    : > "$work_dir/compiled"
    (
      cd "$source"
      if command -v brew >/dev/null && brew --prefix rustup >/dev/null 2>&1; then
        rustup_prefix=$(brew --prefix rustup)
        export PATH="$rustup_prefix/bin:$PATH"
      fi
      python3 Meta/ladybird.py vcpkg --preset Distribution
      cmake --fresh --preset Distribution -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DENABLE_GUI_TARGETS=OFF \
        -DVCPKG_OVERLAY_PORTS="$SCRIPT_DIR/vcpkg-overlays"
      ninja -C Build/distribution js
      cmake --install Build/distribution --component js --prefix "$stage"
    )
    libjs_runs "$stage" || { echo "LibJS does not run after install" >&2; return 1; }
    mkdir -p "$ENGINES_ROOT/libjs"
    rm -rf "$install"
    mv "$stage" "$install"
  fi
  activate libjs "$install" bin/js main "$revision" "$revision"
}

sync_duktape() {
  local version=2.7.0 revision install stage
  revision=$(git_tag_revision https://github.com/svaarala/duktape.git "v$version")
  install="$ENGINES_ROOT/duktape/$version"
  if [[ ! -x "$install/duk" ]]; then
    stage="$work_dir/duktape"
    mkdir -p "$stage" "$ENGINES_ROOT/duktape"
    fetch --output "$work_dir/duktape.tar.xz" \
      "https://github.com/svaarala/duktape/releases/download/v$version/duktape-$version.tar.xz"
    tar -xJf "$work_dir/duktape.tar.xz" --strip-components=1 -C "$stage"
    : > "$work_dir/compiled"
    make -C "$stage" -f Makefile.cmdline
    mkdir -p "$install"
    cp "$stage/duk" "$install/duk"
    chmod +x "$install/duk"
  fi
  activate duktape "$install" duk latest-release "$version" "$revision"
}

sync_quickjs() {
  local version install stage
  version=$(fetch https://bellard.org/quickjs/binary_releases/LATEST.json | jq -er '.version')
  install="$ENGINES_ROOT/quickjs/$(safe_key "$version")"
  if [[ ! -x "$install/qjs" ]]; then
    stage="$work_dir/quickjs"
    mkdir -p "$stage" "$ENGINES_ROOT/quickjs"
    fetch --output "$work_dir/quickjs.tar.xz" \
      "https://bellard.org/quickjs/quickjs-$version.tar.xz"
    tar -xJf "$work_dir/quickjs.tar.xz" --strip-components=1 -C "$stage"
    : > "$work_dir/compiled"
    make -C "$stage" -j"$(sysctl -n hw.ncpu)" qjs
    mkdir -p "$install"
    cp "$stage/qjs" "$install/qjs"
    chmod +x "$install/qjs"
  fi
  activate quickjs "$install" qjs latest-release "$version"
}

sync_porffor() {
  local release version url revision install stage reported
  release=$(github_fetch https://api.github.com/repos/CanadaHonk/porffor/releases/latest)
  version=$(jq -er '.tag_name' <<<"$release")
  url=$(jq -er '.assets[] | select(.name == "porffor-darwin-arm64.tar.gz") | .browser_download_url' \
    <<<"$release")
  revision=$(git_tag_revision https://github.com/CanadaHonk/porffor.git "$version")
  install="$ENGINES_ROOT/porffor/$(safe_key "$version")"
  if [[ ! -x "$install/porf" ]]; then
    stage="$work_dir/porffor"
    mkdir -p "$stage" "$ENGINES_ROOT/porffor"
    fetch --output "$work_dir/porffor.tar.gz" "$url"
    tar -xzf "$work_dir/porffor.tar.gz" -C "$stage"
    chmod +x "$stage/porf"
    mv "$stage" "$install"
  fi
  activate porffor "$install" porf latest-release "$version" "$revision"
  reported=$("$install/porf" --version 2>&1)
  if [[ -n "$revision" ]] && [[ "$reported" != *"${revision:0:7}"* ]]; then
    echo "Porffor asset does not match release commit $revision: $reported" >&2
    return 1
  fi
}

github_release_asset() {
  local repository=$1 asset=$2 release
  release=$(github_fetch "https://api.github.com/repos/$repository/releases/latest")
  jq -er --arg asset "$asset" \
    '.tag_name, (.assets[] | select(.name == $asset) | .browser_download_url)' <<<"$release"
}

git_head_revision() {
  local revision
  revision=$(git ls-remote "$1" HEAD | awk '{print $1}')
  [[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid HEAD revision for $1" >&2; return 1; }
  printf '%s' "$revision"
}

checkout_revision() {
  local repository=$1 source=$2 revision=$3
  if [[ ! -d "$source/.git" ]]; then
    git clone --filter=blob:none "$repository" "$source"
  fi
  git -C "$source" fetch --depth 1 origin "$revision"
  git -C "$source" checkout --detach --force "$revision"
}

sync_bun_jitless() {
  local version url revision
  { read -r version; read -r url; } < <(github_release_asset oven-sh/bun bun-darwin-aarch64.zip)
  revision=$(git_tag_revision https://github.com/oven-sh/bun.git "$version")
  install_zip bun-jitless "$version" "$url" bun-darwin-aarch64/bun latest-release "$version" "$revision"
}

sync_quickjs_ng() {
  local version url revision
  { read -r version; read -r url; } < <(github_release_asset quickjs-ng/quickjs qjs-darwin-arm64)
  revision=$(git_tag_revision https://github.com/quickjs-ng/quickjs.git "$version")
  install_single_binary quickjs-ng "$version" "$url" qjs latest-release "$version" "$revision"
}

sync_escargot() {
  local version url revision
  { read -r version; read -r url; } < <(github_release_asset Samsung/escargot escargot-mac64arm.zip)
  revision=$(git_tag_revision https://github.com/Samsung/escargot.git "$version")
  install_zip escargot "$version" "$url" escargot latest-release "$version" "$revision"
}

sync_hermes() {
  local repository=https://github.com/facebook/hermes.git release version revision install source
  release=$(fetch https://registry.npmjs.org/hermes-compiler/latest-v1)
  version=$(jq -er '.version' <<<"$release")
  revision=$(jq -er '.gitHead' <<<"$release")
  [[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid Hermes release revision" >&2; return 1; }
  install="$ENGINES_ROOT/hermes/$(safe_key "$version")"
  if [[ ! -x "$install/bin/hermes" ]]; then
    source="$SOURCE_ROOT/hermes"
    checkout_revision "$repository" "$source" "$revision"
    : > "$work_dir/compiled"
    cmake --fresh -S "$source" -B "$source/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
    ninja -C "$source/build" hermes
    mkdir -p "$work_dir/hermes/bin" "$ENGINES_ROOT/hermes"
    cp "$source/build/bin/hermes" "$work_dir/hermes/bin/hermes"
    rm -rf "$install"
    mv "$work_dir/hermes" "$install"
  fi
  activate hermes "$install" bin/hermes latest-v1-release "$version" "$revision"
}

sync_goja() {
  local revision host_hash install build
  revision=$(git_head_revision https://github.com/dop251/goja.git)
  host_hash=$(shasum -a 256 "$SCRIPT_DIR/goja/main.go" | cut -c1-12)
  install="$ENGINES_ROOT/goja/$revision-$host_hash"
  if [[ ! -x "$install/goja" ]]; then
    command -v go >/dev/null || { echo "missing go for goja" >&2; return 1; }
    : > "$work_dir/compiled"
    build="$work_dir/goja-build"
    mkdir -p "$build" "$work_dir/goja" "$ENGINES_ROOT/goja"
    cp "$SCRIPT_DIR/goja/main.go" "$build/main.go"
    (
      cd "$build"
      go mod init ant-arm64-bench/goja
      go get "github.com/dop251/goja@$revision"
      go build -trimpath -o "$work_dir/goja/goja" .
    )
    rm -rf "$install"
    mv "$work_dir/goja" "$install"
  fi
  activate goja "$install" goja main "$revision" "$revision"
}

engine_ids=(boa v8 sm-baseline bun-jitless kiesel libjs duktape quickjs quickjs-ng porffor hermes escargot goja)
metadata_files=()
for id in "${engine_ids[@]}"; do
  metadata="$ENGINES_ROOT/$id/current/metadata.json"
  set +e
  (
    set -e
    "sync_${id//-/_}"
  )
  status=$?
  set -e
  if (( status != 0 )); then
    if [[ -f "$metadata" ]]; then
      warn "$id failed to synchronize; keeping $(jq -r '.resolvedVersion' "$metadata")"
    else
      warn "$id failed to synchronize and has no previous install"
    fi
  fi
  if [[ -f "$metadata" ]]; then
    metadata_files+=("$metadata")
  fi
done
if [[ -e "$work_dir/compiled" ]]; then
  compiled=true
fi

jq -s \
  --arg resolvedAt "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  '{schema: 1, resolvedAt: $resolvedAt, engines: (map({key: .id, value: .}) | from_entries)}' \
  ${metadata_files[@]+"${metadata_files[@]}"} > "$ENGINE_ROOT/engine-state.json"

node "$SCRIPT_DIR/verify-engines.mjs" --engine-root "$ENGINE_ROOT" --external-only

echo "engines compiled: $compiled"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "compiled=$compiled" >> "$GITHUB_OUTPUT"
fi
