# syntax=docker/dockerfile:1.7

FROM alpine:3.23

RUN apk add --no-cache                                  \
  bash ca-certificates curl git jq tar xz zstd          \
  clang cmake lld llvm meson ninja pkgconf              \
  libunwind-dev libunwind-static linux-headers musl-dev \
  nodejs npm py3-tomli                                  \
  tzdata util-linux-dev util-linux-static
