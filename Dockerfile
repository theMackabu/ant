# syntax=docker/dockerfile:1.7

FROM alpine:3.23 AS toolchain
WORKDIR /work

RUN apk add --no-cache \
    bash build-base ca-certificates curl git tar xz zstd \
    clang cmake lld llvm meson ninja pkgconf \
    libunwind-dev libunwind-static linux-headers musl-dev \
    nodejs npm py3-tomli \
    tzdata util-linux-dev util-linux-static

ARG TARGETARCH
ARG ZIG_VERSION=0.16.0
RUN set -eux; \
    case "$TARGETARCH" in \
      amd64) zig_arch="x86_64-linux" ;; \
      arm64) zig_arch="aarch64-linux" ;; \
      *) echo "unsupported Docker target arch: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/zig-${zig_arch}-${ZIG_VERSION}.tar.xz" -o /tmp/zig.tar.xz; \
    mkdir -p /opt; \
    tar -xJf /tmp/zig.tar.xz -C /opt; \
    mv "/opt/zig-${zig_arch}-${ZIG_VERSION}" /opt/zig; \
    ln -s /opt/zig/zig /usr/local/bin/zig; \
    rm /tmp/zig.tar.xz

FROM toolchain AS deps

COPY meson.build meson_options.txt ./
RUN mkdir -p vendor
COPY vendor/*.wrap vendor/
COPY vendor/packagefiles/ vendor/packagefiles/

RUN meson subprojects download || test -d vendor/boringssl/.git

FROM deps AS build

COPY src/tools/package.json src/tools/npm-shrinkwrap.json /tmp/ant-tools/
RUN --mount=type=cache,id=ant-musl-npm,target=/root/.npm,sharing=locked \
    npm ci --prefix /tmp/ant-tools && \
    mkdir -p src/tools && \
    ln -s /tmp/ant-tools/node_modules src/tools/node_modules

COPY . /work

ARG TARGETARCH
ARG BUILD_TIMESTAMP
ARG BUILD_GIT_HASH
ENV CC=clang CXX=clang++ CC_LD=lld \
    AR=llvm-ar RANLIB=llvm-ranlib CMAKE_AR=llvm-ar CMAKE_RANLIB=llvm-ranlib
RUN --mount=type=cache,id=ant-musl-build-${TARGETARCH},target=/work/build,sharing=locked \
    --mount=type=cache,id=ant-musl-temporal-${TARGETARCH},target=/work/.cache/temporal-rust,sharing=locked \
    set -eux; \
    build_config="${BUILD_TIMESTAMP:-}|${BUILD_GIT_HASH:-}"; \
    cached_config="$(cat build/.ant-container-config 2>/dev/null || true)"; \
    if [ ! -f build/build.ninja ] || [ "$build_config" != "$cached_config" ]; then \
      meson_reconfigure=; \
      if [ -f build/build.ninja ]; then meson_reconfigure=--reconfigure; fi; \
      meson setup build $meson_reconfigure \
        -Db_lto=true \
        --buildtype=release \
        -Dbuild_timestamp="${BUILD_TIMESTAMP:-}" \
        -Dbuild_git_hash="${BUILD_GIT_HASH:-}" \
        --prefer-static \
        -Dstatic_link=true; \
      printf '%s' "$build_config" > build/.ant-container-config; \
    fi; \
    meson compile -C build; \
    mkdir -p /out; \
    llvm-strip --strip-unneeded build/ant; \
    cp build/ant /out/ant

FROM scratch AS export
COPY --from=build /out/ant /ant

FROM toolchain AS runtime-files
RUN set -eux; \
    install -d /runtime/etc/ssl /runtime/usr/local/bin /runtime/usr/share; \
    install -d -m 1777 /runtime/tmp; \
    cp /etc/ssl/certs/ca-certificates.crt /runtime/etc/ssl/cert.pem; \
    cp -a /usr/share/zoneinfo /runtime/usr/share/zoneinfo; \
    rm -f \
      /runtime/usr/share/zoneinfo/iso3166.tab \
      /runtime/usr/share/zoneinfo/leap-seconds.list \
      /runtime/usr/share/zoneinfo/leapseconds \
      /runtime/usr/share/zoneinfo/tzdata.zi \
      /runtime/usr/share/zoneinfo/zone.tab \
      /runtime/usr/share/zoneinfo/zone1970.tab
COPY --from=build /out/ant /runtime/usr/local/bin/ant

FROM scratch AS runtime
COPY --from=runtime-files /runtime/ /

ENV HOME=/tmp
WORKDIR /app
ENTRYPOINT ["/usr/local/bin/ant"]
