#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

ant_bin="${ANT:-./build/ant}"
vendor_ts="packages/nix/src/vendor.ts"
vendor_nix="packages/nix/generated/vendor.nix"
fake_hash="sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="

replace_output_hash() {
  local hash="$1"
  local tmp
  tmp="$(mktemp)"
  if ! awk -v hash="$hash" '
    BEGIN { replaced = 0 }
    /^[[:space:]]*outputHash[[:space:]]*:/ {
      sub(/"[^"]+"/, "\"" hash "\"")
      replaced++
    }
    { print }
    END { if (replaced != 1) exit 2 }
  ' "$vendor_ts" > "$tmp"; then
    rm -f "$tmp"
    return 1
  fi
  mv "$tmp" "$vendor_ts"
  "$ant_bin" packages/nix/generate.ts
}

backup_ts="$(mktemp)"
backup_nix="$(mktemp)"
build_log="$(mktemp)"
cp "$vendor_ts" "$backup_ts"
cp "$vendor_nix" "$backup_nix"
completed=0
cleanup() {
  if [[ "$completed" -eq 0 ]]; then
    cp "$backup_ts" "$vendor_ts"
    cp "$backup_nix" "$vendor_nix"
  fi
  rm -f "$backup_ts" "$backup_nix" "$build_log"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

"$ant_bin" packages/nix/generate.ts --check
replace_output_hash "$fake_hash"

read -r -d '' vendor_expr <<'NIX' || true
let
  flake = builtins.getFlake (toString ./.);
  system = builtins.currentSystem;
  pkgs = import flake.inputs.nixpkgs { inherit system; };
in
pkgs.callPackage ./packages/nix/generated/vendor.nix {}
NIX

echo "Computing ant-vendor Nix hash from the current vendor wraps..."
set +e
nix build --impure --expr "$vendor_expr" --no-link --print-build-logs 2>&1 | tee "$build_log"
build_status="${PIPESTATUS[0]}"
set -e

if [[ "$build_status" -eq 0 ]]; then
  echo "FATAL: nix build unexpectedly succeeded with the fake vendor hash" >&2
  exit 1
fi

new_hash="$(awk '/got:[[:space:]]+sha256-/ { print $2 }' "$build_log" | tail -n 1)"
if [[ -z "$new_hash" ]]; then
  echo "FATAL: could not find the new Nix vendor hash in build output" >&2
  exit 1
fi

replace_output_hash "$new_hash"
completed=1

echo "$vendor_ts -> $new_hash"
echo "Run 'nix build' to verify the full package build."
