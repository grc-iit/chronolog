#!/bin/bash
# Benchmark script to determine the send/recv ↔ RDMA crossover point.
# Iterates over message sizes (and optionally repetition counts), runs both
# modes, records bandwidth & latency.  Server and client run on separate
# nodes via SSH.
#
# Supports:
#   - Single client (thallium_client) or MPI concurrent (thallium_client_mpi)
#   - Running both protocols (-p both) for crossover surface analysis
#   - Varying repetition counts (-R "10,100,1000") for 2D surface
#   - Auto-generating interactive HTML via plot_crossover.py
#
# Usage:
#   # Single protocol, single client:
#   ./thallium_crossover_benchmark.sh -p ofi+sockets -s node01 -c node02
#
#   # Both protocols (crossover analysis):
#   ./thallium_crossover_benchmark.sh -p both -s node01 -c node02
#
#   # 2D surface: both protocols, varying reps:
#   ./thallium_crossover_benchmark.sh -p both -s node01 -c node02 -R "10,100,1000,5000"
#
#   # MPI concurrent clients:
#   ./thallium_crossover_benchmark.sh -p both -s node01 -c node02 -m 4

set -euo pipefail

# ==================== Quick-edit defaults ====================
PROTOCOL="ofi+sockets"
SERVER_PORT=5555
WORK_DIR=""
OUTPUT_FILE=""
SERVER_HOST=""
CLIENT_HOST=""
TIMEOUT=60
AUTO_REPS=1        # 1 = auto-calculate repetitions, 0 = use FIXED_REPS
FIXED_REPS=100
REP_COUNTS=""      # comma-separated rep counts for 2D surface (overrides auto/fixed)
SERVER_STREAMS=1
SERVER_PORTS=1
MPI_NPROCS=0       # 0 = single client, >0 = MPI with N processes
MPI_EXTRA_ARGS=""   # extra args for mpirun (e.g., "--hostfile hosts.txt")
NO_PLOT=0           # 1 = skip HTML generation

# Message sizes: powers of 2 from 64B to 64MB
MSG_SIZES=(64 128 256 512 1024 2048 4096 8192 16384 32768 65536
           131072 262144 524288 1048576 2097152 4194304 8388608
           16777216 33554432 67108864)
# =============================================================

help() {
    cat <<'EOF'
Thallium send/recv vs RDMA crossover benchmark

Usage: $0 -p <protocol> -s <server_host> -c <client_host> [options]

Required:
  -p PROTOCOL      ofi+sockets, ofi+verbs, or "both" (runs both sequentially)
  -s SERVER_HOST   Hostname/IP of the server node
  -c CLIENT_HOST   Hostname/IP of the client node

Optional:
  -P PORT          Server port (default: 5555)
  -w WORK_DIR      Directory containing binaries (default: build/test/communication/)
  -o OUTPUT_FILE   Output CSV file (default: auto-generated with timestamp)
  -r REPS          Fixed repetition count (overrides auto-calc)
  -R REP_LIST      Comma-separated rep counts for 2D surface (e.g., "10,100,1000,5000")
  -t TIMEOUT       Client timeout in seconds (default: 60)
  -S SERVER_IP     Server IP for address string (default: SERVER_HOST)
  -m NPROCS        MPI concurrent clients: number of processes (default: 0 = single)
  -M MPI_ARGS      Extra mpirun arguments (e.g., "--hostfile hosts.txt")
  --no-plot        Skip interactive HTML generation
  -h               Show this help
EOF
}

