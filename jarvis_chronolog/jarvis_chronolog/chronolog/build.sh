#!/bin/bash
# ChronoLog + IOWarp build script
#
# Runs inside the shared jarvis pipeline build container (starts from
# ##BASE_IMAGE## / pipeline.container_base, typically ubuntu:24.04).
# Installs the superset of ChronoLog + clio-core deps mirrored from
# .devcontainer/iowarp/Dockerfile, then clones ChronoLog (with the
# external/iowarp submodule bumped to transparent-compress so
# WRP_CTE_ENABLE_HDF5_VOL is available) and builds+installs into
# /usr/local. The resulting container is committed to
# jarvis-build-<pipeline-name>; the deploy stage then copies /usr/local
# out of that image.
set -eo pipefail

export DEBIAN_FRONTEND=noninteractive
export TZ=UTC

# --------------------------------------------------------------------
# System packages — superset of ChronoLog + clio-core apt deps
# --------------------------------------------------------------------
apt-get update
apt-get install -y --no-install-recommends \
    gcc g++ gdb make ninja-build \
    cmake \
    autoconf automake libtool pkg-config \
    ccache patchelf \
    ca-certificates git curl wget \
    libboost-all-dev \
    libjson-c-dev \
    libssl-dev \
    libfuse-dev \
    libfabric-dev libfabric-bin \
    libspdlog-dev libfmt-dev \
    libcereal-dev \
    python3-dev python3-pip python3-venv \
    pybind11-dev \
    libgtest-dev \
    libmpich-dev mpich \
    libcurl4-openssl-dev \
    redis-server redis-tools \
    nlohmann-json3-dev \
    libpoco-dev \
    libaio-dev liburing-dev \
    catch2 \
    libelf-dev
