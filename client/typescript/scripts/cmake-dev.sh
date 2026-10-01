#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-}" in
    --version|-E) exec cmake "$@" ;;
    --build) shift 2; exec cmake --build --preset dev "$@" ;;
    *) exec cmake --preset dev "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH:-}" "-DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR:?}" "$@" ;;
esac
