#!/usr/bin/env bash
# ares_test.sh — orchestration driver for the ChronoLog performance regression
# test matrix described in test/performance/perf_test_plan.md.
#
# This script is written to be reviewed locally *before* it runs on an actual
# Slurm cluster. By default (DRY_RUN=1) it never executes any side-effecting
# command: every slurm invocation, every deploy_cluster.sh call, every
# pkill / ssh / hosts-file write, and every chrono-performance-test command is
# echoed verbatim to the log file instead.
#
# To actually run it on the cluster, export DRY_RUN=0 before invoking.
#
# Test matrix (from perf_test_plan.md):
#   * component scales : 1, 2, 4 nodes each of keeper/grapher/player
#                        (visor is always a single dedicated node)
#   * protocols        : ofi+sockets, ofi+verbs
#                        (verbs only affects KeeperRecordingService and
#                         KeeperGrapherDrainService per the plan)
#   * client configs   : #client_per_node x #nodes
#                        1x1, 10x1, 10x2, 20x1, 10x4, 20x2,
#                        40x1, 20x4, 40x2, 40x4
#   * tests            : connect/disconnect, acquire/release,
#                        recording bandwidth, replay bandwidth
#   * reps             : 3
#
# Total perf-test invocations: 3 * 2 * 10 * 4 * 3 = 720.

set -u -o pipefail

# ---------------------------------------------------------------------------
# Usage / help
# ---------------------------------------------------------------------------

usage() {
    cat <<'EOF'
USAGE
  ares_test.sh [OPTIONS]

DESCRIPTION
  Orchestration driver for the ChronoLog performance regression test matrix.
  Acquires a Slurm allocation, deploys ChronoLog at every (component scale ×
  protocol) combination, then runs chrono-performance-test across all client
  configs, event sizes, and repetitions, recording timing results to a CSV.

  By default the script operates in DRY_RUN=1 mode: every side-effecting
  command (salloc, deploy_cluster.sh, mpirun, pkill, ssh) is only echoed to
  the log file and never executed.  Set DRY_RUN=0 (or export it) to run for
  real on the cluster.

OPTIONS
  --dry-run        Echo every command to the log instead of executing it.
                   No Slurm allocation, no deployment, no perf runs.
                   Default: on (DRY_RUN=1).  Use --no-dry-run to run for real.
  --no-dry-run     Execute commands for real on the cluster.

  --write          Run write tests (connect_disconnect, acquire_release,
                   recording).  On by default.
  --no-write       Skip write tests.

  --read           Run read tests (replay).  Off by default.
  --no-read        Skip read tests (default).

  --colocate       Allow client processes to run on the same nodes as service
                   processes (keeper / grapher / player).  Reduces the Slurm
                   allocation by MAX_CLIENT_NODES nodes.
  --no-colocate    Keep client nodes fully dedicated (default).

  --debug          After writing each client hosts file, snapshot the contents
                   of CONF_DIR into the log directory for post-run inspection.
                   Off by default.

  -h, --help       Print this help message and exit.

ENVIRONMENT VARIABLES
  Any variable below can be set before invoking the script to override its
  default without modifying source code, e.g.:
    DRY_RUN=0 REPS=1 bash ares_test.sh --write

  Execution mode
    DRY_RUN=0|1           Echo-only mode (no real commands executed).
                          Default: 1 (dry-run).  Overridden by --dry-run /
                          --no-dry-run.
    DEBUG=0|1             Enable per-client-config conf/ snapshots.
                          Default: 0.  Overridden by --debug.
    RUN_WRITE=0|1         Run write tests.  Default: 1.
    RUN_READ=0|1          Run read tests.   Default: 0.
    COLOCATE=0|1          Allow client/service node overlap.  Default: 0.

  Paths  (all default under INSTALL_DIR)
    INSTALL_DIR           ChronoLog installation root.
                          Default: ~/chronolog-install/chronolog
    CONF_DIR              Server config and hosts-file directory.
                          Default: \$INSTALL_DIR/conf
    OUTPUT_DIR            HDF5 story data written by keepers/graphers/players.
                          Default: \$INSTALL_DIR/output
    PERF_BIN              chrono-performance-test binary.
                          Default: \$INSTALL_DIR/tests/chrono-performance-test
    MPIRUN                Full path to mpirun / mpiexec.
    DEPLOY_SCRIPT         Path to deploy_cluster.sh.
                          Default: \$INSTALL_DIR/tools/deploy_cluster.sh
    CONF_FILE             Server JSON config template.
                          Default: \$CONF_DIR/default-chrono-conf.json
    CLIENT_CONF_FILE      Client JSON config template.
                          Default: \$CONF_DIR/default-chrono-client-conf.json

  Test matrix
    COMPONENT_SCALES      Space-separated component scale values (number of
                          keeper / grapher / player nodes per scale point).
                          Default: "1 2 4"
    PROTOCOLS             Space-separated OFI transport strings.
                          Default: "ofi+sockets"
    WRITE_CLIENT_CONFIGS  Space-separated <procs_per_node>x<nodes> tokens for
                          write tests.
                          Default: "1x1 10x1 10x2 20x1 10x4 20x2 40x1 20x4 40x2 40x4"
    READ_CLIENT_CONFIGS   Space-separated <procs_per_node>x<nodes> tokens for
                          read tests (limited to 1 proc/node by the single-
                          player-per-node constraint).
                          Default: "1x1 1x2 1x4"
    REPS                  Repetitions per (test × client-config × event-size).
                          Default: 3
    PER_TEST_TIMEOUT_SEC  Kill a single perf-test run after this many seconds.
                          Default: 120
    MAX_RETRIES           Maximum redeploy+retry attempts when a component
                          error is detected in a run's output.  Default: 2.
    COMPONENT_ERROR_PATTERN
                          grep -E pattern applied to each run's stdout/stderr.
                          A match triggers redeploy and retry.
                          Default: "HG_NOENTRY|HG_TIMEOUT|HG_CANCELED|margo_forward failed"

  Slurm
    SLURM_PARTITION       Force a specific partition name.
                          Default: "" — try "compute" first; if it times out,
                          split between "debug" (idle nodes) and "compute".
    SALLOC_TIMEOUT_SEC    Seconds to wait for salloc before trying the split
                          fallback.  Default: 60.  Set to 0 to disable timeout.
    SLURM_TIME_LIMIT      Wall-time limit passed to salloc (HH:MM:SS).
                          Default: 04:00:00
    SLURM_JOB_NAME        Slurm job name.  Default: chronolog-perf

  Logging  (rarely needed; defaults are derived from the run timestamp)
    LOG_DIR               Directory for all output files.
                          Default: ./ares_test_logs_YYYYMMDD_HHMMSS
    LOG_FILE              Main timestamped execution log.
    RESULT_FILE           CSV results file (input to extract_plot_results.py).

OUTPUT
  All output is written to ares_test_logs_YYYYMMDD_HHMMSS/ in the working
  directory.  A symlink ares_test_logs_latest/ is kept pointing at the most
  recent run directory.

  Files inside the log directory:
    ares_test_<stamp>.log           Full timestamped execution log.
    ares_test_<stamp>.cmd.log       Clean list of every command executed /
                                    that would have been executed in dry-run.
    ares_test_<stamp>.conf.log      Snapshot of all resolved configuration
                                    values at script start.
    ares_test_<stamp>.results       CSV results consumed by
                                    extract_plot_results.py.
                                    Columns: test, protocol, scale, clients,
                                    event_size, event_count, rep, status,
                                    wall_time, tag
    ares_test_<stamp>.sh.snapshot   Verbatim copy of this script as it ran.
    conf_scale<S>_<proto>_clients<P>x<N>/
                                    Conf/ snapshots (only written with --debug).

EXAMPLES
  # Review all commands without running anything (default dry-run):
  bash ares_test.sh

  # Run the full write-test matrix for real:
  bash ares_test.sh --no-dry-run

  # Run write and read tests together for real:
  bash ares_test.sh --no-dry-run --write --read

  # Smoke test: one scale, one client config, one rep:
  COMPONENT_SCALES=1 WRITE_CLIENT_CONFIGS='1x1' REPS=1 bash ares_test.sh --no-dry-run

  # Save nodes by colocating clients with service processes:
  bash ares_test.sh --no-dry-run --colocate

  # Force the compute partition, capture conf snapshots for debugging:
  SLURM_PARTITION=compute bash ares_test.sh --no-dry-run --debug

  # Plot results from the most recent run:
  python3 extract_plot_results.py
EOF
}

