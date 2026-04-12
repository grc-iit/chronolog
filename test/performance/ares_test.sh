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
# Knobs
# ---------------------------------------------------------------------------

: "${DRY_RUN:=1}"                         # 1 = echo-only, 0 = actually run
: "${INSTALL_DIR:=$HOME/chronolog-install/chronolog}"
: "${CONF_DIR:=$INSTALL_DIR/conf}"
: "${BIN_DIR:=$INSTALL_DIR/bin}"
: "${TOOLS_DIR:=$INSTALL_DIR/tools}"
: "${TESTS_DIR:=$INSTALL_DIR/tests}"
: "${PERF_BIN:=$TESTS_DIR/chrono-performance-test}"
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
: "${SLURM_PARTITION:=}"                  # optional: -p <part> for salloc
: "${SLURM_TIME_LIMIT:=04:00:00}"
: "${SLURM_JOB_NAME:=chronolog-perf}"

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

mkdir -p "$LOG_DIR"
: > "$LOG_FILE"
: > "$CMD_LOG_FILE"
: > "$RESULT_FILE"

# Fixed component scales (symmetric: N keepers == N graphers == N players).
COMPONENT_SCALES=(1 2 4)
VISOR_NODES=1
MAX_COMP_NODES=4                                 # max of COMPONENT_SCALES
MAX_CLIENT_NODES=4                               # max #nodes across CLIENT_CONFIGS
# Total allocation: 1 visor + 4 keepers + 4 graphers + 4 players = 13 nodes.
# Client nodes overlap with the player-slot subset of the component pool, so
# they do not add to the allocation. Visor is never shared.
TOTAL_NODES=$(( VISOR_NODES + MAX_COMP_NODES * 3 ))  # 1 + 12 = 13

PROTOCOLS=(ofi+sockets ofi+verbs)

# Client configs, space-separated "<procs_per_node>x<nodes>" tokens, in the
# order given by perf_test_plan.md. Overridable from the environment so the
# operator can start with a small subset and grow it. Examples:
#   CLIENT_CONFIGS='1x1'             bash ares_test.sh   # smoke test
#   CLIENT_CONFIGS='1x1 10x1 10x2'   bash ares_test.sh   # first three
#   bash ares_test.sh                                    # full plan matrix
: "${CLIENT_CONFIGS:=1x1 10x1 10x2 20x1 10x4 20x2 40x1 20x4 40x2 40x4}"
# Parse the string into an array so the main loop can iterate it.
read -ra CLIENT_CONFIGS_ARR <<< "$CLIENT_CONFIGS"

# test_name:extra_flags (barrier is always on per the plan)
# - connect/disconnect: only connect + disconnect, no story work
# - acquire/release:    story lifecycle, no event I/O
# - recording:          write path, #story_per_proc=#client
# - replay:             read path, #story_per_proc=#client
#
# The performance_test binary uses a single bifurcated run with flags; we map
# the plan's four cases onto these flag sets. #story_per_proc == #client is
# expressed by passing -t <client_per_node*nodes> per test.
TESTS=(
    "connect_disconnect"
    "acquire_release"
    "recording"
    "replay"
)

