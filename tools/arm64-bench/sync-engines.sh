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

github_release=$(github_fetch https://api.github.com/repos/boa-dev/boa/releases/latest)
boa_version=$(jq -er '.tag_name' <<<"$github_release")
boa_url=$(jq -er '.assets[] | select(.name == "boa-aarch64-apple-darwin") | .browser_download_url' \
  <<<"$github_release")
boa_revision=$(git_tag_revision https://github.com/boa-dev/boa.git "$boa_version")
install_single_binary boa "$boa_version" "$boa_url" boa latest-release "$boa_version" "$boa_revision"

v8_version=$(fetch https://storage.googleapis.com/chromium-v8/official/canary/v8-mac-arm64-rel-latest.json \
  | jq -er '.version')
v8_url="https://storage.googleapis.com/chromium-v8/official/canary/v8-mac-arm64-rel-$v8_version.zip"
v8_revision=$(git_tag_revision https://chromium.googlesource.com/v8/v8.git "$v8_version")
install_zip v8-jitless "$v8_version" "$v8_url" d8 canary "$v8_version" "$v8_revision"

sm_version=$(fetch https://product-details.mozilla.org/1.0/firefox_history_development_releases.json \
  | jq -er 'to_entries | sort_by(.value) | last | .key')
sm_url="https://archive.mozilla.org/pub/firefox/releases/$sm_version/jsshell/jsshell-mac.zip"
install_zip sm-jitless "$sm_version" "$sm_url" js development-release "$sm_version"

kiesel_revision=$(git ls-remote https://codeberg.org/kiesel-js/kiesel.git HEAD | awk '{print $1}')
[[ "$kiesel_revision" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid Kiesel revision" >&2; exit 1; }
install_single_binary kiesel "$kiesel_revision" \
  https://files.kiesel.dev/kiesel-macos-aarch64-releasefast kiesel main-snapshot \
  "$kiesel_revision" "$kiesel_revision"
kiesel_reported_version=$("$ENGINES_ROOT/kiesel/current/kiesel" --version 2>&1)
[[ "$kiesel_reported_version" == *"${kiesel_revision:0:9}"* ]] || {
  echo "Kiesel snapshot does not match resolved HEAD $kiesel_revision: $kiesel_reported_version" >&2
  exit 1
}

libjs_revision=$(git ls-remote https://github.com/LadybirdBrowser/ladybird.git HEAD | awk '{print $1}')
[[ "$libjs_revision" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid LibJS revision" >&2; exit 1; }
libjs_install="$ENGINES_ROOT/libjs/$libjs_revision"
if [[ ! -x "$libjs_install/js" ]]; then
  libjs_source="$SOURCE_ROOT/ladybird"
  if [[ ! -d "$libjs_source/.git" ]]; then
    git clone --filter=blob:none https://github.com/LadybirdBrowser/ladybird.git "$libjs_source"
  fi
  git -C "$libjs_source" fetch --depth 1 origin "$libjs_revision"
  git -C "$libjs_source" checkout --detach "$libjs_revision"
  (
    cd "$libjs_source"
    if command -v brew >/dev/null && brew --prefix rustup >/dev/null 2>&1; then
      rustup_prefix=$(brew --prefix rustup)
      export PATH="$rustup_prefix/bin:$PATH"
    fi
    python3 Meta/Utils/build_vcpkg.py
    cmake --preset Distribution -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DENABLE_GUI_TARGETS=OFF
    ninja -C Build/distribution js
  )
  mkdir -p "$libjs_install"
  cp "$libjs_source/Build/distribution/bin/js" "$libjs_install/js"
  chmod +x "$libjs_install/js"
fi
activate libjs "$libjs_install" js main "$libjs_revision" "$libjs_revision"

duktape_version=2.7.0
duktape_revision=$(git_tag_revision https://github.com/svaarala/duktape.git "v$duktape_version")
duktape_install="$ENGINES_ROOT/duktape/$duktape_version"
if [[ ! -x "$duktape_install/duk" ]]; then
  duktape_stage="$work_dir/duktape"
  mkdir -p "$duktape_stage" "$ENGINES_ROOT/duktape"
  fetch --output "$work_dir/duktape.tar.xz" \
    "https://github.com/svaarala/duktape/releases/download/v$duktape_version/duktape-$duktape_version.tar.xz"
  tar -xJf "$work_dir/duktape.tar.xz" --strip-components=1 -C "$duktape_stage"
  make -C "$duktape_stage" -f Makefile.cmdline
  mkdir -p "$duktape_install"
  cp "$duktape_stage/duk" "$duktape_install/duk"
  chmod +x "$duktape_install/duk"
fi
activate duktape "$duktape_install" duk latest-release "$duktape_version" "$duktape_revision"

quickjs_version=$(fetch https://bellard.org/quickjs/binary_releases/LATEST.json | jq -er '.version')
quickjs_install="$ENGINES_ROOT/quickjs/$(safe_key "$quickjs_version")"
if [[ ! -x "$quickjs_install/qjs" ]]; then
  quickjs_stage="$work_dir/quickjs"
  mkdir -p "$quickjs_stage" "$ENGINES_ROOT/quickjs"
  fetch --output "$work_dir/quickjs.tar.xz" \
    "https://bellard.org/quickjs/quickjs-$quickjs_version.tar.xz"
  tar -xJf "$work_dir/quickjs.tar.xz" --strip-components=1 -C "$quickjs_stage"
  make -C "$quickjs_stage" -j"$(sysctl -n hw.ncpu)" qjs
  mkdir -p "$quickjs_install"
  cp "$quickjs_stage/qjs" "$quickjs_install/qjs"
  chmod +x "$quickjs_install/qjs"
fi
activate quickjs "$quickjs_install" qjs latest-release "$quickjs_version"

porffor_release=$(github_fetch https://api.github.com/repos/CanadaHonk/porffor/releases/latest)
porffor_version=$(jq -er '.tag_name' <<<"$porffor_release")
porffor_url=$(jq -er '.assets[] | select(.name == "porffor-darwin-arm64.tar.gz") | .browser_download_url' \
  <<<"$porffor_release")
porffor_revision=$(git_tag_revision https://github.com/CanadaHonk/porffor.git "$porffor_version")
porffor_install="$ENGINES_ROOT/porffor/$(safe_key "$porffor_version")"
if [[ ! -x "$porffor_install/porf" ]]; then
  porffor_stage="$work_dir/porffor"
  mkdir -p "$porffor_stage" "$ENGINES_ROOT/porffor"
  fetch --output "$work_dir/porffor.tar.gz" "$porffor_url"
  tar -xzf "$work_dir/porffor.tar.gz" -C "$porffor_stage"
  chmod +x "$porffor_stage/porf"
  mv "$porffor_stage" "$porffor_install"
fi
activate porffor "$porffor_install" porf latest-release "$porffor_version" "$porffor_revision"
porffor_reported_version=$("$porffor_install/porf" --version 2>&1)
if [[ -n "$porffor_revision" ]] && [[ "$porffor_reported_version" != *"${porffor_revision:0:7}"* ]]; then
  echo "Porffor asset does not match release commit $porffor_revision: $porffor_reported_version" >&2
  exit 1
fi

metadata_files=()
for id in boa v8-jitless sm-jitless kiesel libjs duktape quickjs porffor; do
  metadata_files+=("$ENGINES_ROOT/$id/current/metadata.json")
done
jq -s \
  --arg resolvedAt "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  '{schema: 1, resolvedAt: $resolvedAt, engines: (map({key: .id, value: .}) | from_entries)}' \
  "${metadata_files[@]}" > "$ENGINE_ROOT/engine-state.json"

node "$SCRIPT_DIR/verify-engines.mjs" --engine-root "$ENGINE_ROOT" --external-only
