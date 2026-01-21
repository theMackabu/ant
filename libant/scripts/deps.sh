#!/bin/bash
set -e

. "$(dirname "$0")/common.sh"

build_deps() {
  if [ -f "$DEPS_DIR/lib/libllhttp.a" ] && \
     [ -f "$DEPS_DIR/lib/libz.a" ] && \
     [ -f "$DEPS_DIR/lib/libmbedtls.a" ] && \
     [ -f "$DEPS_DIR/lib/libsodium.a" ]; then
    echo "Dependencies already built, skipping..."
    return
  fi

  echo "Building dependencies..."
  mkdir -p "$DEPS_DIR" "$CACHE_DIR"

  if [ ! -f "$DEPS_DIR/lib/libllhttp.a" ]; then
    echo "Building llhttp..."
    rm -rf "$CACHE_DIR/llhttp"
    git clone --depth 1 --branch release/v9.3.0 https://github.com/nodejs/llhttp.git "$CACHE_DIR/llhttp"
    cd "$CACHE_DIR/llhttp"
    cmake -B build \
      -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_STATIC_LIBS=ON \
      -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j$NCPU
    cmake --install build
  fi

  if [ ! -f "$DEPS_DIR/lib/libz.a" ]; then
    echo "Building zlib..."
    rm -rf "$CACHE_DIR/zlib"
    git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git "$CACHE_DIR/zlib"
    cd "$CACHE_DIR/zlib"
    cmake -B build \
      -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j$NCPU
    cmake --install build
    rm -f "$DEPS_DIR/lib/libz.so"* "$DEPS_DIR/lib/libz.dylib"* 2>/dev/null || true
  fi

  if [ ! -f "$DEPS_DIR/lib/libmbedtls.a" ]; then
    echo "Building mbedTLS..."
    rm -rf "$CACHE_DIR/mbedtls"
    git clone --depth 1 --branch mbedtls-3.6.5 --recurse-submodules https://github.com/Mbed-TLS/mbedtls.git "$CACHE_DIR/mbedtls"
    cd "$CACHE_DIR/mbedtls"
    cmake -B build \
      -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
      -DENABLE_PROGRAMS=OFF \
      -DENABLE_TESTING=OFF \
      -DCMAKE_BUILD_TYPE=Release \
      -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
      -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
    cmake --build build -j$NCPU
    cmake --install build
  fi

  if [ ! -f "$DEPS_DIR/lib/libsodium.a" ]; then
    echo "Building libsodium..."
    rm -rf "$CACHE_DIR/libsodium"
    git clone --depth 1 --branch 1.0.20-RELEASE https://github.com/jedisct1/libsodium.git "$CACHE_DIR/libsodium"
    cd "$CACHE_DIR/libsodium"
    ./autogen.sh
    ./configure --prefix="$DEPS_DIR" --disable-shared --enable-static
    make -j$NCPU
    make install
  fi

  echo "Dependencies built in $DEPS_DIR"
}

build_deps