# Check for --help / -h before any side-effecting global setup so that
# 'bash ares_test.sh --help' never creates a log directory.
for _arg in "$@"; do
    case "$_arg" in
        --help|-h) usage; exit 0 ;;
    esac
done
unset _arg

# ---------------------------------------------------------------------------
# Knobs
# ---------------------------------------------------------------------------

: "${DRY_RUN:=0}"                         # 1 = echo-only, 0 = actually run
: "${DEBUG:=0}"                           # 1 = snapshot conf/ dir per client config (set via --debug)
: "${RUN_WRITE:=1}"                       # 1 = run write tests (connect/disconnect, acquire/release, recording)
: "${RUN_READ:=0}"                        # 1 = run read tests (replay)
: "${COLOCATE:=0}"                        # 1 = client nodes may overlap with service nodes (keeper/grapher/player)
: "${INSTALL_DIR:=$HOME/chronolog-install/chronolog}"
: "${CONF_DIR:=$INSTALL_DIR/conf}"
: "${OUTPUT_DIR:=$INSTALL_DIR/output}"
: "${BIN_DIR:=$INSTALL_DIR/bin}"
: "${TOOLS_DIR:=$INSTALL_DIR/tools}"
: "${TESTS_DIR:=$INSTALL_DIR/tests}"
: "${PERF_BIN:=$TESTS_DIR/chrono-performance-test}"
: "${MPIRUN:=/mnt/common/kfeng/spack/opt/spack/linux-ubuntu22.04-skylake_avx512/gcc-11.4.0/mpich-4.0.2-yomnocixlvz4mtgvih66sj7bp4zetml7/bin/mpirun}"                    # full path if not in $PATH
: "${DEPLOY_SCRIPT:=$TOOLS_DIR/deploy_cluster.sh}"
: "${CONF_FILE:=$CONF_DIR/default-chrono-conf.json}"
: "${CLIENT_CONF_FILE:=$CONF_DIR/default-chrono-client-conf.json}"
: "${CLIENT_HOSTS:=$CONF_DIR/hosts_client}"      # not consumed by deploy script, used by mpirun
: "${VISOR_HOSTS:=$CONF_DIR/hosts_visor}"
: "${KEEPER_HOSTS:=$CONF_DIR/hosts_keeper}"
: "${GRAPHER_HOSTS:=$CONF_DIR/hosts_grapher}"
: "${PLAYER_HOSTS:=$CONF_DIR/hosts_player}"

: "${PER_TEST_TIMEOUT_SEC:=120}"          # kill any perf-test run that exceeds this
: "${REPS:=3}"
: "${MAX_RETRIES:=2}"                     # max redeploy+retry attempts on component failure per test run
# grep -E pattern applied to each run's stdout/stderr to detect component failure.
# A match triggers redeploy and retry (up to MAX_RETRIES times).
: "${COMPONENT_ERROR_PATTERN:=HG_NOENTRY|HG_TIMEOUT|HG_CANCELED|margo_forward failed}"
: "${SLURM_PARTITION:=}"                  # force a specific partition; empty = try compute, then debug,compute
: "${SALLOC_TIMEOUT_SEC:=60}"             # per-partition timeout for salloc; 0 = no timeout
: "${SLURM_TIME_LIMIT:=1-00:00:00}"
: "${SLURM_JOB_NAME:=chronolog-perf}"

# Holds the Slurm job ID(s) for the perf allocation (1 entry for a single
# allocation, 2 for a split debug+compute allocation).
PERF_JOB_IDS=()
# 1 = this script instance ran salloc and owns the allocation (scancel on exit).
# 0 = a pre-existing allocation was reused (do not scancel on exit).
PERF_JOB_OWNED=0

STAMP="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${LOG_DIR:-$(pwd)/ares_test_logs_${STAMP}}"
LOG_FILE="${LOG_FILE:-$LOG_DIR/ares_test_${STAMP}.log}"
# CMD_LOG_FILE holds a clean, reviewable list of every command the driver
# would execute (one per line, with section comments) — useful to skim the
# matrix without wading through the full timestamped log. Derived from
# LOG_FILE so the two files share a stamp.
CMD_LOG_FILE="${CMD_LOG_FILE:-${LOG_FILE%.log}.cmd.log}"
CONF_LOG_FILE="${CONF_LOG_FILE:-${LOG_FILE%.log}.conf.log}"
RESULT_FILE="${RESULT_FILE:-$LOG_DIR/ares_test_${STAMP}.results}"

LOG_DIR_PARENT="$(dirname "$LOG_DIR")"
LOG_DIR_BASE="$(basename "$LOG_DIR")"
LATEST_LINK="${LOG_DIR_PARENT}/ares_test_logs_latest"

mkdir -p "$LOG_DIR"
: > "$LOG_FILE"
: > "$CMD_LOG_FILE"
: > "$RESULT_FILE"

# Point the "latest" symlink at this run's log dir.
ln -sfn "$LOG_DIR_BASE" "$LATEST_LINK"

# Fixed component scales (symmetric: N keepers == N graphers == N players).
COMPONENT_SCALES=(1 2 4)
VISOR_NODES=1

PROTOCOLS=(ofi+sockets)

# Write-test client configs: space-separated "<procs_per_node>x<nodes>" tokens.
# Examples:
#   WRITE_CLIENT_CONFIGS='1x1'           bash ares_test.sh   # smoke test
#   WRITE_CLIENT_CONFIGS='1x1 10x1 10x2' bash ares_test.sh   # first three
#   bash ares_test.sh                                        # full matrix
: "${WRITE_CLIENT_CONFIGS:=1x1 10x1 10x2 20x1 10x4 20x2 40x1 20x4 40x2 40x4}"
read -ra WRITE_CLIENT_CONFIGS_ARR <<< "$WRITE_CLIENT_CONFIGS"

# Read-test client configs: limited to 1 client per node (single player process
# per node constraint).
: "${READ_CLIENT_CONFIGS:=1x1 1x2 1x4}"
read -ra READ_CLIENT_CONFIGS_ARR <<< "$READ_CLIENT_CONFIGS"

# Derive MAX_COMP_NODES as the largest value in COMPONENT_SCALES.
MAX_COMP_NODES=0
for _s in "${COMPONENT_SCALES[@]}"; do
    (( _s > MAX_COMP_NODES )) && MAX_COMP_NODES=$_s
done
unset _s

# Derive MAX_CLIENT_NODES as the largest node count across both client config arrays.
MAX_CLIENT_NODES=0
for _c in "${WRITE_CLIENT_CONFIGS_ARR[@]}" "${READ_CLIENT_CONFIGS_ARR[@]}"; do
    _n="${_c##*x}"
    (( _n > MAX_CLIENT_NODES )) && MAX_CLIENT_NODES=$_n
done
unset _c _n

# Total allocation:
#   COLOCATE=0 (default): clients get dedicated nodes -> 1 visor + 3*MAX_COMP_NODES + MAX_CLIENT_NODES
#   COLOCATE=1          : clients share the player slot pool -> 1 visor + 3*MAX_COMP_NODES
if (( COLOCATE )); then
    TOTAL_NODES=$(( VISOR_NODES + MAX_COMP_NODES * 3 ))