# Per-test shared workload settings. Event size is fixed (min==ave==max) so
# the recording/replay bandwidth numbers are not distorted by a payload-size
# distribution; see common_perf_flags for how this is wired into -a/-s/-b.
EVENT_COUNT_DEFAULT=1000
EVENT_SIZE_DEFAULT=4096
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
    printf 'BIN_DIR=%s\n'          "$BIN_DIR"
    printf 'TOOLS_DIR=%s\n'        "$TOOLS_DIR"
    printf 'TESTS_DIR=%s\n'        "$TESTS_DIR"
    printf 'PERF_BIN=%s\n'         "$PERF_BIN"
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
    printf 'PER_TEST_TIMEOUT_SEC=%s\n' "$PER_TEST_TIMEOUT_SEC"
    printf 'REPS=%s\n'             "$REPS"
    printf 'SLURM_PARTITION=%s\n'  "$SLURM_PARTITION"
    printf 'SLURM_TIME_LIMIT=%s\n' "$SLURM_TIME_LIMIT"
    printf 'SLURM_JOB_NAME=%s\n'   "$SLURM_JOB_NAME"
    printf '\n# --- Node layout ---\n'
    printf 'VISOR_NODES=%s\n'      "$VISOR_NODES"
    printf 'MAX_COMP_NODES=%s\n'   "$MAX_COMP_NODES"
    printf 'MAX_CLIENT_NODES=%s\n' "$MAX_CLIENT_NODES"
    printf 'TOTAL_NODES=%s\n'      "$TOTAL_NODES"
    printf '\n# --- Workload ---\n'
    printf 'EVENT_COUNT_DEFAULT=%s\n' "$EVENT_COUNT_DEFAULT"
    printf 'EVENT_SIZE_DEFAULT=%s\n'  "$EVENT_SIZE_DEFAULT"
    printf 'EVENT_INTERVAL_US=%s\n'   "$EVENT_INTERVAL_US"
    printf 'CHRONICLE_COUNT=%s\n'     "$CHRONICLE_COUNT"
    printf '\n# --- Arrays ---\n'
    printf 'COMPONENT_SCALES=(%s)\n'  "${COMPONENT_SCALES[*]}"
    printf 'PROTOCOLS=(%s)\n'         "${PROTOCOLS[*]}"
    printf 'CLIENT_CONFIGS_ARR=(%s)\n' "${CLIENT_CONFIGS_ARR[*]}"
    printf 'TESTS=(%s)\n'             "${TESTS[*]}"
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
    # The script runs from an admin node, NOT inside the perf-test allocation.
    # First, check squeue for a running job with our job name. If found, grab
    # its job ID and proceed. Otherwise, create a new allocation and exit so
    # the operator can rerun once nodes are ready.
    log "[SLURM] checking squeue for running job named '$SLURM_JOB_NAME'"
    log_cmd "squeue -u \$USER -n $SLURM_JOB_NAME -h -o '%i' | head -1"

    if [[ "$DRY_RUN" == "1" ]]; then
        PERF_JOB_ID="dry-run-jobid"
        log "DRY_RUN=1, skipping real squeue — faking PERF_JOB_ID=$PERF_JOB_ID"
        return 0
    fi

    PERF_JOB_ID=$(squeue -u "$USER" -n "$SLURM_JOB_NAME" -h -o '%i' | head -1)
    if [[ -n "$PERF_JOB_ID" ]]; then
        log "Found existing job PERF_JOB_ID=$PERF_JOB_ID"
        return 0
    fi

    # No running job — allocate one with --no-shell and exit. The operator
    # reruns this script once the allocation is granted (squeue will then
    # find the job by name on the next invocation).
    local salloc_args=(
        -J "$SLURM_JOB_NAME"
        -N "$TOTAL_NODES"
        --ntasks-per-node=1
        -t "$SLURM_TIME_LIMIT"
        --exclusive
        --no-shell
    )
    [[ -n "$SLURM_PARTITION" ]] && salloc_args+=(-p "$SLURM_PARTITION")

    log "[SLURM] no '$SLURM_JOB_NAME' job found — requesting allocation (salloc --no-shell)"
    log_cmd "salloc ${salloc_args[*]}"
    salloc "${salloc_args[@]}"
    local rc=$?
    log "salloc exited with rc=$rc — rerun this script once the job appears in squeue"
    exit $rc
}

