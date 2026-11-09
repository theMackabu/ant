# syntax=docker/dockerfile:1.7

# musl: static build for the default image and the sandbox (--target export).
FROM alpine:3.23 AS toolchain
WORKDIR /work

RUN apk add --no-cache                                  \
  bash build-base ca-certificates curl git tar xz zstd  \
  clang cmake lld llvm meson ninja pkgconf              \
  libunwind-dev libunwind-static linux-headers musl-dev \
  nodejs npm py3-tomli                                  \
  tzdata util-linux-dev util-linux-static

FROM toolchain AS deps
COPY meson.build meson_options.txt ./

RUN mkdir -p vendor
COPY vendor/*.wrap vendor/
COPY vendor/packagefiles/ vendor/packagefiles/

RUN meson subprojects download || test -d vendor/boringssl/.git
FROM deps AS build

COPY src/tools/package.json src/tools/npm-shrinkwrap.json /tmp/ant-tools/
RUN --mount=type=cache,id=ant-musl-npm,target=/root/.npm,sharing=locked <<EOF
set -e
npm ci --prefix /tmp/ant-tools
mkdir -p src/tools
ln -s /tmp/ant-tools/node_modules src/tools/node_modules
EOF

COPY . /work

ARG TARGETARCH
ARG BUILD_TIMESTAMP
ARG BUILD_GIT_HASH

ENV CC=clang CXX=clang++ CC_LD=lld AR=llvm-ar RANLIB=llvm-ranlib CMAKE_AR=llvm-ar CMAKE_RANLIB=llvm-ranlib
RUN --mount=type=cache,id=ant-musl-build-${TARGETARCH},target=/work/build,sharing=locked \
    --mount=type=cache,id=ant-musl-rust-${TARGETARCH},target=/work/.cache/rust,sharing=locked <<EOF
set -eux
# a configured build dir keeps its first toolchain; start over when it changes
stamp="$CC $CXX $CC_LD $(clang --version | head -n1)"
[ "$(cat build/.toolchain 2>/dev/null)" = "$stamp" ] || find build -mindepth 1 -delete
meson setup build                          \
  -Db_lto=true                             \
  --buildtype=release                      \
  -Dbuild_timestamp="${BUILD_TIMESTAMP:-}" \
  -Dbuild_git_hash="${BUILD_GIT_HASH:-}"   \
  --prefer-static                          \
  -Dstatic_link=true
echo "$stamp" > build/.toolchain
meson compile -C build
mkdir -p /out
llvm-strip --strip-unneeded build/ant
cp build/ant /out/ant
EOF

FROM scratch AS export
COPY --from=build /out/ant /ant
COPY --from=toolchain /etc/ssl/certs/ca-certificates.crt /ca-certificates.crt

# glibc: the -glibc image, for native addons (static musl can't dlopen).
# Debian 13 has the same glibc as distroless debian13.
FROM node:22-trixie-slim AS glibc-toolchain
WORKDIR /work

ARG LLVM_VERSION=21
RUN <<EOF
set -eux
apt-get update
apt-get install -y --no-install-recommends cmake curl git gpg libstdc++-14-dev ninja-build pkg-config python3-venv
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/llvm.asc
gpg --show-keys --with-colons /tmp/llvm.asc | awk -F: '$1 == "fpr" { print $10 }' \
  | grep -qx 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
gpg --dearmor -o /usr/share/keyrings/llvm.gpg /tmp/llvm.asc
echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/trixie/ llvm-toolchain-trixie-${LLVM_VERSION} main" \
  > /etc/apt/sources.list.d/llvm.list
apt-get update
apt-get install -y --no-install-recommends clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION}
rm -rf /var/lib/apt/lists/* /tmp/llvm.asc
# Rust build scripts link with plain cc/c++
ln -s clang /usr/lib/llvm-${LLVM_VERSION}/bin/cc
ln -s clang++ /usr/lib/llvm-${LLVM_VERSION}/bin/c++
python3 -m venv /opt/meson
echo 'meson==1.12.1 --hash=sha256:930bc7542cbd9f57009e182fd014eba48cf1a0180a7b9006d2d32d8f168d8b02' > /tmp/meson.txt
/opt/meson/bin/pip install --no-cache-dir --require-hashes -r /tmp/meson.txt
rm /tmp/meson.txt
EOF
ENV PATH=/usr/lib/llvm-${LLVM_VERSION}/bin:/opt/meson/bin:${PATH}

FROM glibc-toolchain AS build-glibc
COPY meson.build meson_options.txt ./
COPY vendor/*.wrap vendor/
COPY vendor/packagefiles/ vendor/packagefiles/
RUN meson subprojects download || test -d vendor/boringssl/.git

COPY src/tools/package.json src/tools/npm-shrinkwrap.json /tmp/ant-tools/
RUN --mount=type=cache,id=ant-glibc-npm,target=/root/.npm,sharing=locked <<EOF
set -e
npm ci --prefix /tmp/ant-tools
mkdir -p src/tools
ln -s /tmp/ant-tools/node_modules src/tools/node_modules
EOF

COPY . /work

ARG TARGETARCH
ARG BUILD_TIMESTAMP
ARG BUILD_GIT_HASH

ENV CC=clang CXX=clang++ CC_LD=lld CXX_LD=lld AR=llvm-ar RANLIB=llvm-ranlib CMAKE_AR=llvm-ar CMAKE_RANLIB=llvm-ranlib
RUN --mount=type=cache,id=ant-glibc-build-${TARGETARCH},target=/work/build,sharing=locked \
    --mount=type=cache,id=ant-glibc-rust-${TARGETARCH},target=/work/.cache/rust,sharing=locked <<EOF
set -eux
stamp="$CC $CXX $CC_LD $CXX_LD $(clang --version | head -n1)"
[ "$(cat build/.toolchain 2>/dev/null)" = "$stamp" ] || find build -mindepth 1 -delete
meson setup build                          \
  -Db_lto=true                             \
  --buildtype=release                      \
  -Dbuild_timestamp="${BUILD_TIMESTAMP:-}" \
  -Dbuild_git_hash="${BUILD_GIT_HASH:-}"
echo "$stamp" > build/.toolchain
meson compile -C build
mkdir -p /out
llvm-strip --strip-unneeded build/ant
cp build/ant /out/ant
EOF

# Both images run as distroless nonroot (uid 65532). HOME=/tmp stays writable
# when run with --user as a uid that has no passwd entry.
FROM gcr.io/distroless/cc-debian13:nonroot AS runtime-glibc
COPY --from=build-glibc /out/ant /usr/local/bin/ant
ENV HOME=/tmp
WORKDIR /app
ENTRYPOINT ["/usr/local/bin/ant"]

FROM gcr.io/distroless/static-debian13:nonroot AS runtime
COPY --from=build /out/ant /usr/local/bin/ant
ENV HOME=/tmp
WORKDIR /app
ENTRYPOINT ["/usr/local/bin/ant"]