else
    TOTAL_NODES=$(( VISOR_NODES + MAX_COMP_NODES * 3 + MAX_CLIENT_NODES ))
fi

# Write tests run over the full CLIENT_CONFIGS matrix.
# Read tests (replay) run only with 1 client per node.
WRITE_TESTS=(
    "connect_disconnect"
    "acquire_release"
    "recording"
)
READ_TESTS=(
    "replay"
)

# Per-test shared workload settings. Event size is fixed (min==ave==max) so
# the recording/replay bandwidth numbers are not distorted by a payload-size
# distribution; see common_perf_flags for how this is wired into -a/-s/-b.
#
# EVENT_SIZES covers the full payload spectrum from small messages to large
# blocks. event_count_for_size() maps each size to an event count so the
# total data written stays bounded at ~19 GB for the largest client scale
# (40x4 = 160 clients).
EVENT_SIZES=(1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576)
EVENT_INTERVAL_US=0
CHRONICLE_COUNT=1

# ---------------------------------------------------------------------------
# Snapshots — freeze the script source and resolved config into the log dir
# so every run is fully reproducible even if the script or env changes later.
# ---------------------------------------------------------------------------

# Snapshot the script itself.
cp -- "${BASH_SOURCE[0]}" "$LOG_DIR/ares_test_${STAMP}.sh.snapshot"

# Snapshot all configuration scalars and arrays into a separate conf log.
{
    printf '# ares_test.sh configuration snapshot — %s\n\n' "$STAMP"
    printf '# --- Paths ---\n'
    printf 'INSTALL_DIR=%s\n'      "$INSTALL_DIR"
    printf 'CONF_DIR=%s\n'         "$CONF_DIR"
    printf 'OUTPUT_DIR=%s\n'       "$OUTPUT_DIR"
    printf 'BIN_DIR=%s\n'          "$BIN_DIR"
    printf 'TOOLS_DIR=%s\n'        "$TOOLS_DIR"
    printf 'TESTS_DIR=%s\n'        "$TESTS_DIR"
    printf 'PERF_BIN=%s\n'         "$PERF_BIN"
    printf 'MPIRUN=%s\n'           "$MPIRUN"
    printf 'DEPLOY_SCRIPT=%s\n'    "$DEPLOY_SCRIPT"
    printf 'CONF_FILE=%s\n'        "$CONF_FILE"
    printf 'CLIENT_CONF_FILE=%s\n' "$CLIENT_CONF_FILE"
    printf 'CLIENT_HOSTS=%s\n'     "$CLIENT_HOSTS"
    printf 'VISOR_HOSTS=%s\n'      "$VISOR_HOSTS"
    printf 'KEEPER_HOSTS=%s\n'     "$KEEPER_HOSTS"
    printf 'GRAPHER_HOSTS=%s\n'    "$GRAPHER_HOSTS"
    printf 'PLAYER_HOSTS=%s\n'     "$PLAYER_HOSTS"
    printf '\n# --- Runtime knobs ---\n'
    printf 'DRY_RUN=%s\n'          "$DRY_RUN"
    printf 'COLOCATE=%s\n'         "$COLOCATE"
    printf 'PER_TEST_TIMEOUT_SEC=%s\n' "$PER_TEST_TIMEOUT_SEC"
    printf 'REPS=%s\n'             "$REPS"
    printf 'MAX_RETRIES=%s\n'      "$MAX_RETRIES"
    printf 'COMPONENT_ERROR_PATTERN=%s\n' "$COMPONENT_ERROR_PATTERN"
    printf 'SLURM_PARTITION=%s\n'  "$SLURM_PARTITION"
    printf 'SLURM_TIME_LIMIT=%s\n' "$SLURM_TIME_LIMIT"
    printf 'SLURM_JOB_NAME=%s\n'   "$SLURM_JOB_NAME"
    printf '\n# --- Node layout ---\n'
    printf 'VISOR_NODES=%s\n'      "$VISOR_NODES"
    printf 'MAX_COMP_NODES=%s\n'   "$MAX_COMP_NODES"
    printf 'MAX_CLIENT_NODES=%s\n' "$MAX_CLIENT_NODES"
    printf 'TOTAL_NODES=%s\n'      "$TOTAL_NODES"
    printf '\n# --- Workload ---\n'
    printf 'EVENT_SIZES=(%s)\n'       "${EVENT_SIZES[*]}"
    printf 'EVENT_INTERVAL_US=%s\n'   "$EVENT_INTERVAL_US"
    printf 'CHRONICLE_COUNT=%s\n'     "$CHRONICLE_COUNT"
    printf '\n# --- Arrays ---\n'
    printf 'COMPONENT_SCALES=(%s)\n'  "${COMPONENT_SCALES[*]}"
    printf 'PROTOCOLS=(%s)\n'         "${PROTOCOLS[*]}"
    printf 'WRITE_CLIENT_CONFIGS_ARR=(%s)\n' "${WRITE_CLIENT_CONFIGS_ARR[*]}"
    printf 'READ_CLIENT_CONFIGS_ARR=(%s)\n'  "${READ_CLIENT_CONFIGS_ARR[*]}"
    printf 'WRITE_TESTS=(%s)\n'             "${WRITE_TESTS[*]}"
    printf 'READ_TESTS=(%s)\n'              "${READ_TESTS[*]}"
    printf '\n# --- Log files ---\n'
    printf 'LOG_DIR=%s\n'     "$LOG_DIR"
    printf 'LOG_FILE=%s\n'    "$LOG_FILE"
    printf 'CMD_LOG_FILE=%s\n' "$CMD_LOG_FILE"
    printf 'CONF_LOG_FILE=%s\n' "$CONF_LOG_FILE"
    printf 'RESULT_FILE=%s\n' "$RESULT_FILE"
} > "$CONF_LOG_FILE"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------

log() {
    # Everything the operator should see lands in the log file; echoed to
    # stdout as well so dry-run review is convenient.
    local line="[$(date '+%F %T')] $*"
    printf '%s\n' "$line" | tee -a "$LOG_FILE"
}

# log_cmd: record a command line in both the main log (under the running
# prefix) and the standalone $CMD_LOG_FILE (one bare line, no timestamp).
# The cmd log is intended to be skimmed manually, so it stores commands
# verbatim with no decoration.
log_cmd() {
    local cmd="$*"
    log "    \$ $cmd"
    printf '%s\n' "$cmd" >> "$CMD_LOG_FILE"
}

section() {
    log ""
    log "==================== $* ===================="
    # Mirror sections into the cmd log as bash comments so the file reads
    # top-to-bottom as a commented action list.
    printf '\n# ==================== %s ====================\n' "$*" >> "$CMD_LOG_FILE"
}

# run_cmd: run a shell command, or (in dry-run) echo it to the log. Returns
# the real exit status in live mode; always 0 in dry-run.
run_cmd() {
    local desc="$1"; shift
    log "[CMD] $desc"
    log_cmd "$*"
    if [[ "$DRY_RUN" == "1" ]]; then
        return 0
    fi
    "$@"
}

# run_shell: same as run_cmd but for a full shell-expanded string (needed for
# things that use redirection, globbing, pipes, etc.).
run_shell() {
    local desc="$1"; shift
    local cmd="$*"
    log "[SH]  $desc"
    log_cmd "$cmd"
    if [[ "$DRY_RUN" == "1" ]]; then
        return 0
    fi
    bash -c "$cmd"
}

# write_file: create (or overwrite) a file with the given content. In dry-run
# mode the intended content is echoed to the log instead.
write_file() {
    local path="$1"; shift
    local content="$*"
    log "[WRITE] $path"
    while IFS= read -r line; do
        log "    | $line"
    done <<< "$content"
    if [[ "$DRY_RUN" == "1" ]]; then
        return 0
    fi
    mkdir -p "$(dirname "$path")"
    printf '%s\n' "$content" > "$path"
}

