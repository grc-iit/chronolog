#!/bin/bash
# ChronoLog build script
#
# Runs inside the shared jarvis pipeline build container (started from
# ##BASE_IMAGE## / pipeline.container_base). The pipeline is expected to
# have already composed clio-core into /usr/local — either via a prior
# `jarvis_iowarp.wrp_runtime` pkg in the same pipeline (its build.sh
# installs all iowarp build deps + the clio-core libs), or by injecting
# a cached `jarvis-deploy-*` Library deploy image (builtin.hdf5,
# builtin.compress_libs, …) before this package runs.
#
# Because iowarp is pre-installed, we pass
# -DCHRONOLOG_BUILD_IOWARP=OFF so CMakeLists.txt skips
# `add_subdirectory(external/iowarp)` and instead
# `find_package(iowarp-core)` at /usr/local/lib/cmake/iowarp-core.
# That also removes the need to install iowarp-only deps here (HDF5
# 2.1.1, yaml-cpp, msgpack, libsodium, ZeroMQ, cppzmq, ADIOS2,
# FPZIP/SZ3/libpressio, compression apt libs) — wrp_runtime's build.sh
# already put them under /usr/local.
set -eo pipefail

export DEBIAN_FRONTEND=noninteractive
export TZ=UTC

# --------------------------------------------------------------------
# ChronoLog-specific apt deps. HDF5/ADIOS2/compression devel packages
# are intentionally NOT here: they belong to iowarp and are installed
# by wrp_runtime's build.sh (or a builtin.hdf5 / builtin.compress_libs
# Library pkg) earlier in the same build container. spdlog, json-c,
# fuse, fabric, poco, aio, uring, catch2, elf, redis are chronolog's
# own direct deps (chrono_common + chimod/*).
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
# Argobots 1.2, Mercury 2.2.0, Margo 0.13.1, Thallium 0.10.1 — used by
# chrono_common's transport layer (RDMATransferAgent, StoryChunk*). Not
# part of clio-core's build, so we keep them here.
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

git clone --depth 1 --branch v0.10.1 \
    https://github.com/mochi-hpc/mochi-thallium.git /tmp/thallium
cmake -S /tmp/thallium -B /tmp/thallium-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_PREFIX_PATH=/usr/local
cmake --build /tmp/thallium-build --parallel "$(nproc)"
cmake --install /tmp/thallium-build
rm -rf /tmp/thallium /tmp/thallium-build

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
# ChronoLog: cloned shallow (no --recurse-submodules) since
# CHRONOLOG_BUILD_IOWARP=OFF skips add_subdirectory(external/iowarp).
# Rewrite ssh submodule URLs just in case someone sets
# CHRONOLOG_BUILD_IOWARP=ON out of band in this same container.
# --------------------------------------------------------------------
: "${CHRONOLOG_REPO:=https://github.com/grc-iit/ChronoLog.git}"
: "${CHRONOLOG_REF:=iowarp}"

git config --global url."https://github.com/".insteadOf "git@github.com:"
git clone "${CHRONOLOG_REPO}" /opt/chronolog
(
    cd /opt/chronolog
    git checkout "${CHRONOLOG_REF}"
)

# -DCHRONOLOG_BUILD_IOWARP=OFF: skip external/iowarp, rely on the
#   iowarp-core cmake package pre-installed under /usr/local by
#   wrp_runtime (or a Library pkg) earlier in this build container.
cmake -S /opt/chronolog -B /opt/chronolog/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_PREFIX_PATH=/usr/local \
    -DCHRONOLOG_BUILD_IOWARP=OFF \
    -DCHRONOLOG_BUILD_TESTING=OFF
cmake --build /opt/chronolog/build --parallel "$(nproc)"
cmake --install /opt/chronolog/build
ldconfig

# --------------------------------------------------------------------
# Install jarvis_chronolog (pip) and jarvis_iowarp (namespace package,
# no pyproject.toml — copied directly into site-packages via the
# pre-installed clio-core tree).
# --------------------------------------------------------------------
pip3 install --break-system-packages /opt/chronolog/jarvis_chronolog
SP=$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')
# wrp_runtime's build already clones clio-core to /opt/iowarp; use the
# jarvis_iowarp namespace package from there.
if [ -d /opt/iowarp/jarvis_iowarp/jarvis_iowarp ] && [ ! -d "${SP}/jarvis_iowarp" ]; then
    cp -r /opt/iowarp/jarvis_iowarp/jarvis_iowarp "${SP}/jarvis_iowarp"
fi

# --------------------------------------------------------------------
# Seed a default chimaera config so `chimaera runtime start` works out
# of the box inside the deploy container (wrp_runtime's build.sh
# already does this when it runs first, but do it idempotently here
# too in case chronolog is used without wrp_runtime).
# --------------------------------------------------------------------
mkdir -p /root/.chimaera
for src in \
    /opt/iowarp/context-runtime/config/chimaera_default.yaml \
    /usr/local/share/iowarp-core/chimaera_default.yaml; do
    if [ -f "$src" ] && [ ! -f /root/.chimaera/chimaera.yaml ]; then
        cp "$src" /root/.chimaera/chimaera.yaml
    fi
done
