#!/usr/bin/env bash
#
# sweep.sh — Ingestion kernel benchmark parameter sweep.
#
# TWO MODES:
#
#   LOCAL (default) — single-node, no network.
#     Sweeps: queue × threads
#     Binary: chrono-ingestion-bench
#
#   NETWORK — multi-node via SSH.
#     Server:  single node from hosts_server (or --server HOST)
#     Clients: one or more nodes from hosts_client (or --clients "h1 h2 ...")
#     Sweeps:  rpc × queue × threads × n_clients  (4D matrix)
#     Protocol: reset → parallel send → finalize per rep (script-managed coordination)
#     Binaries: chrono-ingestion-bench-server  (server node)
#               chrono-ingestion-bench-client  (each client node, simultaneously)
#
# MULTI-CLIENT COORDINATION:
#   For each data point the script runs REPS repetitions of:
#     1. coordinator (first client) calls --reset-only  → clears server stats
#     2. n_clients nodes run --send-only in parallel    → fill ingestion queues
#     3. coordinator calls --finalize-only              → merge + report timing
#   This cleanly separates phases so server timing captures concurrent load.
#
# SPEEDUP DIMENSIONS (visible in plots):
#   n_clients:  horizontal scaling — more client nodes hitting one server
#   queue:      container optimization — lockfree vs mutex (STL) ingestion queue
#   threads:    server-side handler ULT count
#   rpc:        transport — sendrecv (one RPC/event) vs rdma (bulk RDMA transfer)
#
# HOST FILES (auto-read; overridden by CLI flags):
#   hosts_server  — one hostname per line; first entry is the server node
#   hosts_client  — one hostname per line; swept from 1 to N clients
#
# USAGE EXAMPLE:
#   # Network mode using host files (auto-detected):
#   ./sweep.sh --server-ip 172.25.x.y --proto-sendrecv ofi+sockets
#
#   # Override hosts explicitly:
#   ./sweep.sh --server ares-comp-03-40g \
#              --clients "ares-comp-04-40g ares-comp-05-40g" \
#              --server-ip 172.25.x.y --threads "1 4 8 16"
#
#   # Local mode (no server/hosts_server):
#   ./sweep.sh --queues mutex --threads "1 2 4 8"
#
# OUTPUT:
#   results/sweep_YYYYMMDD_HHMMSS.log   full run log (stdout + stderr)
#   results/results_local.csv           local mode results
#   results/results_network.csv         network mode results
#   results/*.png                       plots (requires matplotlib)
#
# ALL OPTIONS:
#   --build-dir DIR        build directory (default: <script_dir>/build)
#   --out-dir DIR          output directory for CSV and plots (default: <script_dir>/results)
#   --server HOST          server hostname (overrides hosts_server; enables network mode)
#   --server-ip IP         server IP for thallium address (default: server hostname)
#   --clients "h1 h2 ..."  client hostnames (overrides hosts_client)
#   --client-counts "1 2"  n_clients values to sweep (default: 1 .. len(clients))
#   --proto PROTO          set both transports to the same value
#   --proto-sendrecv PROTO transport for sendrecv RPC mode (default: ofi+sockets)
#   --proto-rdma PROTO     transport for rdma RPC mode     (default: ofi+verbs)
#   --port N               server listen port (default: 5600)
#   --rpcs "sendrecv rdma" RPC modes to sweep in network mode
#   --queues "mutex lockfree"
#   --threads "1 2 4 8 16" space-separated server-side handler ULT counts
#   --stories N
#   --count N              events per client per rep (default: 2000000 local, 500000 network)
#   --size N               payload bytes per event (default: 256)
#   --reps N               repetitions averaged per data point (default: 3)
#   --server-startup-s N   seconds to wait for server to start (default: 3)
#   --no-build             skip cmake build step
#   --no-plot              skip plot generation

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
OUT_DIR="$SCRIPT_DIR/results"

# ---- sweep axes ----
THREAD_COUNTS=(1 2 4 8 16)
QUEUE_MODES=(mutex lockfree)
RPC_MODES=(sendrecv rdma)
CLIENT_COUNTS=()  # filled after host detection; empty = auto (1 .. len(CLIENT_NODES))
PROGRESS_THREADS=(1) # Margo progress ES count; >1 requires a thread-safe transport (ofi+tcp)