record_result() {
    # Append a single CSV-ish line to the result file.
    local line="$*"
    printf '%s\n' "$line" | tee -a "$RESULT_FILE" >> "$LOG_FILE"
}

# ---------------------------------------------------------------------------
# Slurm allocation / re-exec
# ---------------------------------------------------------------------------

allocate_if_needed() {
    log "[SLURM] checking for existing running allocation(s) under $USER (need $TOTAL_NODES nodes)"
    log_cmd "squeue -u \$USER -h -t RUNNING -o '%i %D'"

    if [[ "$DRY_RUN" == "1" ]]; then
        PERF_JOB_IDS=("dry-run-jobid")
        PERF_JOB_OWNED=0
        log "DRY_RUN=1, skipping real squeue — faking PERF_JOB_IDS=${PERF_JOB_IDS[*]}"
        return 0
    fi

    # Collect all running jobs and sum their node counts. If the total meets
    # or exceeds TOTAL_NODES, reuse those jobs and skip salloc and scancel.
    local -a existing_jids=()
    local total_existing=0
    local _jid _n
    while read -r _jid _n; do
        existing_jids+=("$_jid")
        (( total_existing += _n ))
    done < <(squeue -u "$USER" -h -t RUNNING -o '%i %D')

    if (( total_existing >= TOTAL_NODES )); then
        PERF_JOB_IDS=("${existing_jids[@]}")
        PERF_JOB_OWNED=0
        log "[SLURM] found ${#PERF_JOB_IDS[@]} existing job(s) totalling $total_existing node(s) — reusing, will not scancel"
        return 0
    fi

    log "[SLURM] existing allocation insufficient ($total_existing node(s)) — requesting new allocation"

    # Base salloc args; -N is supplied per-call to support split allocations.
    local salloc_args=(
        -J "$SLURM_JOB_NAME"
        -t "$SLURM_TIME_LIMIT"
        --exclusive
        --no-shell
    )

    # Build the timeout prefix (empty when SALLOC_TIMEOUT_SEC=0).
    local timeout_prefix=()
    if (( SALLOC_TIMEOUT_SEC > 0 )); then
        timeout_prefix=(timeout --kill-after=5 "$SALLOC_TIMEOUT_SEC")
    fi

    # _run_salloc <n_nodes> <partition>
    # Submit salloc for n_nodes from the given partition; return its exit code.
    # On timeout (rc=124) cancels any PENDING request left behind.
    _run_salloc() {
        local n="$1" part="$2"
        log "[SLURM] requesting $n node(s) from '$part' (timeout=${SALLOC_TIMEOUT_SEC}s)"
        log_cmd "${timeout_prefix[*]} salloc ${salloc_args[*]} -N $n -p $part"
        "${timeout_prefix[@]}" salloc "${salloc_args[@]}" -N "$n" -p "$part"
        local rc=$?
        if [[ $rc -eq 124 ]]; then
            log "[SLURM] salloc timed out on '$part' — cancelling any pending request"
            scancel -u "$USER" -n "$SLURM_JOB_NAME" --state=PENDING 2>/dev/null || true
        elif [[ $rc -ne 0 ]]; then
            log "[SLURM] salloc failed on '$part' (rc=$rc)"
        fi
        return $rc
    }

    # _wait_for_jobs <expected>
    # Poll squeue until <expected> jobs named SLURM_JOB_NAME appear and
    # populate PERF_JOB_IDS.
    _wait_for_jobs() {
        local expected="$1"
        log "[SLURM] waiting for $expected job(s) named '$SLURM_JOB_NAME' to appear in squeue..."
        local wait_sec=0
        while true; do
            mapfile -t PERF_JOB_IDS < <(squeue -u "$USER" -n "$SLURM_JOB_NAME" -h -o '%i')
            if (( ${#PERF_JOB_IDS[@]} >= expected )); then
                log "[SLURM] job(s) appeared: ${PERF_JOB_IDS[*]} (waited ${wait_sec}s)"
                return 0
            fi
            if (( wait_sec >= 120 )); then
                log "ERROR: expected $expected job(s) did not appear after ${wait_sec}s — aborting"
                exit 1
            fi
            sleep 5
            (( wait_sec += 5 ))
        done
    }

    # We are about to run salloc — mark the allocation as owned so cleanup
    # cancels it on exit.
    PERF_JOB_OWNED=1

    if [[ -n "$SLURM_PARTITION" ]]; then
        # Caller forced a specific partition — use it with no fallback.
        _run_salloc "$TOTAL_NODES" "$SLURM_PARTITION" \
            || { unset -f _run_salloc _wait_for_jobs; exit 1; }
        _wait_for_jobs 1
        unset -f _run_salloc _wait_for_jobs
        return 0
    fi

    # 1st attempt: all nodes from compute.
    _run_salloc "$TOTAL_NODES" "compute"
    local rc=$?
    if [[ $rc -eq 0 ]]; then
        _wait_for_jobs 1
        unset -f _run_salloc _wait_for_jobs
        return 0
    fi
    if [[ $rc -ne 124 ]]; then
        # Hard failure (not a timeout) — splitting won't help.
        unset -f _run_salloc _wait_for_jobs
        exit 1
    fi

    # compute timed out: not enough nodes available there. Split the allocation:
    # take whatever debug has idle, fill the remainder from compute.
    local debug_avail
    debug_avail=$(sinfo -p debug -h --states=idle -o '%D' 2>/dev/null \
                  | awk '{s+=$1} END{print s+0}')
    log "[SLURM] compute timed out; debug has $debug_avail idle node(s), need $TOTAL_NODES total"

    if (( debug_avail == 0 )); then
        log "[SLURM] no idle nodes in debug partition — cannot allocate"
        unset -f _run_salloc _wait_for_jobs
        exit 1
    fi

    local debug_n=$(( debug_avail < TOTAL_NODES ? debug_avail : TOTAL_NODES ))
    local compute_n=$(( TOTAL_NODES - debug_n ))
    local expected_jobs=$(( compute_n > 0 ? 2 : 1 ))
    log "[SLURM] split: $debug_n node(s) from debug, $compute_n node(s) from compute"

    _run_salloc "$debug_n" "debug"
    if [[ $? -ne 0 ]]; then
        unset -f _run_salloc _wait_for_jobs
        exit 1
    fi

    if (( compute_n > 0 )); then
        _run_salloc "$compute_n" "compute"
        if [[ $? -ne 0 ]]; then
            log "[SLURM] compute allocation failed; cancelling debug job"
            scancel -u "$USER" -n "$SLURM_JOB_NAME" 2>/dev/null || true
            unset -f _run_salloc _wait_for_jobs
            exit 1
        fi
    fi

    _wait_for_jobs "$expected_jobs"
    unset -f _run_salloc _wait_for_jobs
}

# Populate ALLOCATED_NODES by querying squeue for the nodelist of every job
# in PERF_JOB_IDS (one entry for a single allocation, two for a split).
# In dry-run we fabricate fake names so the rest of the script has
# deterministic output to review.
discover_nodes() {
    if [[ "$DRY_RUN" == "1" ]]; then
        ALLOCATED_NODES=()
        for ((i=1; i<=TOTAL_NODES; i++)); do
            ALLOCATED_NODES+=("dry-node$(printf '%02d' "$i")")
        done
        log "[SLURM] discovering nodes (dry-run, PERF_JOB_IDS=${PERF_JOB_IDS[*]})"
        log_cmd "squeue -j <jobid> -h -o '%N'  # repeated for each job in PERF_JOB_IDS"
        log_cmd "scontrol show hostnames <nodelist>"
        log "        -> ${ALLOCATED_NODES[*]}"
        return 0
    fi

    ALLOCATED_NODES=()
    local jid
    for jid in "${PERF_JOB_IDS[@]}"; do
        log "[SLURM] discovering nodes for job $jid"
        log_cmd "squeue -j $jid -h -o '%N'"
        local nodelist
        nodelist=$(squeue -j "$jid" -h -o '%N')
        if [[ -z "$nodelist" ]]; then
            log "ERROR: squeue returned empty nodelist for job $jid — is the job still running?"
            exit 1
        fi
        log "        compact nodelist: $nodelist"
        log_cmd "scontrol show hostnames $nodelist"
        local nodes=()
        mapfile -t nodes < <(scontrol show hostnames "$nodelist")
        log "        -> ${nodes[*]}"
        ALLOCATED_NODES+=("${nodes[@]}")
    done

    if (( ${#ALLOCATED_NODES[@]} < TOTAL_NODES )); then
        log "ERROR: need $TOTAL_NODES nodes, got ${#ALLOCATED_NODES[@]}"
        exit 1
    fi
}

# Slice ALLOCATED_NODES into role groups for a given component scale.
# Visor is always a dedicated node; keeper/grapher/player slot pools never
# overlap with each other.  The client pool placement depends on COLOCATE:
#
# COLOCATE=0 (default) — dedicated client nodes:
#   Layout (TOTAL_NODES = VISOR_NODES + MAX_COMP_NODES*3 + MAX_CLIENT_NODES):
#   [0]                                                    -> visor
#   [1 .. 1+MAX_COMP_NODES)                               -> keeper slot pool
#   [1+MAX_COMP_NODES .. 1+2*MAX_COMP_NODES)              -> grapher slot pool
#   [1+2*MAX_COMP_NODES .. 1+3*MAX_COMP_NODES)            -> player slot pool
#   [1+3*MAX_COMP_NODES .. 1+3*MAX_COMP_NODES+MAX_CLIENT_NODES) -> client pool (dedicated)
#
# COLOCATE=1 — clients share the player slot pool:
#   Layout (TOTAL_NODES = VISOR_NODES + MAX_COMP_NODES*3):
#   [0]                                                    -> visor
#   [1 .. 1+MAX_COMP_NODES)                               -> keeper slot pool
#   [1+MAX_COMP_NODES .. 1+2*MAX_COMP_NODES)              -> grapher slot pool
#   [1+2*MAX_COMP_NODES .. 1+3*MAX_COMP_NODES)            -> player slot pool
#   client pool == first MAX_CLIENT_NODES nodes of the player slot pool
#
# In both modes the active keeper/grapher/player nodelists take only the first
# `scale` nodes from their respective slot pools; unused slots stay idle.
partition_nodes() {
    local scale="$1"

    VISOR_NODELIST=("${ALLOCATED_NODES[0]}")

    local off=1
    KEEPER_NODELIST=("${ALLOCATED_NODES[@]:$off:$scale}")

    off=$(( 1 + MAX_COMP_NODES ))
    GRAPHER_NODELIST=("${ALLOCATED_NODES[@]:$off:$scale}")

    off=$(( 1 + 2 * MAX_COMP_NODES ))
    PLAYER_NODELIST=("${ALLOCATED_NODES[@]:$off:$scale}")

    local client_note
    if (( COLOCATE )); then
        # Client pool overlaps with the player slot pool (all MAX_CLIENT_NODES
        # slots, not just the ones running a player at the current scale).
        CLIENT_POOL=("${ALLOCATED_NODES[@]:$off:$MAX_CLIENT_NODES}")
        client_note="colocated with player slot pool"
    else
        # Client pool starts immediately after the player slot pool — fully
        # dedicated nodes that never run a service process.
        off=$(( 1 + 3 * MAX_COMP_NODES ))
        CLIENT_POOL=("${ALLOCATED_NODES[@]:$off:$MAX_CLIENT_NODES}")
        client_note="dedicated (no service overlap)"
    fi

    log "Node partition for scale=$scale (COLOCATE=$COLOCATE):"
    log "    visor   : ${VISOR_NODELIST[*]}"
    log "    keepers : ${KEEPER_NODELIST[*]}"
    log "    graphers: ${GRAPHER_NODELIST[*]}"
    log "    players : ${PLAYER_NODELIST[*]}"
    log "    clients : ${CLIENT_POOL[*]} ($client_note)"
}

# ---------------------------------------------------------------------------
# Hosts files
# ---------------------------------------------------------------------------

write_hosts_files() {
    write_file "$VISOR_HOSTS"   "$(printf '%s\n' "${VISOR_NODELIST[@]}")"
    write_file "$KEEPER_HOSTS"  "$(printf '%s\n' "${KEEPER_NODELIST[@]}")"
    write_file "$GRAPHER_HOSTS" "$(printf '%s\n' "${GRAPHER_NODELIST[@]}")"
    write_file "$PLAYER_HOSTS"  "$(printf '%s\n' "${PLAYER_NODELIST[@]}")"
}

write_client_hosts_file() {
    # $1 = #client_per_node, $2 = #nodes
    # Each hostname is suffixed with -40g to route client MPI traffic over the
    # 40 GbE interface rather than the management network.
    local per_node="$1"
    local nnodes="$2"
    if (( nnodes > MAX_CLIENT_NODES )); then
        log "ERROR: requested $nnodes client nodes > pool $MAX_CLIENT_NODES"
        return 1
    fi
    local subset=("${CLIENT_POOL[@]:0:$nnodes}")
    write_file "$CLIENT_HOSTS" "$(printf '%s-40g\n' "${subset[@]}")"
}

# ---------------------------------------------------------------------------
# Conf dir hygiene
# ---------------------------------------------------------------------------

# clean_conf_dir: remove all files that the script or deploy_cluster.sh
# generates in $CONF_DIR so the next deployment starts from a known-clean
# state. Keeps the two template JSON files intact.
#
# Files removed:
#   hosts_visor, hosts_keeper, hosts_grapher, hosts_player, hosts_client
#     — written by write_hosts_files / write_client_hosts_file
#   hosts_keeper.N, hosts_grapher.N, hosts_player.N
#     — split per-recording-group hosts files written by deploy_cluster.sh
#   default-chrono-conf.json.N
#     — per-recording-group conf files written by deploy_cluster.sh
clean_conf_dir() {
    log "[CONF] cleaning generated files from $CONF_DIR"
    local targets=(
        "$CONF_DIR/hosts_visor"
        "$CONF_DIR/hosts_keeper"
        "$CONF_DIR/hosts_grapher"
        "$CONF_DIR/hosts_player"
        "$CONF_DIR/hosts_client"
    )
    log_cmd "rm -f ${targets[*]} $CONF_DIR/hosts_keeper.* $CONF_DIR/hosts_grapher.* $CONF_DIR/hosts_player.* $CONF_DIR/default-chrono-conf.json.*"
    if [[ "$DRY_RUN" == "1" ]]; then
        return 0
    fi
    rm -f "${targets[@]}"
    rm -f "$CONF_DIR"/hosts_keeper.* "$CONF_DIR"/hosts_grapher.* "$CONF_DIR"/hosts_player.*
    rm -f "$CONF_DIR"/default-chrono-conf.json.*
}

# snapshot_conf_dir <scale> <proto> <per_node> <nnodes>
#
# Copy $CONF_DIR into a flat subdir of $LOG_DIR named after all the parameters
# that affect the chrono-performance-test command line for this client config.
# Called once per (scale, proto, per_node, nnodes) combination, immediately
# after write_client_hosts_file so the snapshot captures hosts_client too.
#
# Dir name pattern:
#   conf_scale<S>_<proto>_clients<P>x<N>
# e.g.: conf_scale2_ofi_verbs_clients10x2
# (esz/ecnt are not included because they vary per test invocation and the
# conf dir captures deployment config, not client workload parameters)
snapshot_conf_dir() {
    local scale="$1" proto="$2" per_node="$3" nnodes="$4"
    local proto_tag="${proto//+/_}"
    local snap_dir="$LOG_DIR/conf_scale${scale}_${proto_tag}_clients${per_node}x${nnodes}"
    log "[CONF] snapshotting $CONF_DIR → $snap_dir"
    log_cmd "cp -r '$CONF_DIR' '$snap_dir'"
    if [[ "$DRY_RUN" == "1" ]]; then
        return 0
    fi
    cp -r "$CONF_DIR" "$snap_dir"
}

# ---------------------------------------------------------------------------
# Deployment lifecycle
# ---------------------------------------------------------------------------

pkill_chrono_everywhere() {
    section "pkill -9 chrono on all component nodes"
    # mpssh -f <host_file> "<remote cmd>" fans out the kill in parallel. We
    # target one component type per hosts file so the visor hosts file is
    # never touched by keeper/grapher/player patterns and vice versa.
    # A pkill failure (no match, unreachable node, ssh hiccup) is logged as
    # a warning but never aborts the run — worst case the next deploy starts
    # on top of a stale process and we'll see it in verify_components_running.
    local role file
    for role in visor keeper grapher player; do
        case "$role" in
            visor)   file="$VISOR_HOSTS"   ;;
            keeper)  file="$KEEPER_HOSTS"  ;;
            grapher) file="$GRAPHER_HOSTS" ;;
            player)  file="$PLAYER_HOSTS"  ;;
        esac
        if ! run_shell "mpssh pkill chrono-${role}" \
                "mpssh -f '$file' \"pkill -9 -f chrono-${role} || true\""; then
            log "WARNING: pkill chrono-${role} via mpssh returned non-zero — continuing"
        fi
    done
    return 0
}

clean_output_dir() {
    section "clean $OUTPUT_DIR on all component nodes"
    # HDF5 story files accumulate quickly during a performance matrix run
    # (25 GB+ after a full run). Wipe the output directory on every node that
    # hosts a component before each new deployment so storage does not overflow.
    local file
    for file in "$VISOR_HOSTS" "$KEEPER_HOSTS" "$GRAPHER_HOSTS" "$PLAYER_HOSTS"; do
        if [[ ! -f "$file" ]]; then
            log "WARNING: hosts file $file not found — skipping output clean for it"
            continue
        fi
        if ! run_shell "mpssh rm -f output/* on $(basename "$file")" \
                "mpssh -f '$file' \"rm -f '${OUTPUT_DIR}/'*\""; then
            log "WARNING: mpssh clean_output_dir for $file returned non-zero — continuing"
        fi
    done
    return 0
}

set_protocol_in_conf() {
    # Only three rpc blocks switch protocol for the ofi+verbs sweep:
    #   chrono_keeper.KeeperRecordingService.rpc.protocol_conf
    #   chrono_keeper.KeeperGrapherDrainService.rpc.protocol_conf
    #   chrono_grapher.KeeperGrapherDrainService.rpc.protocol_conf
    # Every other protocol_conf field in the conf file stays at ofi+sockets
    # for the duration of the matrix (verbs is not supported there).
    local proto="$1"
    local desc="set protocol to $proto for KeeperRecording/KeeperGrapherDrain RPC"
    local jq_prog='
      .chrono_keeper.KeeperRecordingService.rpc.protocol_conf = $p
      | .chrono_keeper.KeeperGrapherDrainService.rpc.protocol_conf = $p
      | .chrono_grapher.KeeperGrapherDrainService.rpc.protocol_conf = $p
    '
    # Single-line variant for the cmd log — easier to copy/paste.
    local jq_prog_oneline='.chrono_keeper.KeeperRecordingService.rpc.protocol_conf = $p | .chrono_keeper.KeeperGrapherDrainService.rpc.protocol_conf = $p | .chrono_grapher.KeeperGrapherDrainService.rpc.protocol_conf = $p'
    log "[CONF] $desc"
    log_cmd "tmp=\$(mktemp) && jq --arg p '$proto' '$jq_prog_oneline' '$CONF_FILE' > \"\$tmp\" && mv \"\$tmp\" '$CONF_FILE'"
    if [[ "$DRY_RUN" == "1" ]]; then
        return 0
    fi
    local tmp; tmp="$(mktemp)"
    jq --arg p "$proto" "$jq_prog" "$CONF_FILE" > "$tmp" && mv "$tmp" "$CONF_FILE"
}

deploy_cluster() {
    # $1 = component scale (== NUM_RECORDING_GROUP, since we want one grapher
    # and one player per group and keepers split evenly across groups).
    local scale="$1"
    section "deploy_cluster.sh -d (recording_groups=$scale)"
    run_cmd "start cluster" "$DEPLOY_SCRIPT" -d \
        -r "$scale" \
        -q "$VISOR_HOSTS" \
        -o "$KEEPER_HOSTS" \
        -k "$GRAPHER_HOSTS" \
        -f "$CONF_FILE" \
        -n "$CLIENT_CONF_FILE"
}

stop_cluster() {
    section "deploy_cluster.sh -s"
    run_cmd "stop cluster" "$DEPLOY_SCRIPT" -s \
        -q "$VISOR_HOSTS" \
        -o "$KEEPER_HOSTS" \
        -k "$GRAPHER_HOSTS" \
        -f "$CONF_FILE" \
        -n "$CLIENT_CONF_FILE" || true
}

verify_components_running() {
    section "verify all four component processes are up"
    local failed=0

    # _check fans out a pgrep to every host in the given hosts file via mpssh.
    # The remote snippet prints "<host>:<role>:UP" or "<host>:<role>:DOWN" so a
    # single grep tells us if any host is missing its component.
    _check() {
        local role="$1"
        local pattern="$2"
        local hosts_file="$3"
        local remote="h=\$(hostname -s); pgrep -af ${pattern} >/dev/null && echo \$h:${role}:UP || echo \$h:${role}:DOWN"
        log "[VERIFY] checking ${role} via mpssh -f ${hosts_file}"
        log_cmd "mpssh -f $hosts_file \"$remote\""
        if [[ "$DRY_RUN" == "1" ]]; then
            return 0
        fi
        local out
        if ! out=$(mpssh -f "$hosts_file" "$remote" 2>&1); then
            log "    mpssh error: $out"
            failed=1
            return 0
        fi
        log "$out"
        if grep -q ':DOWN' <<< "$out"; then
            failed=1
        fi
    }

    local role hfile pattern
    for role in visor keeper grapher player; do
        case "$role" in
            visor)   hfile="$VISOR_HOSTS";   pattern="chrono-visor"   ;;
            keeper)  hfile="$KEEPER_HOSTS";  pattern="chrono-keeper"  ;;
            grapher) hfile="$GRAPHER_HOSTS"; pattern="chrono-grapher" ;;
            player)  hfile="$PLAYER_HOSTS";  pattern="chrono-player"  ;;
        esac
        if [[ ! -f "$hfile" && "$DRY_RUN" != "1" ]]; then
            log "    ERROR: $hfile does not exist — cannot verify $role"
            failed=1
            continue
        fi
        _check "$role" "$pattern" "$hfile"
    done

    if (( failed != 0 )); then
        log "ERROR: one or more components failed to start"
        return 1
    fi
    log "all four components are running"
    return 0
}

