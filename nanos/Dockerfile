FROM alpine:3.23 AS build
WORKDIR /work

RUN apk add --no-cache \
    bash \
    build-base \
    ca-certificates \
    clang \
    cmake \
    curl \
    git \
    jq \
    libunwind-dev \
    libunwind-static \
    linux-headers \
    lld \
    llvm \
    meson \
    musl-dev \
    ninja \
    nodejs \
    npm \
    pkgconf \
    py3-tomli \
    tar \
    util-linux-dev \
    util-linux-static \
	    xz \
	    zstd

ARG TARGETARCH
ARG ZIG_VERSION=0.15.2
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

ARG ZLIB_VERSION=1.3.1
RUN set -eux; \
	    git clone --depth 1 --branch "v${ZLIB_VERSION}" https://github.com/madler/zlib.git /tmp/zlib; \
	    ar_path="$(command -v llvm-ar)"; \
    ranlib_path="$(command -v llvm-ranlib)"; \
    cmake -B /tmp/zlib/build \
      -G Ninja \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_AR="$ar_path" \
      -DCMAKE_RANLIB="$ranlib_path" \
      -DCMAKE_INSTALL_PREFIX=/deps-cache \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
      /tmp/zlib; \
    cmake --build /tmp/zlib/build; \
    cmake --install /tmp/zlib/build; \
    rm -rf /tmp/zlib /deps-cache/lib/libz.so*

COPY . /work

RUN git config --global --add safe.directory /work

ARG BUILD_TIMESTAMP=0
ARG BUILD_GIT_HASH=unknown
RUN --mount=type=cache,id=ant-nanos-meson-wraps,target=/work/vendor/packagecache,sharing=locked \
    --mount=type=cache,id=ant-nanos-build-${TARGETARCH},target=/work/build,sharing=locked \
    set -eux; \
	    ar_path="$(command -v llvm-ar)"; \
	    ranlib_path="$(command -v llvm-ranlib)"; \
    export CC=clang; \
    export CXX=clang++; \
    export AR="$ar_path"; \
    export CMAKE_AR="$ar_path"; \
    export RANLIB="$ranlib_path"; \
    export CMAKE_RANLIB="$ranlib_path"; \
	    export CC_LD=lld; \
	    export PKG_CONFIG_PATH="/deps-cache/lib/pkgconfig:${PKG_CONFIG_PATH:-}"; \
	    export CMAKE_PREFIX_PATH="/deps-cache:${CMAKE_PREFIX_PATH:-}"; \
	    meson_reconfigure=; \
	    if [ -f build/build.ninja ]; then meson_reconfigure=--reconfigure; fi; \
	    meson setup build $meson_reconfigure \
	      -Db_lto=true \
	      --buildtype=release \
	      -Dbuild_timestamp="${BUILD_TIMESTAMP}" \
	      -Dbuild_git_hash="${BUILD_GIT_HASH}" \
	      -Ddeps_prefix_cmake=/deps-cache \
	      --prefer-static \
      -Dstatic_link=true; \
    meson compile -C build; \
    ./build/ant --version-raw; \
    llvm-strip --strip-unneeded build/ant; \
    mkdir -p /out; \
    cp build/ant /out/ant

FROM scratch AS export
COPY --from=build /out/ant /ant
