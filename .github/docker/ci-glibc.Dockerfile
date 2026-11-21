# syntax=docker/dockerfile:1.7

FROM ubuntu:22.04

ARG LLVM_VERSION=21
ARG NODE_VERSION=22
ARG LLVM_KEY=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421

ENV DEBIAN_FRONTEND=noninteractive

COPY <<EOF /etc/apt/apt.conf.d/99retries
Acquire::Retries "3";
Acquire::http::Timeout "15";
Acquire::https::Timeout "15";
EOF

RUN <<EOF
set -eux
apt-get update
apt-get install -y --no-install-recommends         \
  ca-certificates curl git gnupg jq wget xz-utils  \
  binutils cmake make ninja-build patch perl zstd  \
  libc6-dev libstdc++-11-dev pkg-config            \
  python3-pip tzdata

curl -fsSL "https://deb.nodesource.com/setup_${NODE_VERSION}.x" | bash -
apt-get install -y --no-install-recommends nodejs

curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/llvm.asc
gpg --show-keys --with-colons /tmp/llvm.asc \
  | awk -F: '$1 == "fpr" { print $10 }'     \
  | grep -qx "$LLVM_KEY"
gpg --dearmor -o /usr/share/keyrings/llvm.gpg /tmp/llvm.asc
rm -f /tmp/llvm.asc
cat <<LIST > /etc/apt/sources.list.d/llvm.list
deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-${LLVM_VERSION} main
LIST

apt-get update
apt-get install -y --no-install-recommends \
  "clang-${LLVM_VERSION}"                  \
  "lld-${LLVM_VERSION}"                    \
  "llvm-${LLVM_VERSION}"

ln -sf "/usr/bin/clang-${LLVM_VERSION}" /usr/local/bin/cc
ln -sf "/usr/bin/clang++-${LLVM_VERSION}" /usr/local/bin/c++

pip3 install --no-cache-dir meson tomli
rm -rf /var/lib/apt/lists/*
EOF
