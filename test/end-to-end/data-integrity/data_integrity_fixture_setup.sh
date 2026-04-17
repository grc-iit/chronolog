#!/usr/bin/env bash
# FIXTURES_SETUP for end-to-end data-integrity tests.
#
# Starts a local single-user ChronoLog deployment via deploy_local.sh, injects
# the reference input file as one story (chronicle_0_0/story_0_0) using
# chrono-client-admin -f, then waits long enough for the grapher to flush a
# CSV chunk and for the player/store to flush an HDF5 chunk.
#
# Required env (set by CMake when registering the fixture):
#   CHRONOLOG_INSTALL_DIR  – install tree containing chronolog/{bin,conf,...}
#   REFERENCE_INPUT        – path to reference event payload file
#   CSV_OUTPUT_DIR         – directory where CSV chunks are written
#   HDF5_OUTPUT_DIR        – directory where HDF5 archives are written
#
# The fixture exits 0 (skipping the dependent tests) when the install tree is
# missing — matching the project convention for tests that need a running
# stack but cannot be set up automatically.

set -u

INSTALL_DIR="${CHRONOLOG_INSTALL_DIR:-$HOME/chronolog-install/chronolog}"
DEPLOY="${INSTALL_DIR}/tools/deploy/ChronoLog/deploy_local.sh"
ADMIN="${INSTALL_DIR}/bin/chrono-client-admin"
CLIENT_CONF="${INSTALL_DIR}/conf/chrono-client-conf.json"

if [[ ! -x "${DEPLOY}" || ! -x "${ADMIN}" ]]; then
    echo "[data-integrity:setup] ChronoLog install not found at ${INSTALL_DIR}; skipping."
    echo "[data-integrity:setup] Set CHRONOLOG_INSTALL_DIR to enable end-to-end tests."
    exit 0
fi

if [[ ! -f "${REFERENCE_INPUT:-}" ]]; then
    echo "[data-integrity:setup] REFERENCE_INPUT not set or missing: ${REFERENCE_INPUT:-}"
    exit 1
fi

mkdir -p "${CSV_OUTPUT_DIR}" "${HDF5_OUTPUT_DIR}"

echo "[data-integrity:setup] Starting deployment from ${INSTALL_DIR}"
"${DEPLOY}" --start --output-dir "${CSV_OUTPUT_DIR}"

# Give the services a moment to come up and register.
sleep 5

echo "[data-integrity:setup] Injecting reference events from ${REFERENCE_INPUT}"
"${ADMIN}" -c "${CLIENT_CONF}" -f "${REFERENCE_INPUT}" \
    -h 1 -t 1 -n 1

# Wait for grapher CSV flush + player HDF5 flush. The default chunk granularity
# in the bundled config is on the order of seconds; 30s is a safe upper bound
# for the small reference input.
WAIT_DEADLINE=$((SECONDS + 60))
while (( SECONDS < WAIT_DEADLINE )); do
    csv_count=$(find "${CSV_OUTPUT_DIR}" -maxdepth 1 -name '*.csv' 2>/dev/null | wc -l)
    h5_count=$(find "${HDF5_OUTPUT_DIR}" -name '*.h5' 2>/dev/null | wc -l)
    if (( csv_count > 0 && h5_count > 0 )); then
        echo "[data-integrity:setup] Detected ${csv_count} CSV file(s), ${h5_count} HDF5 file(s)"
        break
    fi
    sleep 2
done

echo "[data-integrity:setup] Setup complete."
exit 0
