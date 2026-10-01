# Builder image: toolchain plus vcpkg dependencies prebuilt into a binary cache.
# Builds under rootless Podman and rootless Docker. Build from the repository root:
#   podman build -f deploy/containers/builder.Containerfile -t chronolog-builder:local .
# ubuntu:24.04 index digest resolved from the Docker Hub registry API.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ARG VCPKG_BASELINE=4a1c77189c64dae7afd478333a64d1e604d5dc91

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential g++-13 cmake ninja-build pkg-config \
        git curl ca-certificates zip unzip tar python3 \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_MAX_CONCURRENCY=4 \
    VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg-cache \
    VCPKG_DISABLE_METRICS=1 \
    CXX=g++-13 \
    CC=gcc-13

RUN git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout "$VCPKG_BASELINE" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics \
    && mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"

# Prebuild the dependency set. Downstream configures restore these archives from
# VCPKG_DEFAULT_BINARY_CACHE instead of compiling gRPC again.
COPY vcpkg.json /opt/chronolog-deps/vcpkg.json
RUN "$VCPKG_ROOT/vcpkg" install \
        --x-manifest-root=/opt/chronolog-deps \
        --x-install-root=/opt/chronolog-deps/installed \
        --triplet x64-linux

WORKDIR /src
