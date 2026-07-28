#!/bin/bash
# Driver for the live_tail_read A/B benchmark. See design.md.
#
# Same binary in both arms; the only variable is
# chrono_keeper.DataStoreInternals.live_tail_read.
#
#   off = existing behaviour, tail reads served from sealed chunks only
#   on  = this branch, sealed tail unioned with the active (unsealed) timeline
#
# Usage: run_bench.sh [-j job_id] [-r reps] [-f "A B1 B2"] [-a "off on"] [-o results_dir]
#
# Topology (6 nodes): node0 = visor+grapher+player, node1..2 = keepers,
# node3..5 = client ranks. Host files are written by hand and --job-id is
# deliberately NOT passed to deploy_cluster.sh: its prepare_hosts() overwrites
# all four host files with keepers=ALL nodes, which would put clients on keeper
# nodes and destroy CPU attribution.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
MPIEXEC="${MPIEXEC:-/mnt/common/kfeng/spack/opt/spack/linux-ubuntu22.04-skylake_avx512/gcc-11.4.0/mpich-4.0.2-yomnocixlvz4mtgvih66sj7bp4zetml7/bin/mpiexec}"
BENCH="${WORK_DIR}/tools/benchmark/chrono-bench"
CONF_FILE="${WORK_DIR}/conf/default-chrono-conf.json"
CLIENT_CONF="${WORK_DIR}/conf/default-chrono-client-conf.json"
DEPLOY="${WORK_DIR}/tools/deploy/deploy_cluster.sh"

JOB_ID=""
REPS=3
FAMILIES="A B1 B2"
ARMS="off on"
RESULTS_ROOT="${SCRIPT_DIR}/results/$(date +%Y%m%d_%H%M%S)"
RUN_TIMEOUT="10m"

# Seal window = story_chunk_duration_secs + acceptance_window_secs = 25 s.
CHUNK_SECS=10
ACCEPT_SECS=15
TAIL_CAPACITY=65536
MAX_WAIT=63 # >= 2.5 x seal window, so OFF-arm samples are not truncated

# Payload is 256 B across all latency families, NOT 4096. playback(n) returns
# full payloads for n events on EVERY poll, so poll traffic is
# ranks * n * payload / poll_interval; at 4 KB the deep-tail points would demand
# tens of GB/s. Identical in both arms, so the A/B stays valid.
PAYLOAD=256

usage() {
    grep '^#' "$0" | sed -n '2,20p' | sed 's/^# \?//'
    exit 1
}

while getopts "j:r:f:a:o:h" opt; do
    case ${opt} in
    j) JOB_ID="${OPTARG}" ;;
    r) REPS="${OPTARG}" ;;
    f) FAMILIES="${OPTARG}" ;;
    a) ARMS="${OPTARG}" ;;
    o) RESULTS_ROOT="${OPTARG}" ;;
    *) usage ;;
    esac
done

# ---------------------------------------------------------------------------
# Node assignment
# ---------------------------------------------------------------------------
if [[ -z "${JOB_ID}" ]]; then
    JOB_ID=$(squeue -h -u "$(whoami)" -o "%i" | head -1)
fi
if [[ -z "${JOB_ID}" ]]; then
    echo "No Slurm job found; pass -j <job_id>." >&2
    exit 1
fi

