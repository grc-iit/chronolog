#!/usr/bin/env bash
#
# run_breakdown.sh — sendrecv vs per-event RDMA timing breakdown.
#
# Starts ingestion_keeper_server on SERVER_HOST, runs the client on
# CLIENT_HOST (via SSH), and prints a per-event timing breakdown to
# stdout and a results file in the results/ directory.
#
# Both binaries are expected on a shared NFS path accessible from all nodes.
#
# The breakdown shows how each of the ingest latency components contributes:
#   Client RTT = network overhead + server handler time
#   Server handler (sendrecv) = thallium deser + queue enqueue
#   Server handler (RDMA)     = buf alloc + MR expose + RDMA pull + deser + enqueue
#
# USAGE:
#   ./run_breakdown.sh [options]
#
# OPTIONS:
#   --server-host HOST    SSH hostname for server node  (default: ares-comp-03-40g)
#   --server-ip   IP      IP for thallium address       (default: 172.25.101.3)
#   --client-host HOST    SSH hostname for client node  (default: ares-comp-04-40g)
#   --port        N       listen port                   (default: 5600)
#   --proto       PROTO   thallium protocol             (default: ofi+verbs;ofi_rxm)
#   --count       N       events per run                (default: 1000)
#   --size        N       payload bytes per event       (default: 4096)
#   --reps        N       repetitions to average        (default: 3)
#   --threads     N       server handler ULT count      (default: 4)
#   --queue  mutex|lockfree  queue implementation       (default: mutex)
#   --stories     N       story handle count            (default: 4)
#   --no-build            skip cmake build step

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$BENCH_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results"

# --- Defaults ---
SERVER_HOST="ares-comp-03-40g"
SERVER_IP="172.25.101.3"
CLIENT_HOST="ares-comp-04-40g"
PORT=5600
PROTO="ofi+verbs;ofi_rxm"
COUNT=1000
SIZE=4096
REPS=3
THREADS=4
QUEUE="mutex"
STORIES=4
DO_BUILD=1

# --- Parse CLI ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server-host) SERVER_HOST="$2"; shift 2 ;;
        --server-ip)   SERVER_IP="$2";   shift 2 ;;
        --client-host) CLIENT_HOST="$2"; shift 2 ;;
        --port)        PORT="$2";        shift 2 ;;
        --proto)       PROTO="$2";       shift 2 ;;
        --count)       COUNT="$2";       shift 2 ;;
        --size)        SIZE="$2";        shift 2 ;;
        --reps)        REPS="$2";        shift 2 ;;
        --threads)     THREADS="$2";     shift 2 ;;
        --queue)       QUEUE="$2";       shift 2 ;;
        --stories)     STORIES="$2";     shift 2 ;;
        --no-build)    DO_BUILD=0;       shift   ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SERVER_BIN="$BUILD_DIR/chrono-ingestion-bench-server"
CLIENT_BIN="$BUILD_DIR/chrono-ingestion-bench-client"
SERVER_ADDR="${PROTO}://${SERVER_IP}:${PORT}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUT_FILE="$RESULTS_DIR/breakdown_${TIMESTAMP}.txt"

mkdir -p "$RESULTS_DIR"

# --- Build ---
if (( DO_BUILD )); then
    echo "Building..."
    cmake -S "$BENCH_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$BUILD_DIR" --target ingestion_keeper_server ingestion_keeper_client -j "$(nproc)" > /dev/null
    echo "Build done."
fi

[[ -x "$SERVER_BIN" ]] || { echo "ERROR: $SERVER_BIN not found"; exit 1; }
[[ -x "$CLIENT_BIN" ]] || { echo "ERROR: $CLIENT_BIN not found"; exit 1; }

# --- Cleanup helper ---
kill_server() {
    ssh "$SERVER_HOST" "pkill -9 -f chrono-ingestion-bench-server 2>/dev/null || true" 2>/dev/null || true
}

# Run a single RPC mode on CLIENT_HOST and print output.
# $1 = rpc mode label (sendrecv|rdma)
run_mode() {
    local rpc="$1"
    ssh "$CLIENT_HOST" \
        "'$CLIENT_BIN' '$SERVER_ADDR' \
         --rpc '$rpc' \
         --count $COUNT --size $SIZE --reps $REPS \
         --queue '$QUEUE' --stories $STORIES" 2>&1
}

# --- Header ---
{
printf "=============================================================\n"
printf "  sendrecv vs RDMA timing breakdown\n"
printf "  date:     %s\n" "$(date)"
printf "  server:   %s  (%s)\n" "$SERVER_HOST" "$SERVER_IP"
printf "  client:   %s\n" "$CLIENT_HOST"
printf "  proto:    %s\n" "$PROTO"
printf "  count:    %d events/run\n" "$COUNT"
printf "  size:     %d B payload\n" "$SIZE"
printf "  reps:     %d (averaged)\n" "$REPS"
printf "  threads:  %d server handler ULTs\n" "$THREADS"
printf "  queue:    %s\n" "$QUEUE"
printf "=============================================================\n\n"
} | tee "$OUT_FILE"

# --- Start server ---
echo "Starting server on $SERVER_HOST..." | tee -a "$OUT_FILE"
kill_server
sleep 1

ssh "$SERVER_HOST" \
    "nohup '$SERVER_BIN' '${SERVER_ADDR}' \
     --threads $THREADS --queue $QUEUE --stories $STORIES --size $SIZE \
     > /tmp/keeper_server_breakdown.log 2>&1 &"
sleep 4

# Verify server started and read actual address (port may differ from hint)
ACTUAL_ADDR=$(ssh "$SERVER_HOST" "grep -oP 'address=\K\S+' /tmp/keeper_server_breakdown.log 2>/dev/null | head -1")
if [[ -z "$ACTUAL_ADDR" ]]; then
    echo "ERROR: server did not start. Log:" | tee -a "$OUT_FILE"
    ssh "$SERVER_HOST" "cat /tmp/keeper_server_breakdown.log 2>/dev/null" | tee -a "$OUT_FILE"
    kill_server
    exit 1
fi
echo "Server up: $ACTUAL_ADDR" | tee -a "$OUT_FILE"
echo "" | tee -a "$OUT_FILE"

# Use the address the server actually bound to
SERVER_ADDR="$ACTUAL_ADDR"

# --- Run sendrecv ---
echo "--- sendrecv ---" | tee -a "$OUT_FILE"
run_mode sendrecv | tee -a "$OUT_FILE"
echo "" | tee -a "$OUT_FILE"

# --- Run RDMA (naive: per-event malloc + ibv_reg_mr) ---
echo "--- rdma (naive: per-event malloc+ibv_reg_mr) ---" | tee -a "$OUT_FILE"
run_mode rdma | tee -a "$OUT_FILE"
echo "" | tee -a "$OUT_FILE"

# --- Run RDMA-pool (optimized: pre-pinned buffer pool) ---
echo "--- rdma-pool (optimized: pre-pinned buffer pool) ---" | tee -a "$OUT_FILE"
run_mode rdma-pool | tee -a "$OUT_FILE"
echo "" | tee -a "$OUT_FILE"

echo "Results saved to: $OUT_FILE" | tee -a "$OUT_FILE"

# --- Teardown ---
kill_server