# ---------------------------------------------------------------------------
# Per-test flag assembly
# ---------------------------------------------------------------------------

# Map event size (bytes) and component scale to an event count.
#
# Base counts are calibrated for the maximum component scale (MAX_COMP_NODES)
# so total data written stays bounded at ~19 GB for 40x4 = 160 clients at
# the largest scale.  Smaller scales receive proportionally fewer events
# (scale/MAX_COMP_NODES) so they do not become the bottleneck and the test
# completes in a comparable wall time regardless of scale.
#
# Base tiers (at scale == MAX_COMP_NODES):
#   <=   8192 → 2000   (160 * 2000 *   8192 B ≈  2.6 GB)
#   <= 131072 → 1000   (160 * 1000 * 131072 B ≈ 21.0 GB)
#   <= 262144 →  500   (160 *  500 * 262144 B ≈ 21.0 GB)
#   <= 524288 →  250   (160 *  250 * 524288 B ≈ 21.0 GB)
#       else  →  125   (160 *  125 * 1048576 B ≈ 21.0 GB)
#
# At scale S the effective count = base * S / MAX_COMP_NODES (minimum 1).
event_count_for_size() {
    local esz="$1"
    local scale="$2"
    local base
    if   (( esz <=   8192 )); then base=2000
    elif (( esz <= 131072 )); then base=1000
    elif (( esz <= 262144 )); then base=500
    elif (( esz <= 524288 )); then base=250
    else                           base=125
    fi
    local ecnt=$(( base * scale / MAX_COMP_NODES ))
    (( ecnt < 1 )) && ecnt=1
    echo "$ecnt"
}

