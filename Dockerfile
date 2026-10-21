# syntax=docker/dockerfile:1.7

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
    --mount=type=cache,id=ant-musl-temporal-${TARGETARCH},target=/work/.cache/temporal-rust,sharing=locked <<EOF
set -eux
meson setup build                          \
  -Db_lto=true                             \
  --buildtype=release                      \
  -Dbuild_timestamp="${BUILD_TIMESTAMP:-}" \
  -Dbuild_git_hash="${BUILD_GIT_HASH:-}"   \
  --prefer-static                          \
  -Dstatic_link=true
meson compile -C build
mkdir -p /out
llvm-strip --strip-unneeded build/ant
cp build/ant /out/ant
EOF

FROM scratch AS export
COPY --from=build /out/ant /ant

FROM toolchain AS runtime-files
RUN <<EOF
set -eux
install -d /runtime/etc/ssl /runtime/usr/local/bin /runtime/usr/share
install -d -m 1777 /runtime/tmp
cp /etc/ssl/certs/ca-certificates.crt /runtime/etc/ssl/cert.pem
cp -a /usr/share/zoneinfo /runtime/usr/share/zoneinfo
rm -f                                           \
  /runtime/usr/share/zoneinfo/iso3166.tab       \
  /runtime/usr/share/zoneinfo/leap-seconds.list \
  /runtime/usr/share/zoneinfo/leapseconds       \
  /runtime/usr/share/zoneinfo/tzdata.zi         \
  /runtime/usr/share/zoneinfo/zone.tab          \
  /runtime/usr/share/zoneinfo/zone1970.tab
EOF
COPY --from=build /out/ant /runtime/usr/local/bin/ant

FROM scratch AS runtime
COPY --from=runtime-files /runtime/ /

ENV HOME=/tmp
WORKDIR /app
ENTRYPOINT ["/usr/local/bin/ant"]
