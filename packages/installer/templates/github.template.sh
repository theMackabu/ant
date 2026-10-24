github_api() {
  local path="$1"
  local output="$2"
  local auth=""

  if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    auth="header = \"Authorization: Bearer $GITHUB_TOKEN\""
  fi

  printf '%s\n' "$auth" | curl --config - --fail --silent --show-error \
    --connect-timeout 10 --speed-limit 1 --speed-time 30 \
    --header 'Accept: application/vnd.github+json' \
    --header 'X-GitHub-Api-Version: 2022-11-28' \
    --output "$output" "https://api.github.com/repos/$github_repo/$path"
}

release_asset_field() {
  local json="$1"
  local asset="$2"
  local field="$3"

  awk -v asset="$asset" -v field="$field" '
    { json = json " " $0 }
    END {
      while (match(json, /"([^"\\]|\\.)*"[ \t\r]*:[ \t\r]*"([^"\\]|\\.)*"|"([^"\\]|\\.)*"|[{}]/)) {
        token = substr(json, RSTART, RLENGTH)
        json = substr(json, RSTART + RLENGTH)
        if (token == "{") {
          depth++
          names[depth] = ""
          values[depth] = ""
        } else if (token == "}") {
          if (names[depth] == asset && values[depth] != "") {
            print values[depth]
            exit
          }
          depth--
        } else if (match(token, /^"([^"\\]|\\.)*"[ \t\r]*:[ \t\r]*"/)) {
          key = substr(token, 2, RLENGTH - 1)
          value = substr(token, RLENGTH + 1, length(token) - RLENGTH - 1)
          sub(/"[ \t\r]*:[ \t\r]*"$/, "", key)
          if (key == "name") names[depth] = value
          if (key == field) values[depth] = value
        }
      }
    }
  ' "$json"
}

sha256_of() {
  local file="$1"

  if has_cmd sha256sum; then
    sha256sum <"$file" | awk '{ print $1 }'
  elif has_cmd shasum; then
    shasum -a 256 <"$file" | awk '{ print $1 }'
  elif has_cmd openssl; then
    openssl dgst -sha256 <"$file" | awk '{ print $NF }'
  else
    return 1
  fi
}

zip_extractor() {
  if has_cmd unzip; then
    printf 'unzip'
  elif has_cmd bsdtar; then
    printf 'bsdtar'
  elif [[ "$(tar --version 2>/dev/null)" == *bsdtar* ]]; then
    printf 'tar'
  elif has_cmd python3; then
    printf 'python3'
  elif has_cmd busybox; then
    printf 'busybox'
  else
    return 1
  fi
}

extract_zip() {
  local extractor="$1"
  local zip="$2"
  local dir="$3"

  mkdir -p "$dir" || return 1
  case "$extractor" in
    unzip) unzip -q -o "$zip" -d "$dir" ;;
    bsdtar|tar) "$extractor" -xf "$zip" -C "$dir" ;;
    python3) python3 -m zipfile -e "$zip" "$dir" ;;
    busybox) busybox unzip -q -o "$zip" -d "$dir" ;;
  esac
}

install_from_github() {
  local target="$1"
  local stage="$2"
  local asset="${github_asset//\{target\}/$target}"
  local release="$stage/release.json"
  local zip="$stage/$asset"
  local extractor url digest tag actual entry

  extractor="$(zip_extractor)" ||
    die "unzip, bsdtar, python3, or busybox is required to install $app_name from GitHub"

  run_step "Looking up latest release" github_api releases/latest "$release" ||
    die "Failed to look up the latest $app_name release on GitHub (set GITHUB_TOKEN if rate limited)"

  url="$(release_asset_field "$release" "$asset" browser_download_url)"
  digest="$(release_asset_field "$release" "$asset" digest)"
  if [[ -z "$url" ]]; then
    die "The latest $app_name release on GitHub has no $asset"
  fi
  tag="${url%/*}"
  detail "release: ${tag##*/}"

  run_download "Downloading Ant" "$zip" "$url" ||
    die "Failed to download $app_name from \"$url\""

  if [[ -z "$digest" ]]; then
    detail "release lists no digest for $asset, skipping checksum"
  else
    actual="$(sha256_of "$zip")" ||
      die "sha256sum, shasum, or openssl is required to verify $asset"
    if [[ "$digest" != "sha256:$actual" ]]; then
      die "Checksum mismatch for $asset: expected ${digest#sha256:}, got $actual"
    fi
  fi

  run_step "Extracting $asset" extract_zip "$extractor" "$zip" "$stage/extract" ||
    die "Failed to extract $asset with $extractor"

  entry="$stage/extract/$binary_name"
  if [[ ! -f "$entry" ]]; then
    entry="$entry.exe"
  fi
  mv -f "$entry" "$stage/$binary_name" ||
    die "$asset does not contain $binary_name"
}