while getopts "p:s:c:P:w:o:r:R:t:S:m:M:h-:" opt; do
    case ${opt} in
        p) PROTOCOL="${OPTARG}" ;;
        s) SERVER_HOST="${OPTARG}" ;;
        c) CLIENT_HOST="${OPTARG}" ;;
        P) SERVER_PORT="${OPTARG}" ;;
        w) WORK_DIR="${OPTARG}" ;;
        o) OUTPUT_FILE="${OPTARG}" ;;
        r) FIXED_REPS="${OPTARG}"; AUTO_REPS=0 ;;
        R) REP_COUNTS="${OPTARG}"; AUTO_REPS=0 ;;
        t) TIMEOUT="${OPTARG}" ;;
        S) SERVER_IP_OVERRIDE="${OPTARG}" ;;
        m) MPI_NPROCS="${OPTARG}" ;;
        M) MPI_EXTRA_ARGS="${OPTARG}" ;;
        h) help; exit 0 ;;
        -) case "${OPTARG}" in
               no-plot) NO_PLOT=1 ;;
               *) echo "Unknown option: --$OPTARG" >&2; exit 1 ;;
           esac ;;
        \?) echo "Invalid option: -$OPTARG" >&2; help; exit 1 ;;
        :) echo "Option -$OPTARG requires an argument." >&2; exit 1 ;;
    esac
done

# --- Validate required args ---
if [[ -z "$SERVER_HOST" || -z "$CLIENT_HOST" || -z "$PROTOCOL" ]]; then
    echo "Error: -p, -s, -c are required." >&2
    help
    exit 1
fi

# Build protocol list
if [[ "$PROTOCOL" == "both" ]]; then
    PROTO_LIST=("ofi+sockets" "ofi+verbs")
elif [[ "$PROTOCOL" == "ofi+sockets" || "$PROTOCOL" == "ofi+verbs" ]]; then
    PROTO_LIST=("$PROTOCOL")
else
    echo "Error: protocol must be ofi+sockets, ofi+verbs, or both. Got: $PROTOCOL" >&2
    exit 1
fi

# Build rep count list
if [[ -n "$REP_COUNTS" ]]; then
    IFS=',' read -ra REP_LIST <<< "$REP_COUNTS"
else
    REP_LIST=()  # empty = use auto or fixed per msg_size
fi

# --- Resolve paths ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [[ -z "$WORK_DIR" ]]; then
    WORK_DIR="$REPO_ROOT/build/test/communication"
fi

if [[ -z "$OUTPUT_FILE" ]]; then
    PROTO_SHORT="${PROTOCOL//+/_}"
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    if [[ $MPI_NPROCS -gt 0 ]]; then
        OUTPUT_FILE="${SCRIPT_DIR}/crossover_${PROTO_SHORT}_mpi${MPI_NPROCS}_${TIMESTAMP}.csv"
    else
        OUTPUT_FILE="${SCRIPT_DIR}/crossover_${PROTO_SHORT}_${TIMESTAMP}.csv"
    fi
fi

SERVER_BIN="$WORK_DIR/thallium_server"
if [[ $MPI_NPROCS -gt 0 ]]; then
    CLIENT_BIN="$WORK_DIR/thallium_client_mpi"
    CLIENT_MODE="mpi"
else
    CLIENT_BIN="$WORK_DIR/thallium_client"
    CLIENT_MODE="single"
fi

# --- Resolve server IP ---
if [[ -n "${SERVER_IP_OVERRIDE:-}" ]]; then
    SERVER_IP="$SERVER_IP_OVERRIDE"
else
    SERVER_IP="$SERVER_HOST"
fi

echo "============================================"
echo "Thallium Crossover Benchmark"
echo "============================================"
echo "  Protocol(s):  ${PROTO_LIST[*]}"
echo "  Server:       $SERVER_HOST (addr: $SERVER_IP:$SERVER_PORT)"
echo "  Client:       $CLIENT_HOST"
if [[ $MPI_NPROCS -gt 0 ]]; then
    echo "  Client mode:  MPI ($MPI_NPROCS processes)"
    [[ -n "$MPI_EXTRA_ARGS" ]] && echo "  MPI args:     $MPI_EXTRA_ARGS"
else
    echo "  Client mode:  single process"
