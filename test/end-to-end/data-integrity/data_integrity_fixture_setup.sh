#!/usr/bin/env bash
# FIXTURES_SETUP for end-to-end data-integrity tests.
#
# Starts a local single-user ChronoLog deployment via deploy_local.sh, injects
# the reference input file as one story (chronicle_0_0/story_0_0) using
# chrono-client-admin -f, then waits long enough for the grapher to flush a
# CSV/HDF5 chunk.
#
# Required env (set by CMake when registering the fixture, OR by the user
# running ctest by hand):
#   REFERENCE_INPUT        – path to reference event payload file
#   OUTPUT_DIR             – directory where CSV + HDF5 chunks are written
#                            (single dir; deploy_local.sh --output-dir points
#                            keeper + grapher + player at the same location)
#
# Optional env:
#   CHRONOLOG_INSTALL_DIR  – install tree containing chronolog/{bin,conf,tools,...}
#                            Defaults to $HOME/chronolog-install/chronolog.
#
# The fixture exits 0 (skipping the dependent tests) when the install tree is
# missing — matching the project convention for tests that need a running
# stack but cannot be set up automatically.

set -u

INSTALL_DIR="${CHRONOLOG_INSTALL_DIR:-$HOME/chronolog-install/chronolog}"
DEPLOY="${INSTALL_DIR}/tools/deploy_local.sh"
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

if [[ -z "${OUTPUT_DIR:-}" ]]; then
    echo "[data-integrity:setup] OUTPUT_DIR must be set"
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

echo "[data-integrity:setup] Starting deployment from ${INSTALL_DIR}"
"${DEPLOY}" --start --output-dir "${OUTPUT_DIR}"

# Give the services a moment to come up and register.
sleep 5

echo "[data-integrity:setup] Injecting reference events from ${REFERENCE_INPUT}"
"${ADMIN}" -c "${CLIENT_CONF}" -f "${REFERENCE_INPUT}" \
    -h 1 -t 1 -n 1

# Wait for ChronoGrapher to flush its first HDF5 chunk to OUTPUT_DIR. With
# the bundled config (story_chunk_duration_secs=60, acceptance_window_secs=180)
# chunks land a few minutes after injection. 240s is the upper bound; if
# nothing appears the dependent tests will SKIP rather than block forever.
WAIT_DEADLINE=$((SECONDS + 240))
while (( SECONDS < WAIT_DEADLINE )); do
    h5_count=$(find "${OUTPUT_DIR}" -name '*.h5' 2>/dev/null | wc -l)
    if (( h5_count > 0 )); then
        echo "[data-integrity:setup] Detected ${h5_count} HDF5 file(s) in ${OUTPUT_DIR}"
        break
    fi
    sleep 5
done

echo "[data-integrity:setup] Setup complete."
exit 0