# Shared flags for every perf run. -y enables barriers, -p enables perf
# reporting, -a/-s/-b pin min/ave/max event size to the same value so the
# recording/replay bandwidth numbers are not distorted by a size distribution,
# -h the chronicle count. Note: -t (story_count)
# and -n (event_count) are NOT emitted here — performance_test rejects 0
# ("Only positive number is allowed"), so each test must supply its own
# positive values via test_specific_flags.
common_perf_flags() {
    local esz="$1"
    printf -- '-c %s -y -p -a %d -s %d -b %d -h %d' \
        "$CLIENT_CONF_FILE" \
        "$esz" "$esz" "$esz" \
        "$CHRONICLE_COUNT"
}

# Build the test-specific flag tail. -t (stories per process) is always 1;
# -n (event count) is 1 for lightweight tests and ecnt for
# recording/replay bandwidth tests.
test_specific_flags() {
    local test="$1"
    local ecnt="$2"
    case "$test" in
        connect_disconnect)
            # Minimum positive workload; Connect/Disconnect timers isolate phases.
            printf -- '-t 1 -n 1'
            ;;
        acquire_release)
            # 1 story per process, minimum positive event count.
            printf -- '-t 1 -n 1'
            ;;
        recording)
            # Full write workload: 1 story per process, event count from tier.
            printf -- '-w -t 1 -n %d' "$ecnt"
            ;;
        replay)
            # -r sets read=true and write=false internally; no -w needed.
            printf -- '-r -t 1 -n %d' "$ecnt"
            ;;
        *)
            printf -- ''
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Running one chrono-performance-test invocation
# ---------------------------------------------------------------------------

