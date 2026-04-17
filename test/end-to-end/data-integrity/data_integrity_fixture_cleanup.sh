#!/usr/bin/env bash
# FIXTURES_CLEANUP for end-to-end data-integrity tests.
#
# Stops the deployment and removes log/output artifacts. Best-effort: never
# fails the fixture even if the deployment was already down.

set -u

INSTALL_DIR="${CHRONOLOG_INSTALL_DIR:-$HOME/chronolog-install/chronolog}"
DEPLOY="${INSTALL_DIR}/tools/deploy/ChronoLog/deploy_local.sh"

if [[ ! -x "${DEPLOY}" ]]; then
    echo "[data-integrity:cleanup] No deploy script at ${DEPLOY}; nothing to do."
    exit 0
fi

echo "[data-integrity:cleanup] Stopping deployment"
"${DEPLOY}" --stop  || true
"${DEPLOY}" --clean || true

exit 0
