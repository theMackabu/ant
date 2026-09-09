#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

wraps=(
  vendor/crprintf.wrap
  vendor/minicoro.wrap
  vendor/mir.wrap
  vendor/uuidv7.wrap
  vendor/wirecall.wrap
)

wrap_value() {
  local wrap="$1"
  local key="$2"
  awk -F '[[:space:]]*=[[:space:]]*' -v key="$key" '$1 == key { print $2; exit }' "$wrap"
}

replace_revision() {
  local wrap="$1"
  local revision="$2"
  local tmp
  tmp="$(mktemp)"
  awk -v revision="$revision" '
    BEGIN { replaced = 0 }
    /^revision[[:space:]]*=/ {
      print "revision = " revision
      replaced = 1
      next
    }
    { print }
    END { if (!replaced) exit 2 }
  ' "$wrap" > "$tmp"
  mv "$tmp" "$wrap"
}

for wrap in "${wraps[@]}"; do
  url="$(wrap_value "$wrap" url)"
  if [[ -z "$url" ]]; then
    echo "FATAL: could not read url from $wrap" >&2
    exit 1
  fi

  revision="$(git ls-remote "$url" HEAD | awk '{ print $1 }')"
  if [[ -z "$revision" ]]; then
    echo "FATAL: could not resolve HEAD for $url" >&2
    exit 1
  fi

  echo "$wrap -> $revision"
  replace_revision "$wrap" "$revision"
done

echo "Updated vendor wrap revisions."
echo "Run 'packages/nix/update-vendor-hashes.js' to refresh the Nix vendor hash."