# redeploy_cluster <protocol> <scale>
#
# Full teardown + restart cycle used by the retry path in run_one_perf_test
# when component errors are detected in a run's output.  Returns 0 when all
# four component processes are verified healthy after the redeploy; returns 1
# if verification still fails (caller should give up retrying).
redeploy_cluster() {
    local proto="$1"
    local scale="$2"
    section "[REDEPLOY] component failure detected — redeploying (proto=$proto scale=$scale)"
    pkill_chrono_everywhere
    clean_conf_dir
    write_hosts_files
    clean_output_dir
    set_protocol_in_conf "$proto"
    deploy_cluster "$scale"
    if ! verify_components_running; then
        log "ERROR: components still not healthy after redeploy — giving up retries"
        return 1
    fi
    return 0
}

# run_one_perf_test <test_name> <protocol> <scale> <client_per_node> <client_nodes> <rep> <event_size>
run_one_perf_test() {
    local test="$1"
    local proto="$2"
    local scale="$3"
    local per_node="$4"
    local nnodes="$5"
    local rep="$6"
    local esz="$7"

    local ecnt; ecnt="$(event_count_for_size "$esz" "$scale")"
    local total_clients=$(( per_node * nnodes ))
    local common; common="$(common_perf_flags "$esz")"
    local specific; specific="$(test_specific_flags "$test" "$ecnt")"

    # mpirun/mpiexec launch line — uses the client hosts file we just wrote.
    # We wrap everything in `timeout` so a hung run cannot block the matrix.
    # --kill-after fires SIGKILL 5s after SIGTERM if the run did not exit.
    local mpirun_cmd=(
        "$MPIRUN"
        --hostfile "$CLIENT_HOSTS"
        -np "$total_clients"
        "$PERF_BIN"
        # shellcheck disable=SC2086
        $common $specific
    )
    local timeout_cmd=(timeout --kill-after=5 "${PER_TEST_TIMEOUT_SEC}" "${mpirun_cmd[@]}")

    local tag="test=${test} proto=${proto} scale=${scale} clients=${per_node}x${nnodes} esz=${esz} ecnt=${ecnt} rep=${rep}"
    section "RUN $tag"
    log "[PERF] $tag"
    log_cmd "${timeout_cmd[*]}"

    local wall_start wall_end wall rc status
    local attempt=0

    while true; do
        # Record the current LOG_FILE line count so we can isolate this run's
        # stdout/stderr output afterwards for component-error pattern scanning.
        local log_line_offset=0
        [[ "$DRY_RUN" != "1" ]] && log_line_offset=$(wc -l < "$LOG_FILE")

        wall_start=$(date +%s)
        if [[ "$DRY_RUN" == "1" ]]; then
            rc=0
        else
            # set -e is intentionally off; a non-zero rc is handled below.
            "${timeout_cmd[@]}" 2>&1 | tee -a "$LOG_FILE"
            rc=${PIPESTATUS[0]}
        fi
        wall_end=$(date +%s)
        wall=$(( wall_end - wall_start ))

        if (( rc == 124 || rc == 137 )); then
            status="TIMEOUT(${PER_TEST_TIMEOUT_SEC}s)"
            log "WARNING: $tag exceeded ${PER_TEST_TIMEOUT_SEC}s and was killed"
        elif (( rc != 0 )); then
            status="FAIL(rc=$rc)"
            log "WARNING: $tag exited with rc=$rc"
        else
            status="OK"
        fi

        # Scan the lines appended to LOG_FILE by this run for known component
        # error signatures.  Skip the scan for timeouts (the run was killed by
        # us, not by a component crash) and when retries are exhausted.
        local component_error=0
        if [[ "$DRY_RUN" != "1" && "$status" != TIMEOUT* ]] && \
                (( attempt < MAX_RETRIES )); then
            local run_lines=$(( $(wc -l < "$LOG_FILE") - log_line_offset ))
            if (( run_lines > 0 )) && \
                    tail -n "$run_lines" "$LOG_FILE" | \
                    grep -qE "$COMPONENT_ERROR_PATTERN"; then
                component_error=1
            fi
        fi

        if (( component_error )); then
            (( attempt++ ))
            log "WARNING: component error pattern detected in output (attempt $attempt/$MAX_RETRIES)"
            # Record this attempt so the CSV shows what happened before the retry.
            record_result "$(printf '%s,%s,%s,%sx%s,%s,%s,%s,%s,%ss,%s' \
                "$test" "$proto" "$scale" "$per_node" "$nnodes" "$esz" "$ecnt" "$rep" \
                "COMPONENT_FAIL(attempt=$attempt)" "$wall" "$tag")"
            if redeploy_cluster "$proto" "$scale"; then
                # Restore the client hosts file removed by clean_conf_dir.
                write_client_hosts_file "$per_node" "$nnodes"
                continue
            else
                status="COMPONENT_FAIL(redeploy_failed)"
                break
            fi
        fi

        break
    done

    record_result "$(printf '%s,%s,%s,%sx%s,%s,%s,%s,%s,%ss,%s' \
        "$test" "$proto" "$scale" "$per_node" "$nnodes" "$esz" "$ecnt" "$rep" \
        "$status" "$wall" "$tag")"

    # Cool-down: sleep ~10 % of the run's wall time so the cluster can drain
    # in-flight I/O and network buffers before the next invocation.
    # Integer division (wall / 10) is exact truncating 10 % — pure shell, no
    # external tools required.  Skipped when the quotient rounds to zero (runs
    # shorter than 10 s), which is fine for lightweight tests.
    local cooldown=$(( wall / 10 ))
    if (( cooldown > 0 )); then
        log "[COOLDOWN] ${cooldown}s (~10% of ${wall}s wall time)"
        sleep "$cooldown"
    fi
}