# Populate ALLOCATED_NODES by querying squeue for the nodelist of the perf
# job. In dry-run we fabricate fake names so the rest of the script has
# deterministic output to review.
discover_nodes() {
    if [[ "$DRY_RUN" == "1" ]]; then
        ALLOCATED_NODES=()
        for ((i=1; i<=TOTAL_NODES; i++)); do
            ALLOCATED_NODES+=("dry-node$(printf '%02d' "$i")")
        done
        log "[SLURM] discovering nodes (dry-run, PERF_JOB_ID=$PERF_JOB_ID)"
        log_cmd "squeue -j $PERF_JOB_ID -h -o '%N'"
        log_cmd "scontrol show hostnames <nodelist>"
        log "        -> ${ALLOCATED_NODES[*]}"
        return 0
    fi

    log "[SLURM] discovering nodes for job $PERF_JOB_ID via squeue"
    log_cmd "squeue -j $PERF_JOB_ID -h -o '%N'"
    local nodelist
    nodelist=$(squeue -j "$PERF_JOB_ID" -h -o '%N')
    if [[ -z "$nodelist" ]]; then
        log "ERROR: squeue returned empty nodelist for job $PERF_JOB_ID — is the job still running?"
        exit 1
    fi
    log "        compact nodelist: $nodelist"

    # Expand compact notation (e.g. node[01-13]) into individual hostnames.
    log_cmd "scontrol show hostnames $nodelist"
    mapfile -t ALLOCATED_NODES < <(scontrol show hostnames "$nodelist")
    log "        -> ${ALLOCATED_NODES[*]}"
    if (( ${#ALLOCATED_NODES[@]} < TOTAL_NODES )); then
        log "ERROR: need $TOTAL_NODES nodes, got ${#ALLOCATED_NODES[@]}"
        exit 1
    fi
}

# Slice ALLOCATED_NODES into role groups for a given component scale. With
# only 13 nodes available the client pool can no longer have its own dedicated
# nodes — it overlaps with the player slot subset of the allocation. Visor is
# still a dedicated node, and keeper/grapher/player slots never overlap with
# each other.
#
# Layout (MAX_COMP_NODES = 4 → 13 total nodes):
#   [0]                                 -> visor                 (dedicated)
#   [1 .. 1+MAX_COMP_NODES)             -> keeper slot pool
#   [1+MAX_COMP_NODES .. 1+2*MAX_COMP_NODES)   -> grapher slot pool
#   [1+2*MAX_COMP_NODES .. 1+3*MAX_COMP_NODES) -> player slot pool
#   client pool == the 4 player-slot nodes (overlap is by design)
#
# The active keeper/grapher/player nodelists take the first `scale` nodes from
# their respective slot pools; unused slots stay idle. Client nodes are pinned
# to the player-slot pool so they stay on the same physical nodes across scale
# changes, which keeps client placement out of the scale comparison.
partition_nodes() {
    local scale="$1"

    VISOR_NODELIST=("${ALLOCATED_NODES[0]}")

    local off=1
    KEEPER_NODELIST=("${ALLOCATED_NODES[@]:$off:$scale}")

    off=$(( 1 + MAX_COMP_NODES ))
    GRAPHER_NODELIST=("${ALLOCATED_NODES[@]:$off:$scale}")

    off=$(( 1 + 2 * MAX_COMP_NODES ))
    PLAYER_NODELIST=("${ALLOCATED_NODES[@]:$off:$scale}")

    # Client pool overlaps with the player slot pool (all 4 slots, not just
    # the ones actually running a player at the current scale). This means
    # clients always land on the same physical nodes regardless of scale.
    CLIENT_POOL=("${ALLOCATED_NODES[@]:$off:$MAX_CLIENT_NODES}")

    log "Node partition for scale=$scale:"
    log "    visor   : ${VISOR_NODELIST[*]}"
    log "    keepers : ${KEEPER_NODELIST[*]}"
    log "    graphers: ${GRAPHER_NODELIST[*]}"
    log "    players : ${PLAYER_NODELIST[*]}"
    log "    clients : ${CLIENT_POOL[*]} (overlap with player slot pool)"
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
    local per_node="$1"
    local nnodes="$2"
    if (( nnodes > MAX_CLIENT_NODES )); then
        log "ERROR: requested $nnodes client nodes > pool $MAX_CLIENT_NODES"
        return 1
    fi
    local subset=("${CLIENT_POOL[@]:0:$nnodes}")

    # mpirun-style hostfile: "<host> slots=<per_node>"
    local content=""
    local h
    for h in "${subset[@]}"; do
        content+="${h} slots=${per_node}"$'\n'
    done
    write_file "$CLIENT_HOSTS" "${content%$'\n'}"
}

# ---------------------------------------------------------------------------
# Deployment lifecycle
# ---------------------------------------------------------------------------

pkill_chrono_everywhere() {
    section "pkill -9 chrono on all component nodes"
    # mpssh -f <host_file> "<remote cmd>" fans out the kill in parallel. We
    # target one component type per hosts file so the visor hosts file is
    # never touched by keeper/grapher/player patterns and vice versa.
    run_cmd "mpssh pkill chrono-visor"   mpssh -f "$VISOR_HOSTS"   "pkill -9 -f chrono-visor || true"
    run_cmd "mpssh pkill chrono-keeper"  mpssh -f "$KEEPER_HOSTS"  "pkill -9 -f chrono-keeper || true"
    run_cmd "mpssh pkill chrono-grapher" mpssh -f "$GRAPHER_HOSTS" "pkill -9 -f chrono-grapher || true"
    run_cmd "mpssh pkill chrono-player"  mpssh -f "$PLAYER_HOSTS"  "pkill -9 -f chrono-player || true"
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

# Shared flags for every perf run. -y enables barriers, -p enables perf
# reporting, -a/-s/-b pin min/ave/max event size to the same value so the
# recording/replay bandwidth numbers are not distorted by a size distribution,
# -g the inter-event interval, -h the chronicle count. Note: -t (story_count)
# and -n (event_count) are NOT emitted here — performance_test rejects 0
# ("Only positive number is allowed"), so each test must supply its own
# positive values via test_specific_flags.
common_perf_flags() {
    printf -- '-c %s -y -p -a %d -s %d -b %d -g %d -h %d' \
        "$CLIENT_CONF_FILE" \
        "$EVENT_SIZE_DEFAULT" \
        "$EVENT_SIZE_DEFAULT" \
        "$EVENT_SIZE_DEFAULT" \
        "$EVENT_INTERVAL_US" \
        "$CHRONICLE_COUNT"
}

# Build the test-specific flag tail. #story_per_proc=#client is honored for
# every test except connect/disconnect, which only exercises the session
# endpoints. Because performance_test rejects 0 for -t / -n, we fall back to
# the smallest positive values (1) instead of attempting to zero the workload
# — TimerWrapper reports each phase (Connect, CreateChronicle, AcquireStory,
# WriteEvent, ReplayStory, ReleaseStory, DestroyChronicle, Disconnect)
# separately, so a tiny amount of extra work does not contaminate the
# Connect/Disconnect or AcquireStory/ReleaseStory measurements.
test_specific_flags() {
    local test="$1"
    local total_clients="$2"   # #client_per_node * #nodes
    case "$test" in
        connect_disconnect)
            # Minimum positive workload; Connect/Disconnect timers isolate phases.
            printf -- '-t 1 -n 1'
            ;;
        acquire_release)
            # #story_per_proc=#client, minimum positive event count.
            printf -- '-t %d -n 1' "$total_clients"
            ;;
        recording)
            # Full write workload: #story_per_proc=#client, default event count.
            printf -- '-w -t %d -n %d' "$total_clients" "$EVENT_COUNT_DEFAULT"
            ;;
        replay)
            # -r sets read=true and write=false internally; no -w needed.
            printf -- '-r -t %d -n %d' "$total_clients" "$EVENT_COUNT_DEFAULT"
            ;;
        *)
            printf -- ''
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Running one chrono-performance-test invocation
# ---------------------------------------------------------------------------

