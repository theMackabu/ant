#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

vendor_nix="packages/nix/vendor.nix"
fake_hash="sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="

replace_output_hash() {
  local hash="$1"
  local tmp
  tmp="$(mktemp)"
  awk -v hash="$hash" '
    BEGIN { replaced = 0 }
    /^[[:space:]]*outputHash[[:space:]]*=/ {
      sub(/"[^"]+"/, "\"" hash "\"")
      replaced = 1
    }
    { print }
    END { if (!replaced) exit 2 }
  ' "$vendor_nix" > "$tmp"
  mv "$tmp" "$vendor_nix"
}

backup="$(mktemp)"
build_log="$(mktemp)"
cp "$vendor_nix" "$backup"
cleanup() {
  rm -f "$backup" "$build_log"
}
restore_vendor_hash() {
  cp "$backup" "$vendor_nix"
  cleanup
}
restore_vendor_hash_and_exit() {
  restore_vendor_hash
  exit 130
}
trap restore_vendor_hash_and_exit INT TERM

replace_output_hash "$fake_hash"

read -r -d '' vendor_expr <<'EOF' || true
let
  flake = builtins.getFlake (toString ./.);
  system = builtins.currentSystem;
  pkgs = import flake.inputs.nixpkgs { inherit system; };
in
pkgs.callPackage ./packages/nix/vendor.nix {}
EOF

echo "Computing ant-vendor Nix hash from the current vendor wraps..."
set +e
nix build --impure --expr "$vendor_expr" --no-link --print-build-logs 2>&1 | tee "$build_log"
build_status="${PIPESTATUS[0]}"
set -e
trap - INT TERM

cp "$backup" "$vendor_nix"

if [[ "$build_status" -eq 0 ]]; then
  cleanup
  echo "FATAL: nix build unexpectedly succeeded with the fake vendor hash" >&2
  exit 1
fi

new_hash="$(awk '/got:[[:space:]]+sha256-/ { print $2 }' "$build_log" | tail -n 1)"
if [[ -z "$new_hash" ]]; then
  cleanup
  echo "FATAL: could not find the new Nix vendor hash in build output" >&2
  exit 1
fi

replace_output_hash "$new_hash"
cleanup

echo "$vendor_nix -> $new_hash"
echo "Run 'nix build' to verify the full package build."
