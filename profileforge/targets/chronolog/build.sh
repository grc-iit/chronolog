#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "${REPO_ROOT}"

BUILD_TYPE="${CHRONOLOG_BUILD_TYPE:-Release}"
BUILD_VARIANT="${CHRONOLOG_BUILD_VARIANT:-tau}"
BUILD_DIR="${CHRONOLOG_BUILD_DIR:-${REPO_ROOT}/.agent/build-${BUILD_VARIANT}/${BUILD_TYPE}}"
INSTALL_PREFIX="${CHRONOLOG_INSTALL_PREFIX:-${REPO_ROOT}/.agent/install-${BUILD_VARIANT}}"
RESULT_DIR="${PROFILEFORGE_BUILD_RESULT_DIR:-${REPO_ROOT}/.agent/results/$(date +%Y%m%d-%H%M%S)-profileforge-build}"
JOBS="${CHRONOLOG_BUILD_JOBS:-$(nproc 2>/dev/null || echo 8)}"
TAU_PREFIX="${TAU_PREFIX:-${REPO_ROOT}/opt/tau-2.34}"

mkdir -p "${BUILD_DIR}" "${INSTALL_PREFIX}" "${RESULT_DIR}/chronolog"

exec > >(tee -a "${RESULT_DIR}/chronolog/stdout.log")
exec 2> >(tee -a "${RESULT_DIR}/chronolog/stderr.log" >&2)

source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true
type module >/dev/null 2>&1 || { echo "module command unavailable" >&2; exit 127; }

module load gcc/11.4.0/cmake/3.30.5-pq5ntgo
module load gcc/11.4.0/argobots/1.1-indjy5q
module load gcc/11.4.0/libfabric/1.22.0-lyviqpl
module load gcc/11.4.0/mercury/2.3.1-kcmn6u3
module load gcc/11.4.0/mochi-margo/0.17.0-llh62si
module load gcc/11.4.0/mochi-thallium/0.10.1-2pari3f
module load gcc/11.4.0/boost/1.86.0-cs4onpk
module load gcc/11.4.0/spdlog/1.14.1-y4quhau
module load gcc/11.4.0/json-c/0.16-2ofsb6t
module load gcc/11.4.0/pkgconf/2.2.0-3sczowe
module load gcc/11.4.0/python/3.11.9-zg4555e
module load gcc/11.4.0/py-pybind11/2.13.5-pngixx5
module load openmpi/5.0.5-x2kvx5l/gcc/11.4.0/hdf5/1.14.5-5zbyeqh

TAU_ARGS=()
if [[ "${BUILD_VARIANT}" == "tau" ]]; then
  TAU_ARGS=(-DCHRONOLOG_ENABLE_TAU_PROFILING=ON -DCHRONOLOG_TAU_ROOT="${TAU_PREFIX}")
fi

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -DCMAKE_CXX_FLAGS="-O2 -g -fno-omit-frame-pointer" \
  -DCHRONOLOG_BUILD_TESTING=OFF \
  "${TAU_ARGS[@]}"

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
cmake --install "${BUILD_DIR}"

cat > "${RESULT_DIR}/summary.md" <<EOF
# ProfileForge ChronoLog Build

- build_variant: ${BUILD_VARIANT}
- build_type: ${BUILD_TYPE}
- build_dir: ${BUILD_DIR}
- install_prefix: ${INSTALL_PREFIX}
- install_dir_for_harness: ${INSTALL_PREFIX}/chronolog
- jobs: ${JOBS}
- tau_prefix: ${TAU_PREFIX}
EOF

test -x "${INSTALL_PREFIX}/chronolog/bin/chrono-visor"
test -x "${INSTALL_PREFIX}/chronolog/bin/chrono-keeper"
test -x "${INSTALL_PREFIX}/chronolog/bin/chrono-grapher"
test -x "${INSTALL_PREFIX}/chronolog/bin/chrono-player"
test -x "${INSTALL_PREFIX}/chronolog/tools/benchmark/chrono-bench"

echo "ProfileForge ChronoLog build complete: ${INSTALL_PREFIX}/chronolog"
