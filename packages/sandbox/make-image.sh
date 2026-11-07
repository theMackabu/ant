#!/usr/bin/env bash
set -euo pipefail

if (($# != 4)); then
  echo "usage: packages/sandbox/make-image.sh <nanos-mkfs> <ant-binary> <ca-bundle> <output.img>" >&2
  exit 2
fi

mkfs=$1
ant_binary=$2
ca_bundle=$3
image_out=$4

for input in "$mkfs" "$ant_binary" "$ca_bundle"; do
  if [[ ! -f "$input" ]]; then
    echo "missing input: $input" >&2
    exit 1
  fi
done

staging=$(mktemp -d "${TMPDIR:-/tmp}/ant-sandbox-image.XXXXXX")
trap 'rm -rf "$staging"' EXIT

printf 'root:x:0:0:root:/root:/bin/nobash\n' > "$staging/passwd"
printf 'nameserver  8.8.8.8\n' > "$staging/resolv.conf"
printf 'uniboot' > "$staging/hostname"

host_path() {
  local path
  path=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
  path=${path//\\/\\\\}
  path=${path//\"/\\\"}
  printf '(contents:(host:"%s"))' "$path"
}

cat > "$staging/manifest" <<MANIFEST
(
  children:(
    ant:$(host_path "$ant_binary")
    etc:(children:(
      passwd:$(host_path "$staging/passwd")
      resolv.conf:$(host_path "$staging/resolv.conf")
      ssl:(children:(certs:(children:(ca-certificates.crt:$(host_path "$ca_bundle")))))
    ))
    proc:(children:(sys:(children:(kernel:(children:(hostname:$(host_path "$staging/hostname")))))))
  )
  program:/ant
  arguments:[./ant --sandbox-daemon]
  environment:(PWD:/ USER:root SSL_CERT_FILE:/etc/ssl/certs/ca-certificates.crt)
  consoles:[+pl011 +16550]
)
MANIFEST

mkdir -p "$(dirname -- "$image_out")"
tmp_out="$image_out.tmp.$$"
if ! "$mkfs" "$tmp_out" < "$staging/manifest" > /dev/null; then
  rm -f "$tmp_out"
  exit 1
fi
mv -f "$tmp_out" "$image_out"
