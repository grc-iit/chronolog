#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Run or dry-run the requested Phase 0 final figure grid.

Defaults are conservative for review:
  DRY_RUN=1              generate matrix commands/metadata without launching runs
  CLIENTS_PER_NODE=8     total clients = node_count * CLIENTS_PER_NODE
  TRIALS=1               set TRIALS=3 for headline-repeat runs
  PARTITION=datacrumbs   target Ares partition

Examples:
  .agent/scripts/phase0_requested_figure_grid.sh
  DRY_RUN=0 TRIALS=3 CLIENTS_PER_NODE=8 .agent/scripts/phase0_requested_figure_grid.sh
  SMOKE=1 .agent/scripts/phase0_requested_figure_grid.sh
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MATRIX="${REPO_ROOT}/.agent/scripts/phase0_benchmark_matrix.py"

DRY_RUN="${DRY_RUN:-1}"
TRIALS="${TRIALS:-1}"
CLIENTS_PER_NODE="${CLIENTS_PER_NODE:-8}"
PARTITION="${PARTITION:-datacrumbs}"
SLURM_TIME="${SLURM_TIME:-02:00:00}"
RESULT_ROOT="${RESULT_ROOT:-${REPO_ROOT}/.agent/results/$(date +%Y%m%d-%H%M%S)-requested-final-figure-grid}"

NODES="${NODES:-1,2,4,5,16}"
SIZES="${SIZES:-1024,4096,16384,65536}"

if [[ "${SMOKE:-0}" == "1" ]]; then
  NODES="${SMOKE_NODES:-1}"
  SIZES="${SMOKE_SIZES:-1024}"
  TRIALS="${SMOKE_TRIALS:-1}"
fi

dry_run_arg=()
if [[ "${DRY_RUN}" == "1" ]]; then
  dry_run_arg=(--dry-run)
fi

mkdir -p "${RESULT_ROOT}"

operation_count_for_size() {
  case "$1" in
    1024) echo "${OPS_1K:-40000}" ;;
    4096) echo "${OPS_4K:-20000}" ;;
    16384) echo "${OPS_16K:-10000}" ;;
    65536) echo "${OPS_64K:-2500}" ;;
    *) echo "${OPS_DEFAULT:-10000}" ;;
  esac
}

run_batch() {
  local label="$1"
  shift
  local batch_dir="${RESULT_ROOT}/${label}"
  mkdir -p "${batch_dir}"
  echo "[phase0-final-grid] ${label}" | tee -a "${RESULT_ROOT}/run.log"
  python3 "${MATRIX}" \
    --partition "${PARTITION}" \
    --slurm-time "${SLURM_TIME}" \
    --trials "${TRIALS}" \
    --result-dir "${batch_dir}" \
    "${dry_run_arg[@]}" \
    "$@" 2>&1 | tee -a "${RESULT_ROOT}/run.log"
}

skip_batch() {
  local label="$1"
  local reason="$2"
  local batch_dir="${RESULT_ROOT}/${label}"
  mkdir -p "${batch_dir}"
  echo "[phase0-final-grid] ${label} SKIPPED: ${reason}" | tee -a "${RESULT_ROOT}/run.log"
  cat > "${batch_dir}/SKIPPED.md" <<EOF
# Skipped

- batch: ${label}
- reason: ${reason}
EOF
}

IFS=',' read -r -a node_values <<< "${NODES}"
IFS=',' read -r -a size_values <<< "${SIZES}"

