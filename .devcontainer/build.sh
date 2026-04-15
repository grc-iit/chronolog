#!/usr/bin/env bash
# Configures and builds ChronoLog in Release mode.
# Called by post-create.sh on first container creation; also safe to run manually.
set -euo pipefail

BUILD_DIR="${HOME}/chronolog-build/Release"
INSTALL_DIR="${HOME}/chronolog-install"
REPO_ROOT="$(realpath "$(dirname "${BASH_SOURCE[0]}")/..")"

echo "==> Configuring ChronoLog (Release)..."
cmake \
    -S "${REPO_ROOT}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCHRONOLOG_USE_ADDRESS_SANITIZER=OFF

echo "==> Building ChronoLog..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo "==> Build successful. Binaries in: ${BUILD_DIR}"
