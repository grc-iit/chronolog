#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  phase0_verify_outside_lock_promotion.sh CONTROL_METRICS CANDIDATE_METRICS OUTPUT_DIR

Verifies a ChronoLog archive/range outside-lock promotion pair.

Required semantic difference:
  grapher_stop_drain_wait_outside_lock

Promotion gates:
  --claim-level promotion
  --min-node-count 4
  --min-archive-event-count 10000
  --min-data-bytes 655360000
  --require-profile-artifacts
EOF
}

if [[ "$#" -ne 3 ]]; then
  usage
  exit 2
fi

CONTROL_METRICS="$1"
CANDIDATE_METRICS="$2"
OUTPUT_DIR="$3"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 "${SCRIPT_DIR}/phase0_verify_archive_ab.py" \
  --claim-level promotion \
  --control "${CONTROL_METRICS}" \
  --candidate "${CANDIDATE_METRICS}" \
  --allow-difference grapher_stop_drain_wait_outside_lock \
  --min-node-count 4 \
  --min-archive-event-count 10000 \
  --min-data-bytes 655360000 \
  --require-profile-artifacts \
  --output-dir "${OUTPUT_DIR}"