# ---------------------------------------------------------------------------
# Main matrix
# ---------------------------------------------------------------------------

cleanup_on_exit() {
    # Best-effort cleanup when the script exits. Don't overwrite $? here.
    local ec=$?
    log ""
    log "cleanup_on_exit (exit_code=$ec)"

    # pkill needs hosts files. If the script failed before write_hosts_files
    # ever ran, there's nothing running to kill anyway — skip teardown.
    if [[ -f "$VISOR_HOSTS" || "$DRY_RUN" == "1" ]]; then
        pkill_chrono_everywhere || true
    else
        log "hosts files do not exist — skipping component teardown"
    fi

    # Always clean generated conf files so the next run starts fresh.
    clean_conf_dir || true

    # Release the allocation only if this script instance created it.
    # Pre-existing allocations that were reused are left intact.
    if [[ "$DRY_RUN" != "1" && "$PERF_JOB_OWNED" == "1" && ${#PERF_JOB_IDS[@]} -gt 0 ]]; then
        log "[SLURM] releasing owned allocation(s) (scancel ${PERF_JOB_IDS[*]})"
        log_cmd "scancel ${PERF_JOB_IDS[*]}"
        scancel "${PERF_JOB_IDS[@]}" || true
    elif [[ ${#PERF_JOB_IDS[@]} -gt 0 ]]; then
        log "[SLURM] allocation ${PERF_JOB_IDS[*]} was pre-existing — leaving it intact"
    else
        log "[SLURM] no allocation to release"
    fi
    # Refresh the latest symlink so it always points to the most recent run,
    # even if that run failed.
    ln -sfn "$LOG_DIR_BASE" "$LATEST_LINK"
    log "done. log=$LOG_FILE cmd_log=$CMD_LOG_FILE results=$RESULT_FILE"
    log "latest -> $LATEST_LINK"
    exit $ec
}
trap cleanup_on_exit EXIT INT TERM

main() {
    # Parse script-level flags before forwarding remaining args.
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dry-run)     DRY_RUN=1;   shift ;;
            --no-dry-run)  DRY_RUN=0;   shift ;;
            --debug)       DEBUG=1;     shift ;;
            --write)       RUN_WRITE=1; shift ;;
            --no-write)    RUN_WRITE=0; shift ;;
            --read)        RUN_READ=1;  shift ;;
            --no-read)     RUN_READ=0;  shift ;;
            --colocate)    COLOCATE=1;  shift ;;
            --no-colocate) COLOCATE=0;  shift ;;
            *) break ;;
        esac
    done

    # Recompute TOTAL_NODES now that COLOCATE is finalised (CLI args may have
    # changed it from the env-var default that was used at global-scope init).
    if (( COLOCATE )); then
        TOTAL_NODES=$(( VISOR_NODES + MAX_COMP_NODES * 3 ))
    else
        TOTAL_NODES=$(( VISOR_NODES + MAX_COMP_NODES * 3 + MAX_CLIENT_NODES ))
    fi

    local colocate_desc
    if (( COLOCATE )); then
        colocate_desc="colocated (clients share player nodes)"
    else
        colocate_desc="non-colocated (clients on dedicated nodes)"
    fi

    section "ares_test.sh starting (DRY_RUN=$DRY_RUN DEBUG=$DEBUG RUN_WRITE=$RUN_WRITE RUN_READ=$RUN_READ COLOCATE=$COLOCATE)"
    log "install_dir=$INSTALL_DIR"
    log "conf_file=$CONF_FILE"
    log "perf_bin=$PERF_BIN"
    log "deploy_script=$DEPLOY_SCRIPT"
    log "log_file=$LOG_FILE"
    log "cmd_log_file=$CMD_LOG_FILE"
    log "result_file=$RESULT_FILE"
    log "total_nodes_needed=$TOTAL_NODES (visor=$VISOR_NODES + 3*$MAX_COMP_NODES comp + ${MAX_CLIENT_NODES} client — $colocate_desc)"
    log "write_client_configs=${WRITE_CLIENT_CONFIGS_ARR[*]} (${#WRITE_CLIENT_CONFIGS_ARR[@]} configs)"
    log "read_client_configs=${READ_CLIENT_CONFIGS_ARR[*]} (${#READ_CLIENT_CONFIGS_ARR[@]} configs)"

    # Result-file header.
    record_result "test,protocol,scale,clients,event_size,event_count,rep,status,wall_time,tag"

    allocate_if_needed "$@"
    discover_nodes

    # Clean any leftover generated files from a previous run before we begin.
    clean_conf_dir

    local prev_scale=""
    local first_iteration=1
    local scale proto conf rep per_node nnodes test

    for scale in "${COMPONENT_SCALES[@]}"; do
        # Scale change => repartition nodes and refresh hosts files. The
        # hosts files must exist before pkill_chrono_everywhere runs (mpssh
        # reads them), so they are written here, outside the protocol loop.
        if [[ "$scale" != "$prev_scale" ]]; then
            partition_nodes "$scale"
            write_hosts_files
            prev_scale="$scale"
        fi

        for proto in "${PROTOCOLS[@]}"; do
            section "SCALE=$scale PROTOCOL=$proto"

            # Kill any running components before (re)deploying with the new
            # protocol. On the very first iteration the nodes are assumed
            # clean, so we skip the pkill — no prior deployment to tear down.
            if (( first_iteration )); then
                first_iteration=0
            else
                pkill_chrono_everywhere
                # Wipe generated files so the next deploy starts clean.
                # Hosts files are re-written immediately after.
                clean_conf_dir
                write_hosts_files
            fi
            # Always remove HDF5 story files written by the previous run so
            # accumulated data does not overflow node-local storage.
            clean_output_dir
            set_protocol_in_conf "$proto"
            deploy_cluster "$scale"
            if ! verify_components_running; then
                log "SKIPPING scale=$scale proto=$proto — components not healthy"
                continue
            fi

            # Write tests: full client-config matrix × event size matrix.
            if (( RUN_WRITE )); then
                for conf in "${WRITE_CLIENT_CONFIGS_ARR[@]}"; do
                    per_node="${conf%%x*}"
                    nnodes="${conf##*x}"
                    write_client_hosts_file "$per_node" "$nnodes"
                    (( DEBUG )) && snapshot_conf_dir "$scale" "$proto" "$per_node" "$nnodes"

                    for esz in "${EVENT_SIZES[@]}"; do
                        for test in "${WRITE_TESTS[@]}"; do
                            for (( rep=1; rep<=REPS; rep++ )); do
                                run_one_perf_test \
                                    "$test" "$proto" "$scale" \
                                    "$per_node" "$nnodes" "$rep" "$esz"
                            done
                        done
                    done
                done
            fi

            # Read tests: 1 client per node (single player process per node limit).
            if (( RUN_READ )); then
                for conf in "${READ_CLIENT_CONFIGS_ARR[@]}"; do
                    per_node="${conf%%x*}"
                    nnodes="${conf##*x}"
                    write_client_hosts_file "$per_node" "$nnodes"
                    (( DEBUG )) && snapshot_conf_dir "$scale" "$proto" "$per_node" "$nnodes"

                    for esz in "${EVENT_SIZES[@]}"; do
                        for test in "${READ_TESTS[@]}"; do
                            for (( rep=1; rep<=REPS; rep++ )); do
                                run_one_perf_test \
                                    "$test" "$proto" "$scale" \
                                    "$per_node" "$nnodes" "$rep" "$esz"
                            done
                        done
                    done
                done
            fi
        done
    done

    section "matrix complete"
}

main "$@"