rm -rf /var/lib/apt/lists/*

# --------------------------------------------------------------------
# HDF5 2.1.1 (iowarp VOL API needs ≥ 2.x; Ubuntu apt ships 1.10)
# --------------------------------------------------------------------
wget -q https://github.com/HDFGroup/hdf5/releases/download/2.1.1/hdf5-2.1.1.tar.gz -O /tmp/hdf5.tar.gz
tar xzf /tmp/hdf5.tar.gz -C /tmp
cmake -S /tmp/hdf5-2.1.1 -B /tmp/hdf5-build \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DHDF5_BUILD_CPP_LIB=ON \
    -DHDF5_BUILD_TOOLS=ON \
    -DHDF5_ENABLE_Z_LIB_SUPPORT=ON \
    -DHDF5_ENABLE_SZIP_SUPPORT=OFF \
    -DHDF5_BUILD_EXAMPLES=OFF \
    -DHDF5_BUILD_FORTRAN=OFF \
    -DBUILD_TESTING=OFF
cmake --build /tmp/hdf5-build --parallel "$(nproc)"
cmake --install /tmp/hdf5-build
rm -rf /tmp/hdf5-2.1.1 /tmp/hdf5-build /tmp/hdf5.tar.gz

# --------------------------------------------------------------------
# Argobots 1.2
# --------------------------------------------------------------------
git clone --depth 1 --branch v1.2 \
    https://github.com/pmodels/argobots.git /tmp/argobots
(
    cd /tmp/argobots
    ./autogen.sh
    ./configure --prefix=/usr/local
    make -j"$(nproc)"
    make install
)
rm -rf /tmp/argobots

# --------------------------------------------------------------------
# Mercury 2.2.0
# --------------------------------------------------------------------
git clone --depth 1 --branch v2.2.0 \
    https://github.com/mercury-hpc/mercury.git /tmp/mercury
cmake -S /tmp/mercury -B /tmp/mercury-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DNA_USE_OFI=ON \
    -DNA_USE_SM=ON \
    -DMERCURY_USE_BOOST_PP=ON
cmake --build /tmp/mercury-build --parallel "$(nproc)"
cmake --install /tmp/mercury-build
rm -rf /tmp/mercury /tmp/mercury-build

# --------------------------------------------------------------------
# Mochi-Margo 0.13.1
# --------------------------------------------------------------------
git clone --depth 1 --branch v0.13.1 \
    https://github.com/mochi-hpc/mochi-margo.git /tmp/margo
(
    cd /tmp/margo
    autoreconf -i
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig ./configure --prefix=/usr/local
    make -j"$(nproc)"
    make install
)
rm -rf /tmp/margo

# --------------------------------------------------------------------
# Mochi-Thallium 0.10.1
# --------------------------------------------------------------------
git clone --depth 1 --branch v0.10.1 \
    https://github.com/mochi-hpc/mochi-thallium.git /tmp/thallium
cmake -S /tmp/thallium -B /tmp/thallium-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_PREFIX_PATH=/usr/local
cmake --build /tmp/thallium-build --parallel "$(nproc)"
cmake --install /tmp/thallium-build
rm -rf /tmp/thallium /tmp/thallium-build

# --------------------------------------------------------------------
# yaml-cpp 0.8.0
# --------------------------------------------------------------------
curl -sL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz | tar xz -C /tmp
cmake -S /tmp/yaml-cpp-0.8.0 -B /tmp/yaml-cpp-build \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=ON \
    -DYAML_CPP_BUILD_TESTS=OFF \
    -DYAML_CPP_BUILD_TOOLS=OFF
cmake --build /tmp/yaml-cpp-build --parallel "$(nproc)"
cmake --install /tmp/yaml-cpp-build
rm -rf /tmp/yaml-cpp-0.8.0 /tmp/yaml-cpp-build

# --------------------------------------------------------------------
# msgpack-c 6.1.0
# --------------------------------------------------------------------
git clone --depth 1 --branch c-6.1.0 \
    https://github.com/msgpack/msgpack-c.git /tmp/msgpack-c
cmake -S /tmp/msgpack-c -B /tmp/msgpack-build \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DMSGPACK_BUILD_TESTS=OFF \
    -DMSGPACK_BUILD_EXAMPLES=OFF
cmake --build /tmp/msgpack-build --parallel "$(nproc)"
cmake --install /tmp/msgpack-build
rm -rf /tmp/msgpack-c /tmp/msgpack-build

# --------------------------------------------------------------------
# libsodium 1.0.20 (ZeroMQ prerequisite)
# --------------------------------------------------------------------
curl -sL https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20.tar.gz \
    | tar xz -C /tmp
(
    cd /tmp/libsodium-1.0.20
    ./configure --prefix=/usr/local --with-pic
    make -j"$(nproc)"
    make install
)
rm -rf /tmp/libsodium-1.0.20

# --------------------------------------------------------------------
# ZeroMQ 4.3.5
# --------------------------------------------------------------------
curl -sL https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz \
    | tar xz -C /tmp
cmake -S /tmp/zeromq-4.3.5 -B /tmp/zmq-build \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED=ON \
    -DBUILD_STATIC=ON \
    -DBUILD_TESTS=OFF \
    -DWITH_LIBSODIUM=ON \
    -DWITH_DOCS=OFF \
    -DCMAKE_PREFIX_PATH=/usr/local
cmake --build /tmp/zmq-build --parallel "$(nproc)"
cmake --install /tmp/zmq-build
rm -rf /tmp/zeromq-4.3.5 /tmp/zmq-build

# --------------------------------------------------------------------
# cppzmq 4.10.0 (header-only)
# --------------------------------------------------------------------
curl -sL https://github.com/zeromq/cppzmq/archive/refs/tags/v4.10.0.tar.gz \
    | tar xz -C /tmp
cmake -S /tmp/cppzmq-4.10.0 -B /tmp/cppzmq-build \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_PREFIX_PATH=/usr/local \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCPPZMQ_BUILD_TESTS=OFF
cmake --install /tmp/cppzmq-build
rm -rf /tmp/cppzmq-4.10.0 /tmp/cppzmq-build

ldconfig

# --------------------------------------------------------------------
# jarvis-cd (Python) — needed by the deploy image to run `chimaera
# compose` and by post-install tooling.
# --------------------------------------------------------------------
git clone --depth 1 https://github.com/grc-iit/jarvis-cd.git /opt/jarvis-cd
(
    cd /opt/jarvis-cd
    git config submodule.awesome-scienctific-applications.update none
    pip3 install --break-system-packages .
)

# --------------------------------------------------------------------
# ChronoLog + IOWarp (clio-core is a submodule at external/iowarp).
# Override ChronoLog's pinned clio-core submodule so we pick up
# transparent-compress's WRP_CTE_ENABLE_HDF5_VOL option (needed to skip
# the VOL adapter cleanly) and the non-recursive submodule init.
# --------------------------------------------------------------------
: "${CHRONOLOG_REPO:=https://github.com/grc-iit/ChronoLog.git}"
: "${CHRONOLOG_REF:=iowarp}"
: "${IOWARP_REPO:=https://github.com/iowarp/clio-core.git}"
: "${IOWARP_REF:=transparent-compress}"

# Rewrite all git@github.com:<user>/<repo> URLs to https so nested
# submodules that default to SSH clone without host-key setup still
# work in a fresh container. The jarvis-cd submodule tree references
# awesome-scienctific-applications via SSH.
git config --global url."https://github.com/".insteadOf "git@github.com:"
git clone "${CHRONOLOG_REPO}" /opt/chronolog
(
    cd /opt/chronolog
    git checkout "${CHRONOLOG_REF}"
    git submodule update --init --recursive
    cd external/iowarp
    git fetch --depth 1 "${IOWARP_REPO}" "${IOWARP_REF}"
    git checkout -q FETCH_HEAD
)

# -DWRP_CTE_ENABLE_HDF5_VOL=OFF: skip clio-core's HDF5 VOL adapter. It
# currently requires HDF5 >= 2.0 on the build host and is not needed by
# ChronoLog. The gate was introduced in clio-core transparent-compress;
# we bumped the iowarp submodule above to pick it up.
cmake -S /opt/chronolog -B /opt/chronolog/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_PREFIX_PATH=/usr/local \
    -DCHRONOLOG_BUILD_TESTING=OFF \
    -DWRP_CORE_ENABLE_TESTS=OFF \
    -DWRP_CORE_ENABLE_BENCHMARKS=OFF \
    -DWRP_CORE_ENABLE_PYTHON=OFF \
    -DWRP_CTE_ENABLE_HDF5_VOL=OFF
cmake --build /opt/chronolog/build --parallel "$(nproc)"
cmake --install /opt/chronolog/build
ldconfig

# --------------------------------------------------------------------
# Install jarvis packages shipped in the ChronoLog tree so the deploy
# image can run the chronolog_container pipeline directly.
#
# jarvis_chronolog is a regular pip package. jarvis_iowarp is a
# namespace package with no pyproject.toml in clio-core (pkgs are
# registered via `jarvis repo add` in normal use), so we copy its
# package tree directly into the Python site-packages directory — the
# deploy image only carries /usr/local, so a .pth pointer back into
# /opt/chronolog wouldn't survive.
# --------------------------------------------------------------------
pip3 install --break-system-packages /opt/chronolog/jarvis_chronolog
SP=$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')
if [ -d /opt/chronolog/external/iowarp/jarvis_iowarp/jarvis_iowarp ]; then
    cp -r /opt/chronolog/external/iowarp/jarvis_iowarp/jarvis_iowarp "${SP}/jarvis_iowarp"
fi

# --------------------------------------------------------------------
# Seed a default chimaera config so `chimaera runtime start` works out
# of the box inside the deploy container.
# --------------------------------------------------------------------
mkdir -p /root/.chimaera
if [ -f /opt/chronolog/external/iowarp/context-runtime/config/chimaera_default.yaml ]; then
    cp /opt/chronolog/external/iowarp/context-runtime/config/chimaera_default.yaml \
       /root/.chimaera/chimaera.yaml
fi
