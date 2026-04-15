ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-c"]

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

# ── System packages ───────────────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build toolchain
    gcc g++ gdb make ninja-build \
    cmake \
    autoconf automake libtool pkg-config \
    # Source / VCS
    git curl wget \
    # ChronoLog direct apt-installable deps
    libboost-all-dev \
    libhdf5-dev \
    libjson-c-dev \
    libssl-dev \
    libfuse-dev \
    libfabric-dev \
    libspdlog-dev \
    libcereal-dev \
    python3-dev python3-pip \
    pybind11-dev \
    libgtest-dev \
    libmpich-dev mpich \
    libcurl4-openssl-dev \
    # Dev convenience
    sudo vim less procps openssh-server \
 && rm -rf /var/lib/apt/lists/*

# ── Argobots 1.2 (no Ubuntu package) ─────────────────────────────────────────
RUN git clone --depth 1 --branch v1.2 \
        https://github.com/pmodels/argobots.git /tmp/argobots \
 && cd /tmp/argobots \
 && ./autogen.sh \
 && ./configure --prefix=/usr/local \
 && make -j"$(nproc)" \
 && make install \
 && rm -rf /tmp/argobots

# ── Mercury 2.2.0 ─────────────────────────────────────────────────────────────
RUN git clone --depth 1 --branch v2.2.0 \
        https://github.com/mercury-hpc/mercury.git /tmp/mercury \
 && cmake -S /tmp/mercury -B /tmp/mercury-build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DBUILD_SHARED_LIBS=ON \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DNA_USE_OFI=ON \
          -DNA_USE_SM=ON \
          -DMERCURY_USE_BOOST_PP=ON \
 && cmake --build /tmp/mercury-build --parallel "$(nproc)" \
 && cmake --install /tmp/mercury-build \
 && rm -rf /tmp/mercury /tmp/mercury-build

# ── Mochi-Margo 0.13.1 ────────────────────────────────────────────────────────
RUN git clone --depth 1 --branch v0.13.1 \
        https://github.com/mochi-hpc/mochi-margo.git /tmp/margo \
 && cd /tmp/margo \
 && autoreconf -i \
 && ./configure --prefix=/usr/local \
 && make -j"$(nproc)" \
 && make install \
 && rm -rf /tmp/margo

# ── Mochi-Thallium 0.10.1 ────────────────────────────────────────────────────
RUN git clone --depth 1 --branch v0.10.1 \
        https://github.com/mochi-hpc/mochi-thallium.git /tmp/thallium \
 && cmake -S /tmp/thallium -B /tmp/thallium-build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
 && cmake --build /tmp/thallium-build --parallel "$(nproc)" \
 && cmake --install /tmp/thallium-build \
 && rm -rf /tmp/thallium /tmp/thallium-build

# Refresh the dynamic linker cache
RUN ldconfig

# ── Dev user ──────────────────────────────────────────────────────────────────
# Remap grc-iit user UID/GID to match the host user to avoid bind-mount
# permission issues.  Override at build time with --build-arg HOST_UID=...
ARG HOST_UID=1000
ARG HOST_GID=1000

# Ubuntu 24.04 ships a default "ubuntu" user at UID 1000 which blocks UID
# remapping.  Move it out of the way first.
RUN if id ubuntu >/dev/null 2>&1; then \
      usermod -u 59999 ubuntu && groupmod -g 59999 ubuntu; \
    fi \
 && groupadd -g "${HOST_GID}" grc-iit 2>/dev/null || true \
 && useradd -m -u "${HOST_UID}" -g "${HOST_GID}" -s /bin/bash grc-iit \
 && echo "grc-iit ALL=(ALL:ALL) NOPASSWD: ALL" > /etc/sudoers.d/grc-iit

# Install Claude Code
RUN curl -fsSL https://claude.ai/install.sh | bash

USER grc-iit
WORKDIR /workspace
CMD ["/bin/bash"]