# run_one_perf_test <test_name> <protocol> <scale> <client_per_node> <client_nodes> <rep>
run_one_perf_test() {
    local test="$1"
    local proto="$2"
    local scale="$3"
    local per_node="$4"
    local nnodes="$5"
    local rep="$6"

    local total_clients=$(( per_node * nnodes ))
    local common; common="$(common_perf_flags)"
    local specific; specific="$(test_specific_flags "$test" "$total_clients")"

    # mpirun/mpiexec launch line — uses the client hosts file we just wrote.
    # We wrap everything in `timeout` so a hung run cannot block the matrix.
    # --kill-after fires SIGKILL 5s after SIGTERM if the run did not exit.
    local mpirun_cmd=(
        mpirun
        --hostfile "$CLIENT_HOSTS"
        -np "$total_clients"
        "$PERF_BIN"
        # shellcheck disable=SC2086
        $common $specific
    )

    local tag="test=${test} proto=${proto} scale=${scale} clients=${per_node}x${nnodes} rep=${rep}"
    section "RUN $tag"

    local wall_start wall_end rc
    wall_start=$(date +%s)

    local timeout_cmd=(timeout --kill-after=5 "${PER_TEST_TIMEOUT_SEC}" "${mpirun_cmd[@]}")

    log "[PERF] $tag"
    log_cmd "${timeout_cmd[*]}"

    if [[ "$DRY_RUN" == "1" ]]; then
        rc=0
    else
        # The script does not run with `set -e`, so a non-zero rc here simply
        # propagates through $? without aborting the matrix. Do NOT toggle
        # errexit here — turning it on would permanently enable it for the
        # rest of the run and cause later, unrelated failures to abort.
        "${timeout_cmd[@]}" 2>&1 | tee -a "$LOG_FILE"
        rc=${PIPESTATUS[0]}
    fi

    wall_end=$(date +%s)
    local wall=$(( wall_end - wall_start ))

    local status="OK"
    if (( rc == 124 || rc == 137 )); then
        status="TIMEOUT(${PER_TEST_TIMEOUT_SEC}s)"
        log "WARNING: $tag exceeded ${PER_TEST_TIMEOUT_SEC}s and was killed"
    elif (( rc != 0 )); then
        status="FAIL(rc=$rc)"
        log "WARNING: $tag exited with rc=$rc"
    fi

    record_result "$(printf '%s,%s,%s,%sx%s,%s,%s,%ss,%s' \
        "$test" "$proto" "$scale" "$per_node" "$nnodes" "$rep" \
        "$status" "$wall" "$tag")"
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

    # Always scancel the perf job so nodes are released on any failure.
    if [[ "$DRY_RUN" != "1" && -n "${PERF_JOB_ID:-}" ]]; then
        log "[SLURM] releasing allocation (scancel $PERF_JOB_ID)"
        log_cmd "scancel $PERF_JOB_ID"
        scancel "$PERF_JOB_ID" || true
    else
        log "[SLURM] releasing allocation (dry-run)"
        log_cmd "scancel \$PERF_JOB_ID"
    fi
    log "done. log=$LOG_FILE cmd_log=$CMD_LOG_FILE results=$RESULT_FILE"
    exit $ec
}
trap cleanup_on_exit EXIT INT TERM

