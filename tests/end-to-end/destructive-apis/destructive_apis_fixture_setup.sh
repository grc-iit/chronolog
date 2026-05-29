#!/usr/bin/env bash
# FIXTURES_SETUP for end-to-end destructive-API tests (#574).
#
# Assumes a ChronoLog stack is already running (the CI's Deploy step, or a
# developer's deploy_local.sh --start). Just verifies the install + output
# directory are present and the client config is readable; the per-test
# binaries do the actual API drive + assertions.

set -u

INSTALL_DIR="${CHRONOLOG_INSTALL_DIR:-$HOME/chronolog-install/chronolog}"
CLIENT_CONF="${INSTALL_DIR}/conf/default-chrono-client-conf.json"
OUTPUT_DIR="${OUTPUT_DIR:-${INSTALL_DIR}/output}"

if [[ ! -f "${CLIENT_CONF}" ]]; then
    echo "[destructive-apis:setup] client conf not found at ${CLIENT_CONF}"
    exit 1
fi

if [[ ! -d "${OUTPUT_DIR}" ]]; then
    echo "[destructive-apis:setup] OUTPUT_DIR does not exist: ${OUTPUT_DIR}"
    echo "[destructive-apis:setup] Expected a running deployment to have created it."
    exit 1
fi

echo "[destructive-apis:setup] install=${INSTALL_DIR}"
echo "[destructive-apis:setup] client_conf=${CLIENT_CONF}"
echo "[destructive-apis:setup] output_dir=${OUTPUT_DIR}"
echo "[destructive-apis:setup] ready"
exit 0