# ---- workload knobs ----
STORIES_LIST=(4)   # space-separated story counts; use "match" to set stories=n_clients
BATCH_SIZES=(1000) # events per RPC for --rpc batch; ignored for other rpc modes
COUNT=""
SIZE=256
REPS=3

# ---- network settings ----
SERVER_HOST=""
CLIENT_NODES=()
SERVER_IP=""
PROTO_SENDRECV="ofi+sockets"
PROTO_RDMA="ofi+verbs"
PORT=5600
SERVER_STARTUP_S=3
ABT_THREAD_STACKSIZE="${ABT_THREAD_STACKSIZE:-2097152}"

# ---- build/plot flags ----
DO_BUILD=1
DO_PLOT=1

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)        BUILD_DIR="$2";                              shift 2 ;;
        --out-dir)          OUT_DIR="$(realpath -m "$2")";               shift 2 ;;
        --server)           SERVER_HOST="$2";                            shift 2 ;;
        --server-ip)        SERVER_IP="$2";                              shift 2 ;;
        --clients)          read -r -a CLIENT_NODES      <<< "$2";      shift 2 ;;
        --client)           CLIENT_NODES=("$2");                         shift 2 ;;  # legacy single-client
        --client-counts)    read -r -a CLIENT_COUNTS     <<< "$2";      shift 2 ;;
        --proto)            PROTO_SENDRECV="$2"; PROTO_RDMA="$2";       shift 2 ;;
        --proto-sendrecv)   PROTO_SENDRECV="$2";                        shift 2 ;;
        --proto-rdma)       PROTO_RDMA="$2";                            shift 2 ;;
        --port)             PORT="$2";                                   shift 2 ;;
        --rpcs)             read -r -a RPC_MODES    <<< "$2";           shift 2 ;;
        --queues)           read -r -a QUEUE_MODES  <<< "$2";           shift 2 ;;
        --threads)          read -r -a THREAD_COUNTS <<< "$2";          shift 2 ;;
        --stories)          read -r -a STORIES_LIST  <<< "$2";           shift 2 ;;
        --batch-sizes)      read -r -a BATCH_SIZES        <<< "$2";       shift 2 ;;
        --progress-threads) read -r -a PROGRESS_THREADS  <<< "$2";       shift 2 ;;
        --count)            COUNT="$2";                                  shift 2 ;;
        --size)             SIZE="$2";                                   shift 2 ;;
        --reps)             REPS="$2";                                   shift 2 ;;
        --server-startup-s) SERVER_STARTUP_S="$2";                      shift 2 ;;
        --no-build)         DO_BUILD=0;                                  shift   ;;
        --no-plot)          DO_PLOT=0;                                   shift   ;;
        --help|-h)
            sed -n '2,/^# ALL OPTIONS/{ /^# ALL OPTIONS/q; s/^# *//p }' "$0"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Load host files (CLI flags take precedence)
# ---------------------------------------------------------------------------
if [[ -z "$SERVER_HOST" && -f "$SCRIPT_DIR/hosts_server" ]]; then
    SERVER_HOST=$(grep -vE '^\s*#|^\s*$' "$SCRIPT_DIR/hosts_server" | head -1)