mapfile -t NODES < <(scontrol show hostnames "$(squeue -h -j "${JOB_ID}" -o '%N')")
if [[ ${#NODES[@]} -lt 6 ]]; then
    echo "Need >= 6 nodes in job ${JOB_ID}, got ${#NODES[@]}." >&2
    exit 1
fi

VISOR_NODE="${NODES[0]}"
KEEPER_NODES=("${NODES[1]}" "${NODES[2]}")
CLIENT_NODES=("${NODES[3]}" "${NODES[4]}" "${NODES[5]}")
NUM_CLIENT_NODES=${#CLIENT_NODES[@]}

mkdir -p "${RESULTS_ROOT}"
MANIFEST="${RESULTS_ROOT}/manifest.txt"
{
    echo "job_id=${JOB_ID}"
    echo "visor_grapher_player=${VISOR_NODE}"
    echo "keepers=${KEEPER_NODES[*]}"
    echo "clients=${CLIENT_NODES[*]}"
    echo "reps=${REPS} families=${FAMILIES} arms=${ARMS}"
    echo "chunk_secs=${CHUNK_SECS} accept_secs=${ACCEPT_SECS} seal_window=$((CHUNK_SECS + ACCEPT_SECS))s"
    echo "tail_capacity=${TAIL_CAPACITY} payload=${PAYLOAD}B max_wait=${MAX_WAIT}s"
    echo "git_commit=$(cd "${SCRIPT_DIR}" && git rev-parse --short HEAD 2>/dev/null)"
} | tee "${MANIFEST}"

# ---------------------------------------------------------------------------
# Deployment helpers
# ---------------------------------------------------------------------------
write_host_files() {
    printf '%s\n' "${VISOR_NODE}" >"${WORK_DIR}/conf/hosts_visor"
    printf '%s\n' "${VISOR_NODE}" >"${WORK_DIR}/conf/hosts_grapher"
    printf '%s\n' "${VISOR_NODE}" >"${WORK_DIR}/conf/hosts_player"
    printf '%s\n' "${KEEPER_NODES[@]}" >"${WORK_DIR}/conf/hosts_keeper"
    printf '%s\n' "${CLIENT_NODES[@]}" >"${WORK_DIR}/conf/hosts_client"
}

# Patch the base conf. deploy_cluster.sh derives every per-component conf from
# this file with jq, so the keeper copies inherit whatever is set here.
set_arm_conf() {
    local arm=$1
    local live="false"
    [[ "${arm}" == "on" ]] && live="true"
    local tmp="${WORK_DIR}/conf/.arm.json"
    jq ".chrono_keeper.DataStoreInternals.live_tail_read = ${live}
        | .chrono_keeper.DataStoreInternals.story_chunk_duration_secs = ${CHUNK_SECS}
        | .chrono_keeper.DataStoreInternals.acceptance_window_secs = ${ACCEPT_SECS}
        | .chrono_keeper.DataStoreInternals.tail_capacity = ${TAIL_CAPACITY}" \
        "${CONF_FILE}" >"${tmp}" && mv "${tmp}" "${CONF_FILE}"
    echo "  conf: $(jq -c '.chrono_keeper.DataStoreInternals' "${CONF_FILE}")"
}

kill_stale() {
    # deploy_cluster.sh does not detect prior runs; a stale daemon holds ports
    # and the new launch segfaults on startup.
    local all=("${VISOR_NODE}" "${KEEPER_NODES[@]}")
    for h in "${all[@]}"; do
        ssh -n -o StrictHostKeyChecking=no "${h}" \
            "pkill -9 -f 'chrono-(visor|keeper|grapher|player) --config'" >/dev/null 2>&1 || true
    done
    sleep 3
}

deploy_start() {
    kill_stale
    write_host_files
    (cd "${WORK_DIR}" && "${DEPLOY}" --start --work-dir "${WORK_DIR}" --record-groups 1) \
        >"${RUN_LOG_DIR}/deploy_start.log" 2>&1
    local rc=$?
    # Give keepers time to register with the visor before the first client runs.
    sleep 20
    return ${rc}
}

deploy_stop() {
    (cd "${WORK_DIR}" && "${DEPLOY}" --stop --work-dir "${WORK_DIR}" --record-groups 1) \
        >"${RUN_LOG_DIR}/deploy_stop.log" 2>&1 || true
    kill_stale
}

# ---------------------------------------------------------------------------
# Keeper CPU/RSS samplers
# ---------------------------------------------------------------------------
SAMPLER_STOP=""
start_samplers() {
    local tag=$1
    SAMPLER_STOP="${RUN_LOG_DIR}/.sampler_stop_${tag}"
    rm -f "${SAMPLER_STOP}"
    for h in "${KEEPER_NODES[@]}"; do
        ssh -n -o StrictHostKeyChecking=no "${h}" \
            "nohup bash ${SCRIPT_DIR}/keeper_sampler.sh '${RUN_LOG_DIR}/${tag}.keeper-${h}.csv' '${SAMPLER_STOP}' >/dev/null 2>&1 &" \
            >/dev/null 2>&1 || true
    done
}

stop_samplers() {
    [[ -n "${SAMPLER_STOP}" ]] && touch "${SAMPLER_STOP}"
    sleep 2
}

# ---------------------------------------------------------------------------
# One measurement point
#
# run_point <tag> <ranks> <events_per_rank> <interval_us> <poll_ms> <playback_n> <shared:0|1>
# ---------------------------------------------------------------------------
run_point() {
    local tag=$1 ranks=$2 events=$3 interval_us=$4 poll_ms=$5 pb_n=$6 shared=$7
    local ppn=$(((ranks + NUM_CLIENT_NODES - 1) / NUM_CLIENT_NODES))
    local shared_flag=""
    [[ "${shared}" == "1" ]] && shared_flag="-o"

    echo "    [${tag}] ranks=${ranks} events=${events} interval=${interval_us}us poll=${poll_ms}ms playback_n=${pb_n} shared=${shared}"

    start_samplers "${tag}"
    LD_LIBRARY_PATH="${WORK_DIR}/lib:${LD_LIBRARY_PATH:-}" \
        timeout "${RUN_TIMEOUT}" "${MPIEXEC}" -n "${ranks}" -ppn "${ppn}" -f "${WORK_DIR}/conf/hosts_client" \
        "${BENCH}" -c "${CLIENT_CONF}" -l ${shared_flag} \
        -h 1 -t 1 -a "${PAYLOAD}" -s "${PAYLOAD}" -b "${PAYLOAD}" \
        -n "${events}" -g "${interval_us}" -e "${poll_ms}" -k "${pb_n}" -m "${MAX_WAIT}" \
        -x "${RUN_LOG_DIR}/${tag}" \
        >"${RUN_LOG_DIR}/${tag}.log" 2>&1
    local rc=$?
    stop_samplers
    [[ ${rc} -ne 0 ]] && echo "      WARNING: exit ${rc} (see ${tag}.log)"

    # Surface the headline lines immediately so a broken run is obvious.
    grep -E "send->visible|playback\(\) call|log_event\(\) call|Applied write|never seen|WARNING" \
        "${RUN_LOG_DIR}/${tag}.log" 2>/dev/null | sed 's/^/      /'
    return 0
}

# ---------------------------------------------------------------------------
# Families
# ---------------------------------------------------------------------------

# Family A - visibility latency (the win).
# PER-RANK stories, not shared: playback_n then only has to cover one rank's own
# stream. With a shared story the OFF arm needs playback_n >= events-per-chunk-
# period or a whole sealed chunk scrolls past the last-N window unobserved and is
# miscounted as never_seen, biasing OFF's latency downward.
family_A() {
    local arm=$1
    for ranks in 6 24 60 120; do
        run_point "A_${arm}_r${ranks}" "${ranks}" 400 100000 50 256 0
    done
}

# Family B1 - write-path interference. Shared story so every rank contends on one
# pipeline's sequencingMutex. Write pressure fixed; poll interval sweeps READ
# pressure. 60 ranks x 1000 events = 60000 <= tail_capacity 65536.
family_B1() {
    local arm=$1
    for poll in 1000 200 50 10; do
        run_point "B1_${arm}_p${poll}" 60 1000 50000 "${poll}" 2048 1
    done
}

# Family B2 - playback() service latency vs tail depth. The sharpest test: the ON
# path costs 1 + misses acquisitions of sequencingMutex per read, so cost should
# grow with depth. 24 ranks x 2000 = 48000 events, so depth 16384 is reachable and
# nothing evicts. never_seen is expected to be high here at shallow depths - the
# metric is playback() service time, not visibility.
family_B2() {
    local arm=$1
    for pb_n in 64 256 1024 4096 16384; do
        run_point "B2_${arm}_n${pb_n}" 24 2000 20000 500 "${pb_n}" 1
    done
}

# Smoke - one small point, validates the whole chain before the real sweep.
# OFF should land near the 25 s seal window, ON well under a second.
family_SMOKE() {
    local arm=$1
    run_point "SMOKE_${arm}" 6 60 100000 50 256 0
}

# ---------------------------------------------------------------------------
# Main loop: rep outermost, then arm, so each arm is deployed once per rep.
# ---------------------------------------------------------------------------
for rep in $(seq 1 "${REPS}"); do
    for arm in ${ARMS}; do
        RUN_LOG_DIR="${RESULTS_ROOT}/rep${rep}/${arm}"
        mkdir -p "${RUN_LOG_DIR}"
        echo ""
        echo "=== rep ${rep} / arm ${arm} ==============================="
        set_arm_conf "${arm}"

        if ! deploy_start; then
            echo "  deploy failed, see ${RUN_LOG_DIR}/deploy_start.log" >&2
            deploy_stop
            continue
        fi
        # Record what the keeper actually loaded, so the arm is provable later.
        cp "${CONF_FILE}" "${RUN_LOG_DIR}/effective-chrono-conf.json" 2>/dev/null || true

        for fam in ${FAMILIES}; do
            echo "  --- family ${fam} ---"
            case "${fam}" in
            A) family_A "${arm}" ;;
            B1) family_B1 "${arm}" ;;
            B2) family_B2 "${arm}" ;;
            SMOKE) family_SMOKE "${arm}" ;;
            *) echo "  unknown family ${fam}" >&2 ;;
            esac
        done

        deploy_stop
    done
done

echo ""
echo "Done. Results in ${RESULTS_ROOT}"