fi
echo "  Work dir:     $WORK_DIR"
echo "  Output:       $OUTPUT_FILE"
echo "  Msg sizes:    ${MSG_SIZES[0]}B .. ${MSG_SIZES[-1]}B (${#MSG_SIZES[@]} points)"
if [[ ${#REP_LIST[@]} -gt 0 ]]; then
    echo "  Rep counts:   ${REP_LIST[*]} (2D surface mode)"
elif [[ $AUTO_REPS -eq 1 ]]; then
    echo "  Repetitions:  auto (~100MB total transfer per point)"
else
    echo "  Repetitions:  $FIXED_REPS (fixed)"
fi
echo "  Timeout:      ${TIMEOUT}s per run"
echo "============================================"
echo ""

# --- Check binaries ---
echo "Checking binaries on remote hosts..."
if ! ssh "$SERVER_HOST" "test -x $SERVER_BIN" 2>/dev/null; then
    echo "Warning: $SERVER_BIN not found on $SERVER_HOST"
fi
if ! ssh "$CLIENT_HOST" "test -x $CLIENT_BIN" 2>/dev/null; then
    echo "Warning: $CLIENT_BIN not found on $CLIENT_HOST"
fi

# --- Helpers ---
calc_reps() {
    local size=$1
    if [[ $AUTO_REPS -eq 0 && ${#REP_LIST[@]} -eq 0 ]]; then
        echo $FIXED_REPS
        return
    fi
    local target=104857600
    local reps=$((target / size))
    [[ $reps -lt 3 ]] && reps=3
    [[ $reps -gt 10000 ]] && reps=10000
    echo $reps
}

kill_server() {
    ssh "$SERVER_HOST" "killall -9 thallium_server 2>/dev/null" 2>/dev/null || true
    sleep 1
}

start_server() {
    local proto=$1
    local addr="${proto}://${SERVER_IP}:${SERVER_PORT}"
    kill_server
    ssh "$SERVER_HOST" "ABT_THREAD_STACKSIZE=2097152 $SERVER_BIN '$addr' $SERVER_STREAMS $SERVER_PORTS" &
    SERVER_PID=$!
    sleep 3
    echo "  Server started (PID $SERVER_PID) at $addr"
}

parse_kv() {
    local output=$1 key=$2
    echo "$output" | grep "^${key}:" | head -1 | sed 's/^[^:]*:[[:space:]]*//' | awk '{print $1}'
}

run_client_single() {
    local proto=$1 mode=$2 msg_size=$3 reps=$4
    local addr="${proto}://${SERVER_IP}:${SERVER_PORT}"

    local output
    output=$(timeout "${TIMEOUT}s" ssh "$CLIENT_HOST" "$CLIENT_BIN '$addr' $mode $msg_size $reps" 2>/dev/null) || {
        echo "  ERROR: client timed out (mode=$mode, size=$msg_size)" >&2
        return 1
    }

    local total_time avg_latency bandwidth
    total_time=$(parse_kv "$output" "total_time")
    avg_latency=$(parse_kv "$output" "avg_latency")
    bandwidth=$(parse_kv "$output" "bandwidth")

    if [[ -z "$avg_latency" ]]; then
        echo "  ERROR: no results (mode=$mode, size=$msg_size)" >&2
        return 1
    fi

    echo "${proto},${mode},${msg_size},${reps},${total_time},${avg_latency},${bandwidth}" >> "$OUTPUT_FILE"
    echo "avg_latency=${avg_latency} bandwidth=${bandwidth}"
}

run_client_mpi() {
    local proto=$1 mode=$2 msg_size=$3 reps=$4
    local addr="${proto}://${SERVER_IP}:${SERVER_PORT}"
    local total_reps=$((reps + 3))

    local output
    output=$(timeout "${TIMEOUT}s" ssh "$CLIENT_HOST" \
        "mpirun -np $MPI_NPROCS $MPI_EXTRA_ARGS $CLIENT_BIN '$addr' $SERVER_PORTS $mode $msg_size $total_reps" \
        2>/dev/null) || {
        echo "  ERROR: MPI client timed out (mode=$mode, size=$msg_size)" >&2
        return 1
    }

    local reps_actual nprocs total_comm avg_latency bw_per_client agg_bandwidth
    reps_actual=$(parse_kv "$output" "repetitions")
    nprocs=$(parse_kv "$output" "nprocs")
    total_comm=$(parse_kv "$output" "total_comm")
    avg_latency=$(parse_kv "$output" "avg_latency")
    bw_per_client=$(parse_kv "$output" "bw_per_client")
    agg_bandwidth=$(parse_kv "$output" "agg_bandwidth")

    if [[ -z "$avg_latency" ]]; then
        echo "  ERROR: no results (mode=$mode, size=$msg_size)" >&2
        return 1
    fi

    echo "${proto},${mode},${msg_size},${reps_actual},${nprocs},${total_comm},${avg_latency},${bw_per_client},${agg_bandwidth}" >> "$OUTPUT_FILE"
    echo "avg_latency=${avg_latency} agg_bandwidth=${agg_bandwidth}"
}

fmt_size() {
    local s=$1
    if [[ $s -ge 1048576 ]]; then echo "$((s / 1048576))MB"
    elif [[ $s -ge 1024 ]]; then echo "$((s / 1024))KB"
    else echo "${s}B"; fi
}

# --- CSV header ---
if [[ $MPI_NPROCS -gt 0 ]]; then
    echo "protocol,mode,msg_size,repetitions,nprocs,total_comm_us,avg_latency_us,bw_per_client_MBps,agg_bw_MBps" > "$OUTPUT_FILE"
else
    echo "protocol,mode,msg_size,repetitions,total_time_s,avg_latency_us,bandwidth_MBps" > "$OUTPUT_FILE"
fi

# --- Main benchmark loop ---
trap 'echo "Cleaning up..."; kill_server; exit 1' INT TERM

for proto in "${PROTO_LIST[@]}"; do
    echo ""
    echo "======== Protocol: $proto ========"
    start_server "$proto"

    for msg_size in "${MSG_SIZES[@]}"; do
        hr=$(fmt_size "$msg_size")

        # Determine rep counts for this msg_size
        if [[ ${#REP_LIST[@]} -gt 0 ]]; then
            reps_to_run=("${REP_LIST[@]}")
        else
            reps_to_run=($(calc_reps "$msg_size"))
        fi

        for reps in "${reps_to_run[@]}"; do
            echo "--- $hr ($msg_size bytes), $reps reps ---"

            for mode in "recv" "rdma"; do
                printf "  %-6s ... " "$mode"

                if [[ "$CLIENT_MODE" == "mpi" ]]; then
                    result=$(run_client_mpi "$proto" "$mode" "$msg_size" "$reps" 2>&1) || continue
                else
                    result=$(run_client_single "$proto" "$mode" "$msg_size" "$reps" 2>&1) || continue
                fi

                lat=$(echo "$result" | sed -n 's/.*avg_latency=\([^ ]*\).*/\1/p')
                bw=$(echo "$result" | sed -n 's/.*\(bandwidth\|agg_bandwidth\)=\([^ ]*\).*/\2/p')
                if [[ "$CLIENT_MODE" == "mpi" ]]; then
                    printf "lat=%s us  agg_bw=%s MB/s\n" "$lat" "$bw"
                else
                    printf "lat=%s us  bw=%s MB/s\n" "$lat" "$bw"
                fi
            done
        done
    done

    echo "  Stopping server for $proto..."
    kill_server
done

# --- Generate interactive HTML ---
HTML_FILE="${OUTPUT_FILE%.csv}.html"
if [[ $NO_PLOT -eq 0 ]]; then
    echo ""
    echo "Generating interactive HTML..."
    if command -v python3 &>/dev/null; then
        python3 "$SCRIPT_DIR/plot_crossover.py" "$OUTPUT_FILE" -o "$HTML_FILE" && \
            echo "  HTML: $HTML_FILE" || \
            echo "  Warning: HTML generation failed (check python3 + csv format)"
    else
        echo "  Skipped: python3 not found. Run manually:"
        echo "    python3 $SCRIPT_DIR/plot_crossover.py $OUTPUT_FILE -o $HTML_FILE"
    fi
fi

echo ""
echo "============================================"
echo "Benchmark complete."
echo "  CSV:  $OUTPUT_FILE"
[[ $NO_PLOT -eq 0 && -f "$HTML_FILE" ]] && echo "  HTML: $HTML_FILE (open in browser)"
echo ""
if [[ ${#PROTO_LIST[@]} -ge 2 ]]; then
    echo "The HTML shows an interactive crossover surface."
    echo "Blue = sockets faster, Red = verbs faster."
fi
echo "============================================"
