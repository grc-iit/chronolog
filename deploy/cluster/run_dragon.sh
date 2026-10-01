#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/cluster
exec systemd-run --user --scope --quiet -p MemoryMax=4G -p MemorySwapMax=0 \
    timeout -k 90 820 bash -c '
        python3 deploy/cluster/cluster.py "$@" & driver=$!
        trap '\''kill -TERM "$driver" 2>/dev/null || true; wait "$driver" || true'\'' EXIT
        trap '\''exit 130'\'' INT TERM
        wait "$driver"
    ' cluster-driver "$@"
