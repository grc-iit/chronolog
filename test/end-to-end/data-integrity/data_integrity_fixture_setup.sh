#!/usr/bin/env bash
# FIXTURES_SETUP for end-to-end data-integrity tests.
#
# Assumes a ChronoLog stack is already running (the CI pipeline's Deploy
# ChronoLog step, or a developer's own deploy_local.sh --start). Injects
# the reference input as one story (chronicle_0_0/story_0_0) via
# chrono-client-admin -f, then waits for the grapher to flush at least one
# HDF5 chunk into the running deployment's output directory.
#
# The fixture does NOT deploy the stack itself. An earlier version did, and
# collided with the pipeline's own deployment (double-bind on the visor /
# keeper / grapher / player ports) — the pipeline's visor stayed up and
# caught the injection, but the fixture's grapher never started, so HDF5
# files landed in the pipeline's output dir while tests watched an empty
# build-tree dir. Now both the wait and the tests read from the pipeline's
# OUTPUT_DIR (== $CHRONOLOG_INSTALL_DIR/output, the deploy_local.sh default).
#
# Required env:
#   REFERENCE_INPUT        – path to reference event payload file
#
# Optional env:
#   CHRONOLOG_INSTALL_DIR  – install tree (default: $HOME/chronolog-install/chronolog)
#   OUTPUT_DIR             – override watch dir (default: $CHRONOLOG_INSTALL_DIR/output)

set -u

INSTALL_DIR="${CHRONOLOG_INSTALL_DIR:-$HOME/chronolog-install/chronolog}"
ADMIN="${INSTALL_DIR}/bin/chrono-client-admin"
CLIENT_CONF="${INSTALL_DIR}/conf/chrono-client-conf.json"
OUTPUT_DIR="${OUTPUT_DIR:-${INSTALL_DIR}/output}"

if [[ ! -x "${ADMIN}" ]]; then
    echo "[data-integrity:setup] chrono-client-admin not found at ${ADMIN}; skipping."
    exit 1
fi

if [[ ! -f "${REFERENCE_INPUT:-}" ]]; then
    echo "[data-integrity:setup] REFERENCE_INPUT not set or missing: ${REFERENCE_INPUT:-}"
    exit 1
fi

if [[ ! -d "${OUTPUT_DIR}" ]]; then
    echo "[data-integrity:setup] OUTPUT_DIR does not exist: ${OUTPUT_DIR}"
    echo "[data-integrity:setup] Expected a running deployment to have created it."
    exit 1
fi

echo "[data-integrity:setup] Injecting reference events from ${REFERENCE_INPUT}"
echo "[data-integrity:setup] Watch dir: ${OUTPUT_DIR}"
if ! "${ADMIN}" -c "${CLIENT_CONF}" -f "${REFERENCE_INPUT}" -h 1 -t 1 -n 1; then
    echo "[data-integrity:setup] Injection via chrono-client-admin failed (non-zero exit)."
    exit 1
fi

# Wait for ChronoGrapher to flush its first HDF5 chunk matching the injected
# story. With the bundled config (story_chunk_duration_secs=60,
# acceptance_window_secs=180) chunks land a few minutes after injection.
STORY_PREFIX="chronicle_0_0.story_0_0."
WAIT_DEADLINE=$((SECONDS + 240))
while (( SECONDS < WAIT_DEADLINE )); do
    h5_count=$(find "${OUTPUT_DIR}" -maxdepth 1 -name "${STORY_PREFIX}*.h5" 2>/dev/null | wc -l)
    if (( h5_count > 0 )); then
        echo "[data-integrity:setup] Detected ${h5_count} HDF5 file(s) for ${STORY_PREFIX}* in ${OUTPUT_DIR}"
        # The grapher has flushed, but the player's HDF5ArchiveReadingAgent
        # picks up new archive files on its own fs-monitor cadence (default
        # ~60s). Give it one polling interval before Replay-lens tests fire,
        # otherwise chrono-client-admin -r returns zero events against a
        # player that hasn't loaded the file yet.
        PLAYER_POLL_WAIT="${DATA_INTEGRITY_PLAYER_POLL_WAIT:-65}"
        echo "[data-integrity:setup] Waiting ${PLAYER_POLL_WAIT}s for player to pick up the archive..."
        sleep "${PLAYER_POLL_WAIT}"
        echo "[data-integrity:setup] Setup complete."
        exit 0
    fi
    sleep 5
done

echo "[data-integrity:setup] Timed out waiting for HDF5 flush into ${OUTPUT_DIR}"
echo "[data-integrity:setup] Existing contents:"
ls -la "${OUTPUT_DIR}" || true
exit 1