for node_count in "${node_values[@]}"; do
  client_count=$((node_count * CLIENTS_PER_NODE))
  for size in "${size_values[@]}"; do
    operation_count="$(operation_count_for_size "${size}")"
    common=(
      --node-counts "${node_count}"
      --message-sizes "${size}"
      --operation-counts "${operation_count}"
      --client-counts "${client_count}"
    )

    if (( node_count < 2 )); then
      skip_batch "n${node_count}-s${size}-chronolog-append-sync" \
        "pending 1-node ChronoLog deployment support: current distributed wrapper requires at least 2 nodes"
      skip_batch "n${node_count}-s${size}-chronolog-append-async-wal-drain" \
        "pending 1-node ChronoLog deployment support: current distributed wrapper requires at least 2 nodes"
      skip_batch "n${node_count}-s${size}-chronolog-archive-range-sync-async-publish" \
        "pending 1-node ChronoLog deployment support: current distributed wrapper requires at least 2 nodes"
    else
      run_batch "n${node_count}-s${size}-chronolog-append-sync" \
        --systems chronolog \
        --workflows append_throughput \
        --chronolog-completion-modes keeper_journal_group_commit_tail_only \
        --chronolog-producer-wait-modes bounded_outstanding \
        --chronolog-producer-outstanding-values 16 \
        --chronolog-producer-batch-sizes 16 \
        --chronolog-keeper-journal-batch-writev-values 1 \
        --chronolog-keeper-journal-move-batch-payloads-values 1 \
        --chronolog-keeper-journal-group-commit-flush-events 64 \
        --chronolog-keeper-journal-group-commit-strict-flush-event-cap-values 1 \
        "${common[@]}"

      run_batch "n${node_count}-s${size}-chronolog-append-async-wal-drain" \
        --systems chronolog \
        --workflows append_throughput \
        --chronolog-completion-modes keeper_journal_group_fdatasync_async_drain \
        --chronolog-producer-wait-modes bounded_outstanding \
        --chronolog-producer-outstanding-values 16 \
        --chronolog-producer-batch-sizes 16 \
        --chronolog-keeper-journal-async-drain-threads 1,4 \
        "${common[@]}"

      run_batch "n${node_count}-s${size}-chronolog-archive-range-sync-async-publish" \
        --systems chronolog \
        --workflows archive_range_retrieval \
        --chronolog-completion-modes archive_readback \
        --chronolog-archive-range-event-counts "${operation_count}" \
        --chronolog-hdf5-archive-layouts raw_blob \
        --chronolog-raw-blob-async-publish-values 0,1 \
        --chronolog-raw-blob-async-publish-threads 4 \
        --chronolog-grapher-stop-story-archive-drain-values 1 \
        --chronolog-grapher-stop-story-archive-drain-timeout-ms 120000 \
        "${common[@]}"
    fi

    if (( node_count < 2 )); then
      skip_batch "n${node_count}-s${size}-kafka-append-sync-async" \
        "pending 1-node Kafka deployment support: current distributed wrapper requires at least 2 nodes"
      skip_batch "n${node_count}-s${size}-kafka-range-sync-async" \
        "pending 1-node Kafka deployment support: current distributed wrapper requires at least 2 nodes"
    else
      run_batch "n${node_count}-s${size}-kafka-append-sync-async" \
        --systems kafka \
        --workflows append_throughput \
        --kafka-acks-values 0,all \
        "${common[@]}"

      run_batch "n${node_count}-s${size}-kafka-range-sync-async" \
        --systems kafka \
        --workflows range_retrieval \
        --kafka-acks-values 0,all \
        "${common[@]}"
    fi

    run_batch "n${node_count}-s${size}-mofka-append-memory-none-noflush" \
      --systems mofka \
      --workflows append_throughput \
      --mofka-partition-types memory \
      --mofka-storage-target-types memory \
      --mofka-producer-wait-modes none \
      --mofka-producer-flush-modes no_flush \
      "${common[@]}"

    run_batch "n${node_count}-s${size}-mofka-append-memory-wait-flush" \
      --systems mofka \
      --workflows append_throughput \
      --mofka-partition-types memory \
      --mofka-storage-target-types memory \
      --mofka-producer-wait-modes per_event \
      --mofka-producer-flush-modes after_loop \
      "${common[@]}"

    run_batch "n${node_count}-s${size}-mofka-append-pmdk-none-noflush" \
      --systems mofka \
      --workflows append_throughput \
      --mofka-partition-types default \
      --mofka-storage-target-types pmdk \
      --mofka-producer-wait-modes none \
      --mofka-producer-flush-modes no_flush \
      "${common[@]}"

    run_batch "n${node_count}-s${size}-mofka-append-pmdk-wait-flush" \
      --systems mofka \
      --workflows append_throughput \
      --mofka-partition-types default \
      --mofka-storage-target-types pmdk \
      --mofka-producer-wait-modes per_event \
      --mofka-producer-flush-modes after_loop \
      "${common[@]}"

    run_batch "n${node_count}-s${size}-mofka-range-memory-afterloop-noflush" \
      --systems mofka \
      --workflows range_retrieval \
      --mofka-partition-types memory \
      --mofka-storage-target-types memory \
      --mofka-producer-wait-modes after_loop \
      --mofka-producer-flush-modes no_flush \
      "${common[@]}"

    run_batch "n${node_count}-s${size}-mofka-range-pmdk-wait-flush" \
      --systems mofka \
      --workflows range_retrieval \
      --mofka-partition-types default \
      --mofka-storage-target-types pmdk \
      --mofka-producer-wait-modes per_event \
      --mofka-producer-flush-modes after_loop \
      "${common[@]}"
  done
done

cat > "${RESULT_ROOT}/README.md" <<EOF
# Requested Phase 0 Final Figure Grid

- dry_run: ${DRY_RUN}
- trials: ${TRIALS}
- clients_per_node: ${CLIENTS_PER_NODE}
- node_counts: ${NODES}
- message_sizes: ${SIZES}
- partition: ${PARTITION}
- slurm_time: ${SLURM_TIME}

Each child directory contains the matrix metadata and generated commands for one semantic batch.
EOF

echo "[phase0-final-grid] result root: ${RESULT_ROOT}"
