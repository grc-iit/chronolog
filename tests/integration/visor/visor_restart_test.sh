#!/usr/bin/env bash
# Visor crash and failover gate (section 12, I9.1): kill the leader of three Visor replicas mid-workload, then kill
# and restart all three. The Catalog revision and acquisitions survive and writers keep appending. The stack comes
# from tests/integration/dynamic/run.py, imported by visor/visor_restart.py, on random loopback port blocks.
set -eu
visor=$1 keeper=$2 grapher=$3 player=$4 rpc=$5
exec timeout -k 15 300 python3 "$(dirname "$0")/visor/visor_restart.py" \
    --visor "$visor" --keeper "$keeper" --grapher "$grapher" --player "$player" --rpc "$rpc"
