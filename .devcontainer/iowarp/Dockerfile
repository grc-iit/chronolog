ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-c"]

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

# ── System packages ───────────────────────────────────────────────────────────
# Note: libhdf5-dev is intentionally omitted — HDF5 2.1.1 is built from source
# below (Ubuntu 24.04 apt only ships 1.10, which is too old for iowarp's VOL API).
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build toolchain
    gcc g++ gdb make ninja-build \
    cmake \
    autoconf automake libtool pkg-config \
    # Source / VCS
    git curl wget \
    # ChronoLog apt deps
    libboost-all-dev \
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
    # iowarp apt deps
    redis-server redis-tools \
    nlohmann-json3-dev \
    libpoco-dev \
    libaio-dev \
    # Dev convenience
    sudo vim less procps openssh-server \
 && rm -rf /var/lib/apt/lists/*

# ── HDF5 2.1.1 (from source — needed by both ChronoLog and iowarp VOL API) ───
RUN wget -q https://github.com/HDFGroup/hdf5/releases/download/2.1.1/hdf5-2.1.1.tar.gz -O /tmp/hdf5.tar.gz \
 && tar xzf /tmp/hdf5.tar.gz -C /tmp \
 && cmake -S /tmp/hdf5-2.1.1 -B /tmp/hdf5-build \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_SHARED_LIBS=ON \
          -DHDF5_BUILD_CPP_LIB=ON \
          -DHDF5_BUILD_TOOLS=ON \
          -DHDF5_ENABLE_Z_LIB_SUPPORT=ON \
          -DHDF5_ENABLE_SZIP_SUPPORT=OFF \
          -DHDF5_BUILD_EXAMPLES=OFF \
          -DHDF5_BUILD_FORTRAN=OFF \
          -DBUILD_TESTING=OFF \
 && cmake --build /tmp/hdf5-build --parallel "$(nproc)" \
 && cmake --install /tmp/hdf5-build \
 && rm -rf /tmp/hdf5-2.1.1 /tmp/hdf5-build /tmp/hdf5.tar.gz

# ── Argobots 1.2 ─────────────────────────────────────────────────────────────
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

# ── yaml-cpp 0.8.0 ────────────────────────────────────────────────────────────
RUN curl -sL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz | tar xz -C /tmp \
 && cmake -S /tmp/yaml-cpp-0.8.0 -B /tmp/yaml-cpp-build \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DBUILD_SHARED_LIBS=ON \
          -DYAML_CPP_BUILD_TESTS=OFF \
          -DYAML_CPP_BUILD_TOOLS=OFF \
 && cmake --build /tmp/yaml-cpp-build --parallel "$(nproc)" \
 && cmake --install /tmp/yaml-cpp-build \
 && rm -rf /tmp/yaml-cpp-0.8.0 /tmp/yaml-cpp-build

# ── msgpack-c ─────────────────────────────────────────────────────────────────
RUN git clone --depth 1 --branch c-6.1.0 \
        https://github.com/msgpack/msgpack-c.git /tmp/msgpack-c \
 && cmake -S /tmp/msgpack-c -B /tmp/msgpack-build \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DMSGPACK_BUILD_TESTS=OFF \
          -DMSGPACK_BUILD_EXAMPLES=OFF \
 && cmake --build /tmp/msgpack-build --parallel "$(nproc)" \
 && cmake --install /tmp/msgpack-build \
 && rm -rf /tmp/msgpack-c /tmp/msgpack-build

# ── libsodium 1.0.20 (required by ZeroMQ) ────────────────────────────────────
RUN curl -sL https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20.tar.gz \
        | tar xz -C /tmp \
 && cd /tmp/libsodium-1.0.20 \
 && ./configure --prefix=/usr/local --with-pic \
 && make -j"$(nproc)" \
 && make install \
 && rm -rf /tmp/libsodium-1.0.20

# ── ZeroMQ 4.3.5 ─────────────────────────────────────────────────────────────
RUN curl -sL https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz \
        | tar xz -C /tmp \
 && cmake -S /tmp/zeromq-4.3.5 -B /tmp/zmq-build \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DBUILD_SHARED=ON \
          -DBUILD_STATIC=ON \
          -DBUILD_TESTS=OFF \
          -DWITH_LIBSODIUM=ON \
          -DWITH_DOCS=OFF \
          -DCMAKE_PREFIX_PATH=/usr/local \
 && cmake --build /tmp/zmq-build --parallel "$(nproc)" \
 && cmake --install /tmp/zmq-build \
 && rm -rf /tmp/zeromq-4.3.5 /tmp/zmq-build

# ── cppzmq 4.10.0 (header-only C++ bindings for ZeroMQ) ──────────────────────
RUN curl -sL https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz \
        | tar xz -C /tmp \
 && cmake -S /tmp/cppzmq-4.10.0 -B /tmp/cppzmq-build \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_PREFIX_PATH=/usr/local \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DCPPZMQ_BUILD_TESTS=OFF \
 && cmake --install /tmp/cppzmq-build \
 && rm -rf /tmp/cppzmq-4.10.0 /tmp/cppzmq-build

# Refresh the dynamic linker cache
RUN ldconfig

# ── Dev user ──────────────────────────────────────────────────────────────────
ARG HOST_UID=1000
ARG HOST_GID=1000

RUN if id ubuntu >/dev/null 2>&1; then \
      usermod -u 59999 ubuntu && groupmod -g 59999 ubuntu; \
    fi \
 && groupadd -g "${HOST_GID}" grc-iit 2>/dev/null || true \
 && useradd -m -u "${HOST_UID}" -g "${HOST_GID}" -s /bin/bash grc-iit \
 && echo "grc-iit ALL=(ALL:ALL) NOPASSWD: ALL" > /etc/sudoers.d/grc-iit

USER grc-iit

# Install Claude Code
RUN curl -fsSL https://claude.ai/install.sh | bash
WORKDIR /workspace
CMD ["/bin/bash"]