main() {
    section "ares_test.sh starting (DRY_RUN=$DRY_RUN)"
    log "install_dir=$INSTALL_DIR"
    log "conf_file=$CONF_FILE"
    log "perf_bin=$PERF_BIN"
    log "deploy_script=$DEPLOY_SCRIPT"
    log "log_file=$LOG_FILE"
    log "cmd_log_file=$CMD_LOG_FILE"
    log "result_file=$RESULT_FILE"
    log "total_nodes_needed=$TOTAL_NODES (visor=$VISOR_NODES + 3*$MAX_COMP_NODES, clients overlap player slots)"
    log "client_configs=${CLIENT_CONFIGS_ARR[*]} (${#CLIENT_CONFIGS_ARR[@]} configs)"

    # Result-file header.
    record_result "test,protocol,scale,clients,rep,status,wall_time,tag"

    allocate_if_needed "$@"
    discover_nodes

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
            fi
            set_protocol_in_conf "$proto"
            deploy_cluster "$scale"
            if ! verify_components_running; then
                log "SKIPPING scale=$scale proto=$proto — components not healthy"
                continue
            fi

            for conf in "${CLIENT_CONFIGS_ARR[@]}"; do
                # token is "<per_node>x<nnodes>" (e.g. 10x2).
                per_node="${conf%%x*}"
                nnodes="${conf##*x}"
                write_client_hosts_file "$per_node" "$nnodes"

                for test in "${TESTS[@]}"; do
                    for (( rep=1; rep<=REPS; rep++ )); do
                        run_one_perf_test \
                            "$test" "$proto" "$scale" \
                            "$per_node" "$nnodes" "$rep"
                    done
                done
            done
        done
    done

    section "matrix complete"
}

main "$@"