fi
if [[ ${#CLIENT_NODES[@]} -eq 0 && -f "$SCRIPT_DIR/hosts_client" ]]; then
    mapfile -t CLIENT_NODES < <(grep -vE '^\s*#|^\s*$' "$SCRIPT_DIR/hosts_client")
fi

# ---------------------------------------------------------------------------
# Detect mode and set defaults
# ---------------------------------------------------------------------------
if [[ -n "$SERVER_HOST" ]]; then
    NETWORK_MODE=1
    [[ -z "$SERVER_IP" ]] && SERVER_IP="$SERVER_HOST"
    [[ -z "$COUNT"     ]] && COUNT=500000
    if [[ ${#CLIENT_NODES[@]} -eq 0 ]]; then
        echo "ERROR: network mode requires client nodes (hosts_client file or --clients)"
        exit 1
    fi
    # Auto-derive CLIENT_COUNTS if not specified: 1, 2, ..., N
    if [[ ${#CLIENT_COUNTS[@]} -eq 0 ]]; then
        for (( c=1; c<=${#CLIENT_NODES[@]}; c++ )); do
            CLIENT_COUNTS+=("$c")
        done
    fi
else
    NETWORK_MODE=0
    [[ -z "$COUNT" ]] && COUNT=2000000
fi

# ---------------------------------------------------------------------------
# Prepare output directory and log file
# ---------------------------------------------------------------------------
mkdir -p "$OUT_DIR"
LOG_FILE="$OUT_DIR/sweep_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG_FILE") 2>&1
echo "Sweep log: $LOG_FILE"
echo

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if (( DO_BUILD )); then
    echo "Building..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$BUILD_DIR" -j "$(nproc)" > /dev/null
    echo "Build done."
    echo
fi

# ---------------------------------------------------------------------------
# ════════════════════════════  LOCAL MODE  ════════════════════════════════
# Sweeps: queue × threads (single node, no network)
# ---------------------------------------------------------------------------
run_local() {
    local BENCH="$BUILD_DIR/chrono-ingestion-bench"
    [[ -x "$BENCH" ]] || { echo "ERROR: $BENCH not found"; exit 1; }

    local CSV="$OUT_DIR/results_local.csv"
    "$BENCH" --header > "$CSV"

    local total=$(( ${#QUEUE_MODES[@]} * ${#THREAD_COUNTS[@]} ))
    local i=0
    echo "LOCAL sweep: queues=[${QUEUE_MODES[*]}] threads=[${THREAD_COUNTS[*]}]"
    echo "             stories=$STORIES count=$COUNT size=${SIZE}B reps=$REPS"
    echo

    for qm in "${QUEUE_MODES[@]}"; do
        for nt in "${THREAD_COUNTS[@]}"; do
            i=$(( i + 1 ))
            printf "[%2d/%2d] queue=%-9s threads=%2d ... " "$i" "$total" "$qm" "$nt"
            "$BENCH" \
                --queue   "$qm" \
                --threads "$nt" \
                --stories "$STORIES" \
                --count   "$COUNT" \
                --size    "$SIZE" \
                --reps    "$REPS" >> "$CSV"
            tail -1 "$CSV" | awk -F',' '{printf "ingest=%7s Mev/s  pipeline=%7s Mev/s\n", $8, $9}'
        done
    done

    echo
    echo "CSV: $CSV"
    _print_local_table "$CSV"

    if (( DO_PLOT )); then
        python3 "$SCRIPT_DIR/plot_results.py" "$CSV" "$OUT_DIR" 2>/dev/null \
            && echo "Plots: $OUT_DIR/" \
            || echo "(plot skipped — matplotlib not available)"
    fi
}

_print_local_table() {
    python3 - "$1" <<'PY'
import csv, sys
from collections import defaultdict
with open(sys.argv[1]) as f:
    rows = list(csv.DictReader(f))
by_t = defaultdict(dict)
for r in rows:
    by_t[int(r['threads'])][r['queue']] = float(r['ingest_Mevs'])
queues = sorted({r['queue'] for r in rows})
print("\nIngest throughput (Mev/s):")
hdr = "  threads | " + " | ".join(f"{q:>10s}" for q in queues) + " | speedup"
print(hdr); print("  " + "-" * (len(hdr) - 2))
for t in sorted(by_t):
    cells = by_t[t]
    cols = " | ".join(f"{cells.get(q, float('nan')):>10.3f}" for q in queues)
    if 'mutex' in cells and 'lockfree' in cells and cells['mutex'] > 0:
        sp = cells['lockfree'] / cells['mutex']
        print(f"  {t:>7d} | {cols} | {sp:>6.2f}x")
    else:
        print(f"  {t:>7d} | {cols} |    n/a")
PY
}

# ---------------------------------------------------------------------------
# ══════════════════════════  NETWORK MODE  ════════════════════════════════
# Sweeps: rpc × queue × threads × n_clients  (4D matrix)
#
# Outer loops: rpc × queue × threads → restart server for each triple
# Inner loop:  n_clients → sweep client count without server restart
#
# Per data point, REPS repetitions of:
#   reset_stats (coordinator) → parallel --send-only → finalize (coordinator)
# ---------------------------------------------------------------------------

_kill_server() {
    ssh "$SERVER_HOST" \
        "killall -9 chrono-ingestion-bench-server 2>/dev/null || true" \
        2>/dev/null || true
    sleep 6   # OFI/verbs kernel resources (QP, CQ, MR) need time to drain after SIGKILL
              # before a new server can bind the same address without heap corruption
}

# Launch n_procs concurrent client processes on a single node via one SSH session.
# Blocks until all processes on that node finish.  Runs in a subshell (called with &).
# The generated launcher script lives on the shared NFS path and is removed on exit.
_launch_clients_on_node() {
    local chost="$1"
    local n_procs="$2"
    local client_bin="$3"
    local server_addr="$4"
    local rpc="$5"      # already the bare rpc name (batch, sendrecv, …)
    local count="$6"
    local size="$7"
    local stories="$8"
    local batch_size="${9:-}"  # non-empty only for --rpc batch

    local batch_arg=""
    [[ -n "$batch_size" ]] && batch_arg="--batch-size ${batch_size}"

    local tmpscript
    tmpscript=$(mktemp "${OUT_DIR}/.launch_XXXXXX.sh")
    chmod +x "$tmpscript"

    # Variables expanded by local bash; \$! and \${pids[@]} evaluated remotely.
    cat > "$tmpscript" <<SCRIPT
#!/usr/bin/env bash
export ABT_THREAD_STACKSIZE=${ABT_THREAD_STACKSIZE}
pids=()
for (( p=1; p<=${n_procs}; p++ )); do
    "${client_bin}" "${server_addr}" \\
        --send-only --rpc "${rpc}" ${batch_arg} \\
        --count ${count} --size ${size} \\
        --stories ${stories} --reps 1 &
    pids+=(\$!)
done
failed=0
for pid in "\${pids[@]}"; do wait "\$pid" || failed=1; done
exit \$failed
SCRIPT

    ssh "$chost" "bash '${tmpscript}'"
    local rc=$?
    rm -f "$tmpscript"
    return $rc
}

# Run one (rpc_label, queue, threads, n_clients, stories) data point: REPS repetitions of
# reset → parallel-send → finalize, then emit a CSV row.
# rpc_label may be "batch_1000", "sendrecv", etc.  The bare rpc name (for --rpc flag)
# is derived by stripping the trailing _N suffix.  batch_size (10th arg) is forwarded
# to _launch_clients_on_node only for batch mode.  progress_threads (11th arg) is
# recorded in the CSV but handled by the server; clients don't need it.
_run_nc_point() {
    local rpc="$1" qm="$2" nt="$3" nc="$4" actual_stories="$5"
    local server_addr="$6" cur_proto="$7" count="$8" csv="$9"
    local batch_size="${10:-}"        # non-empty only when rpc label is "batch_N"
    local progress_threads="${11:-1}" # Margo progress ES count (default 1)

    # Bare rpc name for --rpc CLI flag: "batch_1000" → "batch", "sendrecv" → "sendrecv"
    local rpc_actual="${rpc%%_[0-9]*}"

    local ingest_ms_arr=()
    local merge_ms_arr=()
    local srv_threads=4

    for (( rep=1; rep<=REPS; rep++ )); do
        printf "         rep %d/%d: " "$rep" "$REPS"

        # Phase 1: reset
        ssh "$COORD_HOST" \
            "ABT_THREAD_STACKSIZE=$ABT_THREAD_STACKSIZE \
             '$CLIENT_BIN' '$server_addr' --reset-only"

        # Phase 2: parallel send
        local n_nodes=${#CLIENT_NODES[@]}
        local client_pids=()
        for (( ni=0; ni<n_nodes; ni++ )); do
            local base=$(( nc / n_nodes ))
            local extra=$(( ni < (nc % n_nodes) ? 1 : 0 ))
            local procs_per_node=$(( base + extra ))
            (( procs_per_node == 0 )) && continue
            printf "             %s: %d procs\n" "${CLIENT_NODES[$ni]}" "$procs_per_node"
            _launch_clients_on_node \
                "${CLIENT_NODES[$ni]}" "$procs_per_node" \
                "$CLIENT_BIN" "$server_addr" "$rpc_actual" \
                "$count" "$SIZE" "$actual_stories" "$batch_size" &
            client_pids+=($!)
        done
        local send_ok=1
        for pid in "${client_pids[@]}"; do wait "$pid" || send_ok=0; done
        if (( ! send_ok )); then
            echo "ERROR: one or more client nodes failed"; _kill_server; exit 1
        fi

        # Phase 3: finalize — use bare rpc name; pass --rpc so oneway can call wait_for
        local fin_result
        fin_result=$(ssh "$COORD_HOST" \
            "ABT_THREAD_STACKSIZE=$ABT_THREAD_STACKSIZE \
             '$CLIENT_BIN' '$server_addr' --finalize-only \
             --rpc '$rpc_actual' --n-clients $nc --count $count")

        IFS=',' read -r rep_ingest rep_merge srv_threads <<< "$fin_result"
        ingest_ms_arr+=("$rep_ingest")
        merge_ms_arr+=("$rep_merge")
        printf "ingest=%.3f ms  merge=%.3f ms\n" "$rep_ingest" "$rep_merge"
    done

    local ingest_csv merge_csv
    ingest_csv=$(printf '%s,' "${ingest_ms_arr[@]}"); ingest_csv="${ingest_csv%,}"
    merge_csv=$(printf '%s,'  "${merge_ms_arr[@]}");  merge_csv="${merge_csv%,}"

    local row
    row=$(python3 - <<PY
rpc_label        = "$rpc"
queue_label      = "$qm"
srv_threads      = $srv_threads
progress_threads = $progress_threads
n_clients        = $nc
stories          = $actual_stories
count            = $count

ingest_ms_list = [$ingest_csv]
merge_ms_list  = [$merge_csv]
ingest_ms   = sum(ingest_ms_list) / len(ingest_ms_list)
merge_ms    = sum(merge_ms_list)  / len(merge_ms_list)
pipeline_ms = ingest_ms + merge_ms
total_events = n_clients * count
mevs = lambda ms: (total_events / ms / 1000.0) if ms > 0 else 0.0
print(f"{rpc_label},{queue_label},{srv_threads},{progress_threads},{n_clients},{stories},"
      f"{total_events},{ingest_ms:.3f},{merge_ms:.3f},{pipeline_ms:.3f},"
      f"{mevs(ingest_ms):.3f},{mevs(pipeline_ms):.3f}")
PY
)
    echo "$row" >> "$csv"
    echo "$row" | awk -F',' \
        '{printf "         => ingest=%7s Mev/s  pipeline=%7s Mev/s\n", $11, $12}'
    echo
}

run_network() {
    local SERVER_BIN="$BUILD_DIR/chrono-ingestion-bench-server"
    local CLIENT_BIN="$BUILD_DIR/chrono-ingestion-bench-client"

    [[ -x "$SERVER_BIN" ]] || { echo "ERROR: $SERVER_BIN not found (build with Thallium)"; exit 1; }
    [[ -x "$CLIENT_BIN" ]] || { echo "ERROR: $CLIENT_BIN not found"; exit 1; }

    local CSV="$OUT_DIR/results_network.csv"
    local COORD_HOST="${CLIENT_NODES[0]}"

    # Write CSV header (progress_threads column added after threads)
    echo "rpc,queue,threads,progress_threads,n_clients,stories,total_events,ingest_ms,merge_ms,pipeline_ms,ingest_Mevs,pipeline_Mevs" > "$CSV"

    echo "NETWORK sweep:"
    echo "  server:        $SERVER_HOST  (IP: $SERVER_IP)"
    echo "  client nodes:  ${CLIENT_NODES[*]}"
    echo "  n_clients:     ${CLIENT_COUNTS[*]}"
    echo "  queues:        ${QUEUE_MODES[*]}"
    echo "  threads:       ${THREAD_COUNTS[*]}"
    echo "  progress_threads: ${PROGRESS_THREADS[*]}"
    echo "  rpcs:          ${RPC_MODES[*]}"
    echo "  stories:       ${STORIES_LIST[*]}  (\"match\" = stories equals n_clients)"
    echo "  stories=$( IFS=,; echo "${STORIES_LIST[*]}" )  count=$COUNT/client  size=${SIZE}B  reps=$REPS"
    echo

    # Total points: batch rpc multiplies by #BATCH_SIZES; others multiply by 1.
    local n_batch_variants=0
    for _r in "${RPC_MODES[@]}"; do
        [[ "$_r" == "batch" ]] && n_batch_variants=$(( n_batch_variants + ${#BATCH_SIZES[@]} )) \
                                || n_batch_variants=$(( n_batch_variants + 1 ))
    done
    local total_points=$(( ${#PROGRESS_THREADS[@]} * n_batch_variants \
                           * ${#QUEUE_MODES[@]} * ${#THREAD_COUNTS[@]} \
                           * ${#STORIES_LIST[@]} * ${#CLIENT_COUNTS[@]} ))
    local point=0

    for pt in "${PROGRESS_THREADS[@]}"; do
    for rpc_base in "${RPC_MODES[@]}"; do

        # For batch: iterate over BATCH_SIZES; label CSV rows "batch_N".
        # For all other modes: single iteration with rpc_label == rpc_base.
        local bsz_list=("")
        [[ "$rpc_base" == "batch" ]] && bsz_list=("${BATCH_SIZES[@]}")

        for bsz in "${bsz_list[@]}"; do
            local rpc_label="$rpc_base"
            [[ -n "$bsz" ]] && rpc_label="batch_${bsz}"

        for qm in "${QUEUE_MODES[@]}"; do
            for nt in "${THREAD_COUNTS[@]}"; do

                # Choose transport protocol for this rpc mode
                local cur_proto
                if [[ "$rpc_base" == "rdma" ]]; then
                    cur_proto="$PROTO_RDMA"
                else
                    cur_proto="$PROTO_SENDRECV"
                fi
                local server_addr="${cur_proto}://${SERVER_IP}:${PORT}"

                for stories_spec in "${STORIES_LIST[@]}"; do

                # "match": stories = n_clients — restart server for each nc.
                # fixed N: start server once, sweep all nc values.
                if [[ "$stories_spec" == "match" ]]; then

                    for nc in "${CLIENT_COUNTS[@]}"; do
                        local actual_stories=$nc
                        point=$(( point + 1 ))

                        printf "Starting server: queue=%-9s threads=%2d pt=%d stories=%2d proto=%s on %s\n" \
                               "$qm" "$nt" "$pt" "$actual_stories" "$cur_proto" "$SERVER_HOST"
                        _kill_server
                        ssh "$SERVER_HOST" \
                            "ABT_THREAD_STACKSIZE=$ABT_THREAD_STACKSIZE \
                             nohup '$SERVER_BIN' '$server_addr' \
                             --queue '$qm' --threads $nt --progress-threads $pt \
                             --stories $actual_stories \
                             > /tmp/keeper_server_${qm}_${nt}_pt${pt}_${rpc_base}.log 2>&1 &"
                        sleep "$SERVER_STARTUP_S"

                        printf "[%2d/%2d] rpc=%-12s queue=%-9s threads=%2d pt=%d stories=%2d n_clients=%d\n" \
                               "$point" "$total_points" "$rpc_label" "$qm" "$nt" "$pt" "$actual_stories" "$nc"

                        _run_nc_point "$rpc_label" "$qm" "$nt" "$nc" "$actual_stories" \
                                      "$server_addr" "$cur_proto" "$COUNT" "$CSV" "$bsz" "$pt"

                        _kill_server
                    done

                else
                    local actual_stories=$stories_spec

                    printf "Starting server: queue=%-9s threads=%2d pt=%d stories=%2d proto=%s on %s\n" \
                           "$qm" "$nt" "$pt" "$actual_stories" "$cur_proto" "$SERVER_HOST"
                    _kill_server
                    ssh "$SERVER_HOST" \
                        "ABT_THREAD_STACKSIZE=$ABT_THREAD_STACKSIZE \
                         nohup '$SERVER_BIN' '$server_addr' \
                         --queue '$qm' --threads $nt --progress-threads $pt \
                         --stories $actual_stories \
                         > /tmp/keeper_server_${qm}_${nt}_pt${pt}_${rpc_base}.log 2>&1 &"
                    sleep "$SERVER_STARTUP_S"

                    for nc in "${CLIENT_COUNTS[@]}"; do
                        point=$(( point + 1 ))
                        printf "[%2d/%2d] rpc=%-12s queue=%-9s threads=%2d pt=%d stories=%2d n_clients=%d\n" \
                               "$point" "$total_points" "$rpc_label" "$qm" "$nt" "$pt" "$actual_stories" "$nc"

                        _run_nc_point "$rpc_label" "$qm" "$nt" "$nc" "$actual_stories" \
                                      "$server_addr" "$cur_proto" "$COUNT" "$CSV" "$bsz" "$pt"
                    done

                    _kill_server
                fi

                done  # stories_spec
            done
        done

        done  # bsz
    done  # rpc_base
    done  # pt

    echo "CSV: $CSV"
    _print_network_table "$CSV"

    if (( DO_PLOT )); then
        python3 "$SCRIPT_DIR/plot_results.py" "$CSV" "$OUT_DIR" 2>/dev/null \
            && echo "Plots: $OUT_DIR/" \
            || echo "(network plot failed — check python3/matplotlib)"
    fi
}

# Print a summary table from the network CSV.
# CSV columns: rpc,queue,threads,n_clients,stories,total_events,
#              ingest_ms,merge_ms,pipeline_ms,ingest_Mevs,pipeline_Mevs
_print_network_table() {
    python3 - "$1" <<'PY'
import csv, sys
from collections import defaultdict

with open(sys.argv[1]) as f:
    rows = list(csv.DictReader(f))
if not rows:
    print("(empty CSV)")
    sys.exit(0)

n_clients_vals = sorted({int(r['n_clients']) for r in rows})
thread_vals    = sorted({int(r['threads'])   for r in rows})
rpc_vals       = sorted({r['rpc']            for r in rows})
queue_vals     = sorted({r['queue']          for r in rows})

# ---- throughput table: rows = (rpc+queue), cols = n_clients, sub-cols = threads ----
print("\nIngest throughput (Mev/s) — rpc × queue rows, n_clients × threads columns:")
pairs = [(rpc, q) for rpc in rpc_vals for q in queue_vals]
hdr = f"  {'rpc+queue':<22s}"
for nc in n_clients_vals:
    for t in thread_vals:
        hdr += f" | {nc}c/{t}t"
print(hdr)
print("  " + "-" * (len(hdr) - 2))

data = {}
for r in rows:
    key = (r['rpc'], r['queue'], int(r['n_clients']), int(r['threads']))
    data[key] = float(r['ingest_Mevs'])

for (rpc, q) in pairs:
    label = f"{rpc}+{q}"
    row_str = f"  {label:<22s}"
    for nc in n_clients_vals:
        for t in thread_vals:
            v = data.get((rpc, q, nc, t), float('nan'))
            row_str += f" | {v:6.2f}"
    print(row_str)

# ---- speedup summary (at median thread count) ----
print()
print("Speedup summary (median thread count):")
mid_t = thread_vals[len(thread_vals) // 2]
baseline = data.get(('sendrecv', 'mutex', 1, mid_t))

for nc in n_clients_vals:
    for rpc in rpc_vals:
        for q in queue_vals:
            v = data.get((rpc, q, nc, mid_t))
            if baseline and v:
                sp = v / baseline
                print(f"  {rpc:>9s}+{q:<9s}  n_clients={nc}  threads={mid_t}: "
                      f"{v:6.2f} Mev/s  ({sp:.2f}x vs sendrecv+mutex n=1)")
PY
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------
if (( NETWORK_MODE )); then
    trap '_kill_server' EXIT
    run_network
else
    run_local
fi
