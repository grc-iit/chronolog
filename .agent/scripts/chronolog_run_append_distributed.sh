#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PROJECT_TOOL_BIN="${REPO_ROOT}/.agent/tools/bin"
PROJECT_TOOL_LIB="${REPO_ROOT}/.agent/tools/lib"
if [[ -d "${PROJECT_TOOL_BIN}" ]]; then
  export PATH="${PROJECT_TOOL_BIN}:${PATH:-}"
fi
if [[ -d "${PROJECT_TOOL_LIB}" ]]; then
  export LD_LIBRARY_PATH="${PROJECT_TOOL_LIB}:${LD_LIBRARY_PATH:-}"
fi
export PATH="/usr/local/bin:/usr/bin:/bin:${PATH:-}"

reject_tmpfs_storage_path() {
  local label="$1"
  local path="$2"
  local resolved
  resolved="$(realpath -m "${path}")"
  case "${resolved}" in
    /tmp|/tmp/*|/dev/shm|/dev/shm/*)
      echo "${label} must not be tmpfs-backed (${resolved}); choose .agent/results on persistent/shared storage or explicit local NVMe/SSD" >&2
      exit 2
      ;;
  esac
  if command -v findmnt >/dev/null 2>&1; then
    local fs_type
    local mount_probe="${resolved}"
    while [[ ! -e "${mount_probe}" && "${mount_probe}" != "/" ]]; do
      mount_probe="$(dirname "${mount_probe}")"
    done
    fs_type="$(findmnt -T "${mount_probe}" -no FSTYPE 2>/dev/null | head -n 1 || true)"
    case "${fs_type}" in
      tmpfs|ramfs|devtmpfs)
        echo "${label} resolves to ${fs_type} (${resolved}); storage/durability benchmarks must not use memory-backed filesystems" >&2
        exit 2
        ;;
    esac
  fi
}

usage() {
  cat <<'USAGE'
Usage: chronolog_run_append_distributed.sh [options]

Run a distributed bare-metal ChronoLog benchmark/profiling workflow.

Options:
  --result-dir DIR             Existing or new result directory to use.
  --partition NAME             SLURM partition. Default: debug.
  --nodelist LIST              Optional SLURM nodelist, for example ares-comp-[03-04].
  --node-count N               Node count. Default: 2.
  --client-count N             Client process count. Default: 1. Multi-client is implemented for append_throughput, mixed_append_tail, keeper_restart_recovery, and archive_range_retrieval.
  --slurm-time TIME            SLURM time limit. Default: 00:10:00.
  --record-groups N            ChronoLog recording groups. Default: 1.
  --workflow NAME              append_throughput, append_latency, range_retrieval, archive_range_retrieval,
                               mixed_append_tail, keeper_restart_recovery, or append_durable. Default: append_throughput.
  --completion-mode MODE       Completion mode for durable workflows. Default: live_return.
                               Journal modes include keeper_journal_buffered,
                               keeper_journal_fdatasync, keeper_journal_fdatasync_tail_only,
                               keeper_journal_group_fdatasync,
                               keeper_journal_group_fdatasync_early_ack, and
                               keeper_journal_group_fdatasync_async_drain,
                               keeper_journal_group_fdatasync_wal_drain,
                               keeper_journal_group_fdatasync_tail_only,
                               keeper_journal_group_commit_tail_only,
                               keeper_journal_group_commit_deferred_tail_only.
  --operation-count N          Events per client. Default: 10.
  --message-size-bytes N       Average event size. Default: 1024.
  --data-collection-poll-interval-us N
                               DataStore polling interval in microseconds. Default: 1000000.
  --grapher-inactive-story-delay-seconds N
                               Grapher pipeline retirement delay after ReleaseStory. If unset, append_durable
                               archive_readback uses 3 seconds explicitly; other workflows use the installed config.
  --grapher-retire-on-stop MODE
                               Whether Grapher finalizes the story pipeline during stop_story_recording. Default: env or 1.
  --grapher-stop-retire-grace-us N
                               Bounded microsecond grace before Grapher stop-story finalizes and unregisters ingestion.
                               Default: CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US or 0.
  --grapher-stop-story-archive-drain MODE
                               Whether Grapher stop_story_recording waits until the retired story archive extraction
                               queue drains. Default: CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN or 0.
  --grapher-stop-story-archive-drain-timeout-ms N
                               Timeout for Grapher archive-drain stop semantic. Default: 30000.
  --grapher-stop-drain-wait-outside-lock MODE
                               Default-off probe: release Grapher datastore mutex while waiting for Keeper
                               drain-complete notifications during stop. 0 or 1.
  --grapher-extraction-threads N
                               Grapher archive extraction threads. Default: CHRONOLOG_GRAPHER_EXTRACTION_THREADS or 2.
  --hdf5-archive-atomic-rename MODE
                               Write HDF5 chunks to a temporary file without SWMR, close, then rename. 0 or 1.
                               Default: CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME or 0.
  --hdf5-archive-chunk-events N
                               HDF5 dataset chunk length in events. 0 keeps the default contiguous layout.
                               Default: CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS or 0.
  --hdf5-archive-layout NAME   HDF5 archive layout: vlen, blob_map, raw_blob, or fixed_record. Default: CHRONOLOG_HDF5_ARCHIVE_LAYOUT or vlen.
  --archive-event-count-poll-interval-seconds N
                               Harness-side archive event-count polling interval for archive_readback validation.
                               Default: CHRONOLOG_ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS or 1.0.
  --archive-event-count-wait-mode MODE
                               Harness-side archive event-count wait mode: inline or parallel_process.
                               Default: CHRONOLOG_ARCHIVE_EVENT_COUNT_WAIT_MODE or inline.
  --archive-readback-mode MODE
                               Harness-side archive readback issue mode: inline or parallel_thread.
                               Default: CHRONOLOG_ARCHIVE_READBACK_MODE or inline.
  --raw-blob-preallocate MODE  Preallocate raw_blob payload sidecar with posix_fallocate before writev. 0 or 1.
                               Default: CHRONOLOG_RAW_BLOB_PREALLOCATE or 0.
  --raw-blob-async-close MODE  For raw_blob atomic archive writes, close the payload sidecar concurrently
                               with HDF5 metadata work, then wait before final rename. 0 or 1.
                               Default: CHRONOLOG_RAW_BLOB_ASYNC_CLOSE or 0.
  --raw-blob-async-write MODE  For raw_blob atomic archive writes, write and close the payload sidecar
                               concurrently with HDF5 metadata work, then wait before publication. 0 or 1.
                               Default: CHRONOLOG_RAW_BLOB_ASYNC_WRITE or 0.
  --raw-blob-async-publish MODE
                               For raw_blob atomic archive writes, enqueue payload close, final rename, and manifest
                               publication on a dedicated publisher thread. 0 or 1.
                               Default: CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH or 0.
  --raw-blob-async-publish-threads N
                               Dedicated publisher worker count for --raw-blob-async-publish=1.
                               Default: CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS or 1.
  --raw-blob-publish-before-close MODE
                               With --raw-blob-async-publish=1, rename/publish raw_blob archives before waiting
                               for delayed payload close validation. Probe only; 0 or 1.
                               Default: CHRONOLOG_RAW_BLOB_PUBLISH_BEFORE_CLOSE or 0.
  --raw-blob-fast-reserve MODE
                               Reserve the deterministic raw_blob archive temp filename with O_EXCL
                               before falling back to directory scan. 0 or 1.
                               Default: CHRONOLOG_RAW_BLOB_FAST_RESERVE or 0.
  --raw-blob-sidecar-meta MODE
                               Store raw_blob event offset/size metadata in a binary .rawmeta sidecar
                               instead of an HDF5 raw_meta dataset. 0 or 1.
                               Default: CHRONOLOG_RAW_BLOB_SIDECAR_META or 0.
  --chrono-bench-barrier MODE  Whether chrono-bench uses --barrier. true or false. Default: false.
  --chrono-bench-shared-story MODE
                               Whether chrono-bench uses --shared_story. true or false. Default: false.
  --chronolog-producer-outstanding N
                               Outstanding chrono-bench write calls per MPI rank before waiting, or Keeper RPC futures
                               when --chronolog-producer-wait-mode=bounded_futures. Default: 1.
  --chronolog-producer-batch-size N
                               Records grouped into one chrono-bench write call before waiting. Default: 1.
  --chronolog-producer-wait-mode MODE
                               ChronoLog producer wait policy: auto, per_event, bounded_outstanding,
                               bounded_futures, bounded_api, bounded_per_keeper, or after_loop.
                               Default: CHRONOLOG_PRODUCER_WAIT_MODE or auto.
  --chronolog-bench-owned-batch MODE
                               Enable chrono-bench owned batch submission path. Values: 0/1/true/false.
                               Default: CHRONOLOG_BENCH_OWNED_BATCH or 0.
  --chronolog-bench-owned-batch-min-message-size N
                               Use the owned batch path only when a submitted chrono-bench batch contains a payload
                               at least this large. 0 preserves size-agnostic owned-batch behavior. Default:
                               CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE or 0.
  --chronolog-client-batch-keeper-selection MODE
                               Keeper routing for multi-record client batches: per_event or single_keeper. Default:
                               CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION or per_event.
  --chronolog-client-keeper-time-bucket-ns N
                               Optional timestamp bucket in ns for RoundRobin Keeper selection. 0 preserves raw
                               timestamp modulo Keeper count. Default: CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS or 0.
  --chronolog-client-parallel-tail-rpc MODE
                               Enable parallel Keeper-tail RPC collection for full client replay. 0 or 1.
                               Default: CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC or 1.
  --visor-parallel-recording-stop MODE
                               Diagnostic release path: notify Keeper and Grapher stop paths concurrently. 0 or 1.
                               Default: CHRONOLOG_VISOR_PARALLEL_RECORDING_STOP or 0.
  --chronolog-client-keeper-cursor-drain MODE
                               Drain successive Keeper cursor batches inside one replay_tail_incremental call.
                               0 or 1. Default: CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN or 0.
  --chronolog-client-keeper-cursor-drain-max-batches N
                               Optional per-Keeper cap on drained cursor batches per replay call. 0 means no cap.
                               Default: CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES or 0.
  --chronolog-client-keeper-cursor-packed-batch MODE
                               Use packed metadata+payload cursor batch RPC wire format. 0 or 1.
                               Default: CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH or 0.
  --chronolog-client-keeper-cursor-metadata-only-output MODE
                               Diagnostic for packed Keeper cursor batches: retrieve payload bytes but return
                               metadata-only Events with empty records to avoid payload string reconstruction.
                               0 or 1. Default: CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT or 0.
  --chronolog-client-keeper-cursor-packed-bulk MODE
                               Diagnostic for keeper_cursor_packed: move packed payload bytes through a
                               Thallium bulk transfer while returning metadata in the RPC response. 0 or 1.
                               Default: CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK or 0.
  --chronolog-client-keeper-cursor-packed-bulk-stream MODE
                               Diagnostic for keeper_cursor_packed bulk: let the Keeper advance multiple
                               cursor batches inside one RPC and return one combined bulk payload. 0 or 1.
                               Default: CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM or 0.
  --chronolog-client-keeper-cursor-packed-bulk-stream-max-batches N
                               Optional Keeper-side cap for packed bulk stream batches per RPC. 0 means no
                               explicit cap beyond the client receive buffer. Default:
                               CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES or 0.
  --chronolog-client-keeper-cursor-packed-bulk-buffer-bytes N
                               Per-Keeper client receive buffer for packed bulk transfer. Default:
                               CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES or
                               CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES.
  --chronolog-keeper-tail-packed-bulk-uninitialized-buffer MODE
                               Diagnostic for packed bulk Keeper tail: read payloads into an uninitialized
                               Keeper-side bulk buffer instead of std::string payloadBlob before transfer.
                               0 or 1. Default:
                               CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER or 0.
  --chronolog-keeper-tail-packed-bulk-chunk-bytes N
                               Diagnostic for packed bulk Keeper tail: split one payload bulk transfer into
                               N-byte Thallium bulk subsegment transfers. 0 preserves one transfer.
                               Default: CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES or 0.
  --chronolog-keeper-tail-packed-bulk-parallel-transfer MODE
                               Diagnostic for packed bulk Keeper tail: use Margo's parallel bulk-transfer
                               helper with --chronolog-keeper-tail-packed-bulk-chunk-bytes. 0 or 1.
                               Default: CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER or 0.
  --chronolog-mixed-tail-read-mode MODE
                               mixed_append_tail read mode: full, incremental, keeper_cursor, or
                               keeper_cursor_packed. Default:
                               CHRONOLOG_MIXED_TAIL_READ_MODE or full.
  --chronolog-mixed-tail-reader-start-mode MODE
                               mixed_append_tail reader start: ready or after_writers_done. Default:
                               CHRONOLOG_MIXED_TAIL_READER_START_MODE or ready.
  --chronolog-mixed-tail-writer-release-story MODE
                               Whether mixed_append_tail writers call ReleaseStory before reader completion.
                               yes, no, or after_reader. Default: CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY or yes.
  --chronolog-client-execution-mode MODE
                               keeper_restart_recovery client execution: sequential or threads. Sequential preserves
                               older rows; threads applies concurrent client pressure. Default: sequential.
  --chronicle-count N          Chronicles per process. Default: 1.
  --story-count N              Stories per process. Default: 1.
  --install-dir DIR            ChronoLog install tree. Default: .agent/install-consistent/chronolog.
  --profile-mode MODE          none, tau, gperftools, darshan, perf, strace, or ebpf. Default: none.
  --keeper-journal-shards N    Keeper-local journal shards per Keeper. Default: 1.
  --keeper-journal-shard-policy POLICY
                               Shard selection policy: mixed, client, story_client, or story. Default: mixed.
  --keeper-journal-placement MODE
                               Journal placement: shared or node_local. Default: shared.
  --keeper-journal-local-base DIR
                               Base directory on each Keeper node for node_local placement. Required when placement is node_local.
  --keeper-journal-tail-read-mode MODE
                               Keeper journal tail payload read mode: auto, direct, coalesced, or vectored.
                               Default: CHRONOLOG_KEEPER_JOURNAL_TAIL_READ_MODE or auto.
  --keeper-journal-tail-direct-read-into-events 0|1
                               Opt-in diagnostic path that reads direct journal-tail payloads into returned events.
                               Default: CHRONOLOG_KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS or 0.
  --keeper-tail-batch-max-events N
                               Maximum events returned by one Keeper journal-tail cursor RPC. 0 means unlimited.
                               Default: CHRONOLOG_KEEPER_TAIL_BATCH_MAX_EVENTS or 0.
  --keeper-tail-batch-max-bytes N
                               Maximum payload bytes returned by one Keeper journal-tail cursor RPC. 0 means unlimited.
                               Default: CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES or 0.
  --keeper-journal-fdatasync-batch-events N
                               Events per shard between periodic fdatasync calls in grouped mode. Non-boundary appends
                               do not wait for per-record fdatasync. Default: 64.
  --keeper-journal-batch-writev MODE
                               Write each Keeper journal owner batch with one pwritev when group-commit is active. 0 or 1.
                               Default: CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV or 0.
  --keeper-journal-move-batch-payloads MODE
                               Move record_events payload strings into deferred Keeper journal work instead of copying
                               them. Only affects deferred batch RPC journal completion. 0 or 1.
                               Default: CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS or 0.
  --keeper-journal-async-callback-dispatch MODE
                               Dispatch single-record callbacks on the callback dispatcher instead of inline on the
                               journal owner. 0 or 1. Default: CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH or 0.
  --keeper-journal-async-batch-completion-dispatch MODE
                               Dispatch deferred batch RPC completions after journal durability on the callback
                               dispatcher instead of executing the response callback on the journal owner. 0 or 1.
                               Default: CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH or 1.
  --keeper-journal-callback-dispatch-threads N
                               Callback dispatcher thread count for async callback/batch completion dispatch.
                               Default: CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS or 1.
  --keeper-journal-callback-batch-drain-min-payload-bytes N
                               When callback batch drain is enabled, drain multiple callback completions only if
                               the front completion has at least this max payload size. 0 preserves unconditional
                               batch drain. Default: CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES or 0.
  --keeper-journal-durable-complete-before-publish MODE
                               Complete deferred RPCs immediately after grouped journal fdatasync and before
                               publishing Keeper tail-index entries. 0 or 1. Default:
                               CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH or 0.
  --keeper-journal-group-commit-flush-events N
                               Opt-in deferred group-commit records per fdatasync before RPC completion. Default: 1.
  --keeper-journal-group-commit-flush-bytes N
                               Optional deferred group-commit byte cap before fdatasync. 0 disables. Default:
                               CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES or 0.
  --keeper-journal-group-commit-strict-flush-event-cap MODE
                               Prevent owner drains from overshooting --keeper-journal-group-commit-flush-events
                               when a pending group is already partially full. 0 or 1. Default:
                               CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP or 0.
  --keeper-journal-group-commit-large-payload-bytes N
                               Payload size threshold that enables a lower deferred group-commit event cap. 0 disables.
  --keeper-journal-group-commit-large-payload-flush-events N
                               Event cap used when the pending batch contains payloads at or above the large-payload threshold.
  --keeper-journal-group-commit-wait-us N
                               Optional microsecond window to accumulate single-writer group-commit appends before
                               fdatasync. Default: 0.
  --keeper-journal-group-commit-flush-wait-us N
                               Optional microsecond window after journal writes to wait for more appends before
                               group-commit fdatasync. Default: 0.
  --keeper-journal-group-commit-queue-boundary-min-events N
                               When flush-wait is enabled, wait at queue boundary only while the pending group
                               has fewer than this many records. 0 preserves unconditional queue-boundary behavior.
  --keeper-journal-group-commit-queue-boundary-min-payload-bytes N
                               Apply queue-boundary minimum-record waiting only when the pending group contains
                               a payload at least this large. 0 preserves size-agnostic behavior.
  --keeper-journal-group-commit-queue-boundary-wait-every-n N
                               Wait at every Nth eligible queue boundary. 1 preserves current behavior.
  --keeper-journal-owner-drain-yields N
                               Optional yield count while the shard owner is draining append work before grouped
                               fdatasync. This is a coalescing probe for queue-boundary flushes, not a durability
                               semantic change. Default: CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS or 0.
  --keeper-journal-notify-owner-only-on-empty MODE
                               Notify the shard owner only when append enqueue transitions the owner queue from
                               empty to non-empty. 0 or 1. This is a scheduling probe, not a durability semantic
                               change. Default: CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY or 0.
  --keeper-append-stats-interval-events N
                               Keeper append-stats log interval. Smaller values are useful for smoke tests where
                               per-Keeper event distribution can fall below the default interval.
                               Default: CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS or 10000.
  --keeper-restart-pre-crash-stats-wait-seconds N
                               For keeper_restart_recovery, request a SIGUSR1 append-stats snapshot and wait this
                               many seconds before the intentional SIGKILL restart. Default:
                               CHRONOLOG_KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS or 2.
  --keeper-wal-drain-batch-events N
                               WAL cursor records per async-drain read batch. Default: 1.
  --keeper-wal-drain-batch-wait-us N
                               Optional microsecond window to accumulate WAL cursors before drain. Default: 0.
  --keeper-journal-async-drain-threads N
                               Number of Keeper async-drain workers for journal-first compatibility drain.
                               Default: CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_THREADS or 1.
  --keeper-data-collection-streams N
                               Keeper datastore collection xstreams. Default: CHRONOLOG_KEEPER_DATA_COLLECTION_STREAMS or 3.
  --keeper-data-collection-threads-per-stream N
                               Keeper datastore collection threads per xstream. Default:
                               CHRONOLOG_KEEPER_DATA_COLLECTION_THREADS_PER_STREAM or 2.
  --keeper-recording-margo-progress-thread MODE
                               Keeper recording service Margo progress-thread flag passed to margo_init. 0 or 1.
                               Default: CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD or 1.
  --keeper-recording-margo-handlers N
                               Keeper recording service Margo RPC handler count passed to margo_init.
                               Default: CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS or 1.
  --grapher-data-collection-streams N
                               Grapher datastore collection xstreams. Default: CHRONOLOG_GRAPHER_DATA_COLLECTION_STREAMS or 3.
  --grapher-data-collection-threads-per-stream N
                               Grapher datastore collection threads per xstream. Default:
                               CHRONOLOG_GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM or 2.
  --grapher-recording-margo-xstreams N
                               Grapher recording service Margo progress-thread flag/count passed to margo_init.
                               Default: CHRONOLOG_GRAPHER_RECORDING_MARGO_XSTREAMS or 1.
  --grapher-recording-margo-handlers N
                               Grapher recording service Margo RPC handler count passed to margo_init.
                               Default: CHRONOLOG_GRAPHER_RECORDING_MARGO_HANDLERS or 1.
  --grapher-direct-deserialize MODE
                               Use direct-buffer Cereal deserialization in Grapher receive path. 0 or 1.
                               Default: CHRONOLOG_GRAPHER_DIRECT_DESERIALIZE or 0.
  --keeper-drain-margo-progress-thread N
                               Keeper-to-Grapher drain sender Thallium client progress-thread flag. Default: 0.
  --keeper-drain-margo-rpc-threads N
                               Keeper-to-Grapher drain sender RPC thread count. Default: 0.
  --keeper-extraction-threads N
                               Keeper extraction worker threads that serialize and drain story chunks. Default: 2.
  --keeper-direct-serialize MODE
                               Serialize Keeper drain chunks directly into the outbound string buffer. 0 or 1.
                               Default: CHRONOLOG_KEEPER_DIRECT_SERIALIZE or 0.
  --keeper-fast-wire MODE      Use compact ChronoLog-specific Keeper-to-Grapher wire format. 0 or 1.
                               Default: CHRONOLOG_KEEPER_FAST_WIRE or 1.
  --perf-bin PATH              perf binary for profile-mode=perf. Default: CHRONOLOG_PERF_BIN or PATH.
  --perf-call-graph MODE       perf call graph mode for profile-mode=perf. Default: CHRONOLOG_PERF_CALL_GRAPH or fp.
  -h, --help                   Show this help.
USAGE
}

timestamp() {
  date +%Y%m%d-%H%M%S
}

make_result_dir() {
  local requested="$1"
  if [[ -z "${requested}" ]]; then
    requested="${REPO_ROOT}/.agent/results/$(timestamp)"
  fi
  mkdir -p "${requested}"
  cd "${requested}" && pwd
}

module_loads() {
  cat <<'EOF'
module load gcc/11.4.0/cmake/3.30.5-pq5ntgo
module load gcc/11.4.0/argobots/1.1-indjy5q
module load gcc/11.4.0/libfabric/1.22.0-lyviqpl
module load gcc/11.4.0/mercury/2.3.1-kcmn6u3
module load gcc/11.4.0/mochi-margo/0.17.0-llh62si
module load gcc/11.4.0/mochi-thallium/0.10.1-2pari3f
module load gcc/11.4.0/boost/1.86.0-cs4onpk
module load gcc/11.4.0/spdlog/1.14.1-y4quhau
module load gcc/11.4.0/json-c/0.16-2ofsb6t
module load gcc/11.4.0/pkgconf/2.2.0-3sczowe
module load gcc/11.4.0/python/3.11.9-zg4555e
module load gcc/11.4.0/py-pybind11/2.13.5-pngixx5
module load openmpi/5.0.5-x2kvx5l/gcc/11.4.0/hdf5/1.14.5-5zbyeqh
EOF
}

RESULT_DIR="${PHASE0_RESULT_DIR:-}"
PARTITION="${CHRONOLOG_SLURM_PARTITION:-debug}"
NODELIST="${CHRONOLOG_NODELIST:-}"
NODE_COUNT="${CHRONOLOG_NODE_COUNT:-2}"
CLIENT_COUNT="${CHRONOLOG_CLIENT_COUNT:-1}"
CLIENT_MPI_OVERSUBSCRIBE="${CHRONOLOG_CLIENT_MPI_OVERSUBSCRIBE:-0}"
SLURM_TIME="${CHRONOLOG_SLURM_TIME:-00:10:00}"
RECORD_GROUPS="${CHRONOLOG_RECORD_GROUPS:-1}"
OPERATION_COUNT="${CHRONOLOG_OPERATION_COUNT:-10}"
MESSAGE_SIZE_BYTES="${CHRONOLOG_MESSAGE_SIZE_BYTES:-1024}"
DATA_COLLECTION_POLL_INTERVAL_US="${CHRONOLOG_DATA_COLLECTION_POLL_INTERVAL_US:-1000000}"
GRAPHER_INACTIVE_STORY_DELAY_SECONDS="${CHRONOLOG_GRAPHER_INACTIVE_STORY_DELAY_SECONDS:-}"
GRAPHER_RETIRE_ON_STOP="${CHRONOLOG_GRAPHER_RETIRE_ON_STOP:-1}"
GRAPHER_STOP_RETIRE_GRACE_US="${CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US:-0}"
GRAPHER_STOP_STORY_ARCHIVE_DRAIN="${CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN:-0}"
GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS="${CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS:-30000}"
GRAPHER_STOP_DRAIN_COMPLETE_WAIT="${CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT:-0}"
GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS="${CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS:-30000}"
GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC="${CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC:-0}"
GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK="${CHRONOLOG_GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK:-0}"
GRAPHER_EXTRACTION_THREADS="${CHRONOLOG_GRAPHER_EXTRACTION_THREADS:-2}"
HDF5_ARCHIVE_ATOMIC_RENAME="${CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME:-0}"
HDF5_ARCHIVE_CHUNK_EVENTS="${CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS:-0}"
HDF5_ARCHIVE_LAYOUT="${CHRONOLOG_HDF5_ARCHIVE_LAYOUT:-vlen}"
ARCHIVE_EVENT_COUNT_WAIT_MODE="${CHRONOLOG_ARCHIVE_EVENT_COUNT_WAIT_MODE:-inline}"
ARCHIVE_READBACK_MODE="${CHRONOLOG_ARCHIVE_READBACK_MODE:-inline}"
RAW_BLOB_PREALLOCATE="${CHRONOLOG_RAW_BLOB_PREALLOCATE:-0}"
RAW_BLOB_ASYNC_WRITE="${CHRONOLOG_RAW_BLOB_ASYNC_WRITE:-0}"
RAW_BLOB_ASYNC_CLOSE="${CHRONOLOG_RAW_BLOB_ASYNC_CLOSE:-0}"
RAW_BLOB_ASYNC_PUBLISH="${CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH:-0}"
RAW_BLOB_ASYNC_PUBLISH_THREADS="${CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS:-1}"
RAW_BLOB_PUBLISH_BEFORE_CLOSE="${CHRONOLOG_RAW_BLOB_PUBLISH_BEFORE_CLOSE:-0}"
RAW_BLOB_FAST_RESERVE="${CHRONOLOG_RAW_BLOB_FAST_RESERVE:-0}"
RAW_BLOB_SIDECAR_META="${CHRONOLOG_RAW_BLOB_SIDECAR_META:-0}"
CHRONO_BENCH_BARRIER="${CHRONOLOG_CHRONO_BENCH_BARRIER:-false}"
CHRONO_BENCH_SHARED_STORY="${CHRONOLOG_CHRONO_BENCH_SHARED_STORY:-false}"
CHRONOLOG_PRODUCER_OUTSTANDING="${CHRONOLOG_PRODUCER_OUTSTANDING:-1}"
CHRONOLOG_PRODUCER_BATCH_SIZE="${CHRONOLOG_PRODUCER_BATCH_SIZE:-1}"
CHRONOLOG_PRODUCER_WAIT_MODE="${CHRONOLOG_PRODUCER_WAIT_MODE:-auto}"
CHRONOLOG_BENCH_OWNED_BATCH="${CHRONOLOG_BENCH_OWNED_BATCH:-0}"
CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE="${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE:-0}"
CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION="${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION:-per_event}"
CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS="${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS:-0}"
CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC="${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC:-1}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN="${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN:-0}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES="${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES:-0}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH:-0}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT="${CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT:-0}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK:-0}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM:-0}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES:-0}"
CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES:-}"
CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER="${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER:-0}"
CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES="${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES:-0}"
CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER="${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER:-0}"
CHRONOLOG_MIXED_TAIL_READ_MODE="${CHRONOLOG_MIXED_TAIL_READ_MODE:-full}"
CHRONOLOG_MIXED_TAIL_READER_START_MODE="${CHRONOLOG_MIXED_TAIL_READER_START_MODE:-ready}"
CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY="${CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY:-yes}"
CHRONOLOG_CLIENT_EXECUTION_MODE="${CHRONOLOG_CLIENT_EXECUTION_MODE:-sequential}"
if [[ "${CHRONOLOG_CLIENT_EXECUTION_MODE}" == "threaded" ]]; then
  CHRONOLOG_CLIENT_EXECUTION_MODE="threads"
fi
if [[ "${CHRONOLOG_CLIENT_EXECUTION_MODE}" != "sequential" && "${CHRONOLOG_CLIENT_EXECUTION_MODE}" != "threads" ]]; then
  echo "Unsupported ChronoLog client execution mode: ${CHRONOLOG_CLIENT_EXECUTION_MODE}. Expected sequential or threads." >&2
  exit 2
fi
CHRONICLE_COUNT="${CHRONOLOG_CHRONICLE_COUNT:-1}"
STORY_COUNT="${CHRONOLOG_STORY_COUNT:-1}"
INSIDE_ALLOCATION="${CHRONOLOG_INSIDE_ALLOCATION:-0}"
WORKFLOW="${CHRONOLOG_WORKFLOW:-append_throughput}"
COMPLETION_MODE="${CHRONOLOG_COMPLETION_MODE:-live_return}"
INSTALL_DIR="${CHRONOLOG_INSTALL_DIR:-${REPO_ROOT}/.agent/install-consistent/chronolog}"
PROFILE_MODE="${CHRONOLOG_PROFILE_MODE:-none}"
STARTUP_SLEEP_SECONDS="${CHRONOLOG_STARTUP_SLEEP_SECONDS:-5}"
STARTUP_READY_TIMEOUT_SECONDS="${CHRONOLOG_STARTUP_READY_TIMEOUT_SECONDS:-60}"
PERF_BIN="${CHRONOLOG_PERF_BIN:-}"
PERF_EVENT="${CHRONOLOG_PERF_EVENT:-cycles}"
PERF_CALL_GRAPH="${CHRONOLOG_PERF_CALL_GRAPH:-fp}"
GPERFTOOLS_PRELOAD_LIB="${CHRONOLOG_GPERFTOOLS_PRELOAD_LIB:-${REPO_ROOT}/opt/gperftools/lib/libtcmalloc_and_profiler.so}"
GPERFTOOLS_ENABLE_CPU="${CHRONOLOG_GPERFTOOLS_ENABLE_CPU:-1}"
GPERFTOOLS_ENABLE_HEAP="${CHRONOLOG_GPERFTOOLS_ENABLE_HEAP:-0}"
GPERFTOOLS_HEAP_ALLOCATION_INTERVAL="${CHRONOLOG_GPERFTOOLS_HEAP_ALLOCATION_INTERVAL:-10485760}"
GPERFTOOLS_CPU_PROFILE_SIGNAL="${CHRONOLOG_GPERFTOOLS_CPU_PROFILE_SIGNAL:-}"
DEPLOY_STOP_TIMEOUT_SECONDS="${CHRONOLOG_DEPLOY_STOP_TIMEOUT_SECONDS:-90}"
KEEPER_APPEND_STATS_WAIT_SECONDS="${CHRONOLOG_KEEPER_APPEND_STATS_WAIT_SECONDS:-10}"
KEEPER_APPEND_STATS_INTERVAL_EVENTS="${CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS:-10000}"
CLIENT_APPEND_STATS_INTERVAL_EVENTS="${CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS:-${KEEPER_APPEND_STATS_INTERVAL_EVENTS}}"
KEEPER_STOP_STORY_STATS_WAIT_MS="${CHRONOLOG_KEEPER_STOP_STORY_STATS_WAIT_MS:-0}"
KEEPER_STOP_STORY_FLUSH_DRAIN="${CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN:-0}"
KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS="${CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS:-30000}"
KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE="${CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE:-0}"
KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS="${CHRONOLOG_KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS:-2}"
VISOR_PARALLEL_KEEPER_STOP="${CHRONOLOG_VISOR_PARALLEL_KEEPER_STOP:-0}"
VISOR_PARALLEL_RECORDING_STOP="${CHRONOLOG_VISOR_PARALLEL_RECORDING_STOP:-0}"
KEEPER_JOURNAL_SHARDS="${CHRONOLOG_KEEPER_JOURNAL_SHARDS:-1}"
KEEPER_JOURNAL_SHARD_POLICY="${CHRONOLOG_KEEPER_JOURNAL_SHARD_POLICY:-mixed}"
KEEPER_JOURNAL_PLACEMENT="${CHRONOLOG_KEEPER_JOURNAL_PLACEMENT:-shared}"
KEEPER_JOURNAL_LOCAL_BASE="${CHRONOLOG_KEEPER_JOURNAL_LOCAL_BASE:-}"
KEEPER_JOURNAL_LOCAL_PRECLEAN="${CHRONOLOG_KEEPER_JOURNAL_LOCAL_PRECLEAN:-1}"
KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS="${CHRONOLOG_KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS:-64}"
KEEPER_JOURNAL_TAIL_READ_MODE="${CHRONOLOG_KEEPER_JOURNAL_TAIL_READ_MODE:-auto}"
KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS="${CHRONOLOG_KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS:-0}"
KEEPER_TAIL_BATCH_MAX_EVENTS="${CHRONOLOG_KEEPER_TAIL_BATCH_MAX_EVENTS:-0}"
KEEPER_TAIL_BATCH_MAX_BYTES="${CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES:-0}"
KEEPER_JOURNAL_WRITEV="${CHRONOLOG_KEEPER_JOURNAL_WRITEV:-0}"
KEEPER_JOURNAL_BATCH_WRITEV="${CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV:-0}"
KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS="${CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS:-0}"
KEEPER_JOURNAL_ATOMIC_OFFSETS="${CHRONOLOG_KEEPER_JOURNAL_ATOMIC_OFFSETS:-0}"
KEEPER_JOURNAL_SINGLE_WRITER="${CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER:-0}"
KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS="${CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS:-64}"
KEEPER_JOURNAL_GROUP_COMMIT_WAIT="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS:-1}"
KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS:-64}"
KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES:-0}"
KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N:-1}"
KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS="${CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS:-0}"
KEEPER_JOURNAL_OWNER_DRAIN_YIELDS="${CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS:-0}"
KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY="${CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY:-0}"
KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH="${CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH:-0}"
KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH="${CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH:-1}"
KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS="${CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS:-1}"
KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN="${CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN:-0}"
KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX="${CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX:-0}"
KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES="${CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES:-0}"
KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH="${CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH:-0}"
KEEPER_JOURNAL_ASYNC_DRAIN_THREADS="${CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_THREADS:-1}"
KEEPER_RECORDING_MARGO_XSTREAMS="${CHRONOLOG_KEEPER_RECORDING_MARGO_XSTREAMS:-1}"
KEEPER_RECORDING_MARGO_PROGRESS_THREAD="${CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD:-1}"
KEEPER_RECORDING_MARGO_HANDLERS="${CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS:-1}"
GRAPHER_RECORDING_MARGO_XSTREAMS="${CHRONOLOG_GRAPHER_RECORDING_MARGO_XSTREAMS:-1}"
GRAPHER_RECORDING_MARGO_HANDLERS="${CHRONOLOG_GRAPHER_RECORDING_MARGO_HANDLERS:-1}"
GRAPHER_DIRECT_DESERIALIZE="${CHRONOLOG_GRAPHER_DIRECT_DESERIALIZE:-0}"
KEEPER_DRAIN_MARGO_PROGRESS_THREAD="${CHRONOLOG_KEEPER_DRAIN_MARGO_PROGRESS_THREAD:-1}"
KEEPER_DRAIN_MARGO_RPC_THREADS="${CHRONOLOG_KEEPER_DRAIN_MARGO_RPC_THREADS:-0}"
KEEPER_EXTRACTION_THREADS="${CHRONOLOG_KEEPER_EXTRACTION_THREADS:-2}"
KEEPER_DIRECT_SERIALIZE="${CHRONOLOG_KEEPER_DIRECT_SERIALIZE:-0}"
KEEPER_FAST_WIRE="${CHRONOLOG_KEEPER_FAST_WIRE:-1}"
KEEPER_WAL_DRAIN_BATCH_EVENTS="${CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_EVENTS:-1}"
KEEPER_WAL_DRAIN_BATCH_WAIT_US="${CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_WAIT_US:-0}"
KEEPER_DATA_COLLECTION_STREAMS="${CHRONOLOG_KEEPER_DATA_COLLECTION_STREAMS:-3}"
KEEPER_DATA_COLLECTION_THREADS_PER_STREAM="${CHRONOLOG_KEEPER_DATA_COLLECTION_THREADS_PER_STREAM:-2}"
GRAPHER_DATA_COLLECTION_STREAMS="${CHRONOLOG_GRAPHER_DATA_COLLECTION_STREAMS:-3}"
GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM="${CHRONOLOG_GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM:-2}"
ARCHIVE_WAIT_SECONDS="${CHRONOLOG_ARCHIVE_WAIT_SECONDS:-420}"
ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS="${CHRONOLOG_ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS:-1.0}"
ARCHIVE_RANGE_EVENT_COUNT="${CHRONOLOG_ARCHIVE_RANGE_EVENT_COUNT:-0}"
HDF5_ARCHIVE_READER_BIN="${CHRONOLOG_HDF5_ARCHIVE_READER_BIN:-${REPO_ROOT}/.agent/build-hdf5-unit/Release/test/unit/chrono-player/chronolog-test-player-hdf5-archive-reader}"
export CHRONOLOG_KEEPER_APPEND_STATS_WAIT_SECONDS="${KEEPER_APPEND_STATS_WAIT_SECONDS}"
export CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS="${KEEPER_APPEND_STATS_INTERVAL_EVENTS}"
export CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS="${CLIENT_APPEND_STATS_INTERVAL_EVENTS}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-dir) RESULT_DIR="$2"; shift 2 ;;
    --partition) PARTITION="$2"; shift 2 ;;
    --nodelist) NODELIST="$2"; shift 2 ;;
    --node-count) NODE_COUNT="$2"; shift 2 ;;
    --client-count) CLIENT_COUNT="$2"; shift 2 ;;
    --client-mpi-oversubscribe) CLIENT_MPI_OVERSUBSCRIBE="$2"; shift 2 ;;
    --slurm-time) SLURM_TIME="$2"; shift 2 ;;
    --record-groups) RECORD_GROUPS="$2"; shift 2 ;;
    --workflow) WORKFLOW="$2"; shift 2 ;;
    --completion-mode) COMPLETION_MODE="$2"; shift 2 ;;
    --operation-count) OPERATION_COUNT="$2"; shift 2 ;;
    --message-size-bytes) MESSAGE_SIZE_BYTES="$2"; shift 2 ;;
    --data-collection-poll-interval-us) DATA_COLLECTION_POLL_INTERVAL_US="$2"; shift 2 ;;
    --grapher-inactive-story-delay-seconds) GRAPHER_INACTIVE_STORY_DELAY_SECONDS="$2"; shift 2 ;;
    --grapher-retire-on-stop) GRAPHER_RETIRE_ON_STOP="$2"; shift 2 ;;
    --grapher-stop-retire-grace-us) GRAPHER_STOP_RETIRE_GRACE_US="$2"; shift 2 ;;
    --grapher-stop-story-archive-drain) GRAPHER_STOP_STORY_ARCHIVE_DRAIN="$2"; shift 2 ;;
    --grapher-stop-story-archive-drain-timeout-ms) GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS="$2"; shift 2 ;;
    --grapher-stop-drain-wait-outside-lock) GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK="$2"; shift 2 ;;
    --grapher-extraction-threads) GRAPHER_EXTRACTION_THREADS="$2"; shift 2 ;;
    --hdf5-archive-atomic-rename) HDF5_ARCHIVE_ATOMIC_RENAME="$2"; shift 2 ;;
    --hdf5-archive-chunk-events) HDF5_ARCHIVE_CHUNK_EVENTS="$2"; shift 2 ;;
    --hdf5-archive-layout) HDF5_ARCHIVE_LAYOUT="$2"; shift 2 ;;
    --archive-event-count-poll-interval-seconds) ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS="$2"; shift 2 ;;
    --archive-event-count-wait-mode) ARCHIVE_EVENT_COUNT_WAIT_MODE="$2"; shift 2 ;;
    --archive-readback-mode) ARCHIVE_READBACK_MODE="$2"; shift 2 ;;
    --raw-blob-preallocate) RAW_BLOB_PREALLOCATE="$2"; shift 2 ;;
    --raw-blob-async-write) RAW_BLOB_ASYNC_WRITE="$2"; shift 2 ;;
    --raw-blob-async-close) RAW_BLOB_ASYNC_CLOSE="$2"; shift 2 ;;
    --raw-blob-async-publish) RAW_BLOB_ASYNC_PUBLISH="$2"; shift 2 ;;
    --raw-blob-async-publish-threads) RAW_BLOB_ASYNC_PUBLISH_THREADS="$2"; shift 2 ;;
    --raw-blob-publish-before-close) RAW_BLOB_PUBLISH_BEFORE_CLOSE="$2"; shift 2 ;;
    --raw-blob-fast-reserve) RAW_BLOB_FAST_RESERVE="$2"; shift 2 ;;
    --raw-blob-sidecar-meta) RAW_BLOB_SIDECAR_META="$2"; shift 2 ;;
    --archive-range-event-count) ARCHIVE_RANGE_EVENT_COUNT="$2"; shift 2 ;;
    --hdf5-archive-reader-bin) HDF5_ARCHIVE_READER_BIN="$2"; shift 2 ;;
    --chrono-bench-barrier) CHRONO_BENCH_BARRIER="$2"; shift 2 ;;
    --chrono-bench-shared-story) CHRONO_BENCH_SHARED_STORY="$2"; shift 2 ;;
    --chronolog-producer-outstanding|--producer-outstanding) CHRONOLOG_PRODUCER_OUTSTANDING="$2"; shift 2 ;;
    --chronolog-producer-batch-size|--producer-batch-size) CHRONOLOG_PRODUCER_BATCH_SIZE="$2"; shift 2 ;;
    --chronolog-producer-wait-mode|--producer-wait-mode) CHRONOLOG_PRODUCER_WAIT_MODE="$2"; shift 2 ;;
    --chronolog-bench-owned-batch) CHRONOLOG_BENCH_OWNED_BATCH="$2"; shift 2 ;;
    --chronolog-bench-owned-batch-min-message-size) CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE="$2"; shift 2 ;;
    --chronolog-client-batch-keeper-selection) CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION="$2"; shift 2 ;;
    --chronolog-client-keeper-time-bucket-ns) CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS="$2"; shift 2 ;;
    --chronolog-client-parallel-tail-rpc) CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-drain) CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-drain-max-batches) CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-packed-batch) CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-metadata-only-output) CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-packed-bulk) CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-packed-bulk-stream) CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-packed-bulk-stream-max-batches) CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES="$2"; shift 2 ;;
    --chronolog-client-keeper-cursor-packed-bulk-buffer-bytes) CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES="$2"; shift 2 ;;
    --chronolog-keeper-tail-packed-bulk-uninitialized-buffer) CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER="$2"; shift 2 ;;
    --chronolog-keeper-tail-packed-bulk-chunk-bytes) CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES="$2"; shift 2 ;;
    --chronolog-keeper-tail-packed-bulk-parallel-transfer) CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER="$2"; shift 2 ;;
    --chronolog-mixed-tail-read-mode) CHRONOLOG_MIXED_TAIL_READ_MODE="$2"; shift 2 ;;
    --chronolog-mixed-tail-reader-start-mode) CHRONOLOG_MIXED_TAIL_READER_START_MODE="$2"; shift 2 ;;
    --chronolog-mixed-tail-writer-release-story) CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY="$2"; shift 2 ;;
    --chronolog-client-execution-mode) CHRONOLOG_CLIENT_EXECUTION_MODE="$2"; shift 2 ;;
    --chronicle-count) CHRONICLE_COUNT="$2"; shift 2 ;;
    --story-count) STORY_COUNT="$2"; shift 2 ;;
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    --profile-mode) PROFILE_MODE="$2"; shift 2 ;;
    --keeper-journal-shards) KEEPER_JOURNAL_SHARDS="$2"; shift 2 ;;
    --keeper-journal-shard-policy) KEEPER_JOURNAL_SHARD_POLICY="$2"; shift 2 ;;
    --keeper-journal-placement) KEEPER_JOURNAL_PLACEMENT="$2"; shift 2 ;;
    --keeper-journal-local-base) KEEPER_JOURNAL_LOCAL_BASE="$2"; shift 2 ;;
    --keeper-journal-tail-read-mode) KEEPER_JOURNAL_TAIL_READ_MODE="$2"; shift 2 ;;
    --keeper-journal-tail-direct-read-into-events) KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS="$2"; shift 2 ;;
    --keeper-tail-batch-max-events) KEEPER_TAIL_BATCH_MAX_EVENTS="$2"; shift 2 ;;
    --keeper-tail-batch-max-bytes) KEEPER_TAIL_BATCH_MAX_BYTES="$2"; shift 2 ;;
    --keeper-journal-fdatasync-batch-events) KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS="$2"; shift 2 ;;
    --keeper-journal-batch-writev) KEEPER_JOURNAL_BATCH_WRITEV="$2"; shift 2 ;;
    --keeper-journal-move-batch-payloads) KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS="$2"; shift 2 ;;
    --keeper-journal-group-commit-flush-events) KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS="$2"; shift 2 ;;
    --keeper-journal-group-commit-flush-bytes) KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES="$2"; shift 2 ;;
    --keeper-journal-group-commit-strict-flush-event-cap) KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP="$2"; shift 2 ;;
    --keeper-journal-group-commit-large-payload-bytes) KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES="$2"; shift 2 ;;
    --keeper-journal-group-commit-large-payload-flush-events) KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS="$2"; shift 2 ;;
    --keeper-journal-group-commit-wait-us) KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US="$2"; shift 2 ;;
    --keeper-journal-group-commit-flush-wait-us) KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US="$2"; shift 2 ;;
    --keeper-journal-group-commit-queue-boundary-min-events) KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS="$2"; shift 2 ;;
    --keeper-journal-group-commit-queue-boundary-min-payload-bytes) KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES="$2"; shift 2 ;;
    --keeper-journal-group-commit-queue-boundary-wait-every-n) KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N="$2"; shift 2 ;;
    --keeper-journal-group-commit-wait-for-active-admissions) KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS="$2"; shift 2 ;;
	    --keeper-journal-owner-drain-yields) KEEPER_JOURNAL_OWNER_DRAIN_YIELDS="$2"; shift 2 ;;
	    --keeper-journal-notify-owner-only-on-empty) KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY="$2"; shift 2 ;;
	    --keeper-append-stats-interval-events) KEEPER_APPEND_STATS_INTERVAL_EVENTS="$2"; shift 2 ;;
	    --keeper-journal-async-callback-dispatch) KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH="$2"; shift 2 ;;
	    --keeper-journal-async-batch-completion-dispatch) KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH="$2"; shift 2 ;;
	    --keeper-journal-callback-dispatch-threads) KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS="$2"; shift 2 ;;
	    --keeper-journal-callback-batch-drain) KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN="$2"; shift 2 ;;
    --keeper-journal-callback-batch-drain-max) KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX="$2"; shift 2 ;;
    --keeper-journal-callback-batch-drain-min-payload-bytes) KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES="$2"; shift 2 ;;
    --keeper-journal-durable-complete-before-publish) KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH="$2"; shift 2 ;;
    --keeper-journal-async-drain-threads) KEEPER_JOURNAL_ASYNC_DRAIN_THREADS="$2"; shift 2 ;;
    --keeper-recording-margo-xstreams) KEEPER_RECORDING_MARGO_XSTREAMS="$2"; shift 2 ;;
    --keeper-recording-margo-progress-thread) KEEPER_RECORDING_MARGO_PROGRESS_THREAD="$2"; shift 2 ;;
    --keeper-recording-margo-handlers) KEEPER_RECORDING_MARGO_HANDLERS="$2"; shift 2 ;;
    --grapher-recording-margo-xstreams) GRAPHER_RECORDING_MARGO_XSTREAMS="$2"; shift 2 ;;
    --grapher-recording-margo-handlers) GRAPHER_RECORDING_MARGO_HANDLERS="$2"; shift 2 ;;
    --grapher-direct-deserialize) GRAPHER_DIRECT_DESERIALIZE="$2"; shift 2 ;;
    --keeper-drain-margo-progress-thread) KEEPER_DRAIN_MARGO_PROGRESS_THREAD="$2"; shift 2 ;;
    --keeper-drain-margo-rpc-threads) KEEPER_DRAIN_MARGO_RPC_THREADS="$2"; shift 2 ;;
    --keeper-extraction-threads) KEEPER_EXTRACTION_THREADS="$2"; shift 2 ;;
    --keeper-direct-serialize) KEEPER_DIRECT_SERIALIZE="$2"; shift 2 ;;
    --keeper-fast-wire) KEEPER_FAST_WIRE="$2"; shift 2 ;;
    --keeper-wal-drain-batch-events) KEEPER_WAL_DRAIN_BATCH_EVENTS="$2"; shift 2 ;;
    --keeper-wal-drain-batch-wait-us) KEEPER_WAL_DRAIN_BATCH_WAIT_US="$2"; shift 2 ;;
    --keeper-data-collection-streams) KEEPER_DATA_COLLECTION_STREAMS="$2"; shift 2 ;;
    --keeper-data-collection-threads-per-stream) KEEPER_DATA_COLLECTION_THREADS_PER_STREAM="$2"; shift 2 ;;
    --grapher-data-collection-streams) GRAPHER_DATA_COLLECTION_STREAMS="$2"; shift 2 ;;
    --grapher-data-collection-threads-per-stream) GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM="$2"; shift 2 ;;
    --keeper-stop-story-flush-drain) KEEPER_STOP_STORY_FLUSH_DRAIN="$2"; shift 2 ;;
    --keeper-stop-story-flush-drain-timeout-ms) KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS="$2"; shift 2 ;;
    --keeper-restart-pre-crash-stats-wait-seconds) KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS="$2"; shift 2 ;;
    --visor-parallel-keeper-stop) VISOR_PARALLEL_KEEPER_STOP="$2"; shift 2 ;;
    --visor-parallel-recording-stop) VISOR_PARALLEL_RECORDING_STOP="$2"; shift 2 ;;
    --perf-bin) PERF_BIN="$2"; shift 2 ;;
    --perf-call-graph) PERF_CALL_GRAPH="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ "${NODE_COUNT}" -lt 2 ]]; then
  echo "Distributed ChronoLog append requires at least 2 nodes" >&2
  exit 2
fi

INSTALL_DIR="$(readlink -f "${INSTALL_DIR}")"
if [[ ! -d "${INSTALL_DIR}" ]]; then
  echo "ChronoLog install directory not found: ${INSTALL_DIR}" >&2
  exit 1
fi

case "${PROFILE_MODE}" in
  none|tau|gperftools|darshan|perf|strace|ebpf) ;;
  *) echo "Unsupported profile mode: ${PROFILE_MODE}" >&2; exit 2 ;;
esac
if [[ "${PROFILE_MODE}" != "none" && "${STARTUP_SLEEP_SECONDS}" -lt 10 ]]; then
  STARTUP_SLEEP_SECONDS=10
fi
if [[ "${CLIENT_COUNT}" -ne 1 && ! "${WORKFLOW}" =~ ^(append_throughput|mixed_append_tail|keeper_restart_recovery|archive_range_retrieval)$ ]]; then
  echo "ChronoLog client-count ${CLIENT_COUNT} requested for ${WORKFLOW}, but multi-client execution is currently implemented only for append_throughput, mixed_append_tail, keeper_restart_recovery, and archive_range_retrieval" >&2
  exit 2
fi
if [[ "${WORKFLOW}" == "append_durable" && "${COMPLETION_MODE}" != "archive_readback" ]]; then
  echo "append_durable requires --completion-mode archive_readback" >&2
  exit 2
fi
if [[ "${WORKFLOW}" == "archive_range_retrieval" && "${COMPLETION_MODE}" != "archive_readback" ]]; then
  echo "archive_range_retrieval requires --completion-mode archive_readback" >&2
  exit 2
fi
if [[ -z "${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}" && "${WORKFLOW}" == "append_durable" && "${COMPLETION_MODE}" == "archive_readback" ]]; then
  GRAPHER_INACTIVE_STORY_DELAY_SECONDS=3
fi
if [[ -z "${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}" && "${WORKFLOW}" == "archive_range_retrieval" && "${COMPLETION_MODE}" == "archive_readback" ]]; then
  GRAPHER_INACTIVE_STORY_DELAY_SECONDS=3
fi
if [[ -z "${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}" && "${WORKFLOW}" == "mixed_append_tail" ]]; then
  GRAPHER_INACTIVE_STORY_DELAY_SECONDS=3
fi
if [[ -z "${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}" && "${WORKFLOW}" == "keeper_restart_recovery" ]]; then
  GRAPHER_INACTIVE_STORY_DELAY_SECONDS=3
fi
if [[ "${PROFILE_MODE}" == "darshan" && "${STARTUP_SLEEP_SECONDS}" -lt 20 ]]; then
  STARTUP_SLEEP_SECONDS=20
fi
if [[ "${PROFILE_MODE}" == "perf" ]]; then
  if [[ -z "${PERF_BIN}" ]]; then
    PERF_BIN="$(command -v perf || true)"
  fi
  if [[ -z "${PERF_BIN}" || ! -x "${PERF_BIN}" ]]; then
    echo "profile-mode=perf requires a runnable perf binary; use --perf-bin or CHRONOLOG_PERF_BIN" >&2
    exit 2
  fi
  PERF_BIN="$(readlink -f "${PERF_BIN}")"
fi
EBPF_DURATION_SECONDS="${CHRONOLOG_EBPF_DURATION_SECONDS:-45}"

case "${CHRONO_BENCH_BARRIER}" in
  true|false) ;;
  *) echo "Unsupported --chrono-bench-barrier value: ${CHRONO_BENCH_BARRIER}" >&2; exit 2 ;;
esac
case "${CHRONO_BENCH_SHARED_STORY}" in
  true|false) ;;
  *) echo "Unsupported --chrono-bench-shared-story value: ${CHRONO_BENCH_SHARED_STORY}" >&2; exit 2 ;;
esac
if ! [[ "${CHRONOLOG_PRODUCER_OUTSTANDING}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--chronolog-producer-outstanding must be a positive integer: ${CHRONOLOG_PRODUCER_OUTSTANDING}" >&2
  exit 2
fi
if ! [[ "${CHRONOLOG_PRODUCER_BATCH_SIZE}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--chronolog-producer-batch-size must be a positive integer: ${CHRONOLOG_PRODUCER_BATCH_SIZE}" >&2
  exit 2
fi
case "${CHRONOLOG_PRODUCER_WAIT_MODE}" in
  auto|per_event|bounded_outstanding|bounded_futures|bounded_api|bounded_per_keeper|after_loop) ;;
  *)
    echo "--chronolog-producer-wait-mode must be auto, per_event, bounded_outstanding, bounded_futures, bounded_api, bounded_per_keeper, or after_loop: ${CHRONOLOG_PRODUCER_WAIT_MODE}" >&2
    exit 2
    ;;
esac
case "${CHRONOLOG_BENCH_OWNED_BATCH}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-bench-owned-batch value: ${CHRONOLOG_BENCH_OWNED_BATCH}" >&2; exit 2 ;;
esac
if ! [[ "${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}" =~ ^[0-9]+$ ]]; then
  echo "--chronolog-bench-owned-batch-min-message-size must be a non-negative integer: ${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}" >&2
  exit 2
fi
case "${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}" in
  per_event|single_keeper) ;;
  *) echo "Unsupported --chronolog-client-batch-keeper-selection value: ${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}. Expected per_event or single_keeper." >&2; exit 2 ;;
esac
if [[ "${CHRONO_BENCH_BARRIER}" == "true" && "${CHRONOLOG_PRODUCER_OUTSTANDING}" -gt 1 ]]; then
  echo "--chronolog-producer-outstanding > 1 is incompatible with --chrono-bench-barrier true" >&2
  exit 2
fi
case "${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-client-parallel-tail-rpc value: ${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}" >&2; exit 2 ;;
esac
case "${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-client-keeper-cursor-drain value: ${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN}" >&2; exit 2 ;;
esac
case "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-client-keeper-cursor-packed-batch value: ${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH}" >&2; exit 2 ;;
esac
case "${CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-client-keeper-cursor-metadata-only-output value: ${CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT}" >&2; exit 2 ;;
esac
case "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-client-keeper-cursor-packed-bulk value: ${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK}" >&2; exit 2 ;;
esac
case "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-client-keeper-cursor-packed-bulk-stream value: ${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM}" >&2; exit 2 ;;
esac
if ! [[ "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES}" =~ ^[0-9]+$ ]]; then
  echo "Unsupported --chronolog-client-keeper-cursor-packed-bulk-stream-max-batches value: ${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES}" >&2
  exit 2
fi
case "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-keeper-tail-packed-bulk-uninitialized-buffer value: ${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER}" >&2; exit 2 ;;
esac
if ! [[ "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES}" =~ ^[0-9]+$ ]]; then
  echo "Unsupported --chronolog-keeper-tail-packed-bulk-chunk-bytes value: ${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES}" >&2
  exit 2
fi
case "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --chronolog-keeper-tail-packed-bulk-parallel-transfer value: ${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER}" >&2; exit 2 ;;
esac
case "${CHRONOLOG_MIXED_TAIL_READ_MODE}" in
  full|incremental|keeper_cursor|keeper_cursor_packed) ;;
  *) echo "Unsupported --chronolog-mixed-tail-read-mode value: ${CHRONOLOG_MIXED_TAIL_READ_MODE}" >&2; exit 2 ;;
esac
if [[ "${WORKFLOW}" == "mixed_append_tail" && "${CHRONOLOG_MIXED_TAIL_READ_MODE}" =~ ^keeper_cursor && ! "${COMPLETION_MODE}" =~ ^keeper_journal_ ]]; then
  echo "mixed_append_tail keeper_cursor modes require a keeper_journal_* completion mode; live_return does not populate the Keeper cursor tail source" >&2
  exit 2
fi
case "${CHRONOLOG_MIXED_TAIL_READER_START_MODE}" in
  ready|after_writers_done) ;;
  *) echo "Unsupported --chronolog-mixed-tail-reader-start-mode value: ${CHRONOLOG_MIXED_TAIL_READER_START_MODE}" >&2; exit 2 ;;
esac
case "${CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY}" in
  yes|no|after_reader) ;;
  *) echo "Unsupported --chronolog-mixed-tail-writer-release-story value: ${CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY}" >&2; exit 2 ;;
esac
case "${KEEPER_JOURNAL_PLACEMENT}" in
  shared|node_local) ;;
  *) echo "Unsupported --keeper-journal-placement value: ${KEEPER_JOURNAL_PLACEMENT}" >&2; exit 2 ;;
esac
case "${KEEPER_JOURNAL_SHARD_POLICY}" in
  mixed|client|story_client|story-client|story) ;;
  *) echo "Unsupported --keeper-journal-shard-policy value: ${KEEPER_JOURNAL_SHARD_POLICY}" >&2; exit 2 ;;
esac
if [[ "${KEEPER_JOURNAL_PLACEMENT}" == "node_local" ]]; then
  if [[ -z "${KEEPER_JOURNAL_LOCAL_BASE}" ]]; then
    echo "--keeper-journal-local-base is required when --keeper-journal-placement node_local; do not rely on /tmp/tmpfs for durability evidence" >&2
    exit 2
  fi
  case "${KEEPER_JOURNAL_LOCAL_BASE}" in
    /tmp|/tmp/*|/dev/shm|/dev/shm/*)
      echo "--keeper-journal-local-base must not be tmpfs-backed (${KEEPER_JOURNAL_LOCAL_BASE}); choose real local NVMe/SSD or an explicitly classified shared path" >&2
      exit 2
      ;;
  esac
fi
case "${HDF5_ARCHIVE_ATOMIC_RENAME}" in
  0|1) ;;
  *) echo "Unsupported --hdf5-archive-atomic-rename value: ${HDF5_ARCHIVE_ATOMIC_RENAME}" >&2; exit 2 ;;
esac
if ! [[ "${GRAPHER_EXTRACTION_THREADS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--grapher-extraction-threads must be a positive integer: ${GRAPHER_EXTRACTION_THREADS}" >&2
  exit 2
fi
if ! [[ "${HDF5_ARCHIVE_CHUNK_EVENTS}" =~ ^[0-9]+$ ]]; then
  echo "--hdf5-archive-chunk-events must be a non-negative integer: ${HDF5_ARCHIVE_CHUNK_EVENTS}" >&2
  exit 2
fi
case "${HDF5_ARCHIVE_LAYOUT}" in
  vlen|blob_map|raw_blob|fixed_record) ;;
  *) echo "Unsupported --hdf5-archive-layout value: ${HDF5_ARCHIVE_LAYOUT}" >&2; exit 2 ;;
esac
case "${ARCHIVE_EVENT_COUNT_WAIT_MODE}" in
  inline|parallel_process) ;;
  *) echo "Unsupported --archive-event-count-wait-mode value: ${ARCHIVE_EVENT_COUNT_WAIT_MODE}" >&2; exit 2 ;;
esac
case "${ARCHIVE_READBACK_MODE}" in
  inline|parallel_thread) ;;
  *) echo "Unsupported --archive-readback-mode value: ${ARCHIVE_READBACK_MODE}" >&2; exit 2 ;;
esac
case "${RAW_BLOB_PREALLOCATE}" in
  0|1) ;;
  *) echo "Unsupported --raw-blob-preallocate value: ${RAW_BLOB_PREALLOCATE}" >&2; exit 2 ;;
esac
case "${RAW_BLOB_ASYNC_WRITE}" in
  0|1) ;;
  *) echo "Unsupported --raw-blob-async-write value: ${RAW_BLOB_ASYNC_WRITE}" >&2; exit 2 ;;
esac
case "${RAW_BLOB_ASYNC_CLOSE}" in
  0|1) ;;
  *) echo "Unsupported --raw-blob-async-close value: ${RAW_BLOB_ASYNC_CLOSE}" >&2; exit 2 ;;
esac
case "${RAW_BLOB_ASYNC_PUBLISH}" in
  0|1) ;;
  *) echo "Unsupported --raw-blob-async-publish value: ${RAW_BLOB_ASYNC_PUBLISH}" >&2; exit 2 ;;
esac
case "${RAW_BLOB_PUBLISH_BEFORE_CLOSE}" in
  0|1) ;;
  *) echo "Unsupported --raw-blob-publish-before-close value: ${RAW_BLOB_PUBLISH_BEFORE_CLOSE}" >&2; exit 2 ;;
esac
case "${RAW_BLOB_FAST_RESERVE}" in
  0|1) ;;
  *) echo "Unsupported --raw-blob-fast-reserve value: ${RAW_BLOB_FAST_RESERVE}" >&2; exit 2 ;;
esac
case "${RAW_BLOB_SIDECAR_META}" in
  0|1) ;;
  *) echo "Unsupported --raw-blob-sidecar-meta value: ${RAW_BLOB_SIDECAR_META}" >&2; exit 2 ;;
esac
if ! [[ "${RAW_BLOB_ASYNC_PUBLISH_THREADS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--raw-blob-async-publish-threads must be a positive integer: ${RAW_BLOB_ASYNC_PUBLISH_THREADS}" >&2
  exit 2
fi
case "${KEEPER_FAST_WIRE}" in
  0|1) ;;
  *) echo "Unsupported --keeper-fast-wire value: ${KEEPER_FAST_WIRE}" >&2; exit 2 ;;
esac
if ! [[ "${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES}" =~ ^[0-9]+$ ]]; then
  echo "--keeper-journal-group-commit-flush-bytes must be a non-negative integer: ${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES}" >&2
  exit 2
fi
case "${KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP}" in
  0|1|true|false|TRUE|FALSE|yes|no|YES|NO) ;;
  *) echo "--keeper-journal-group-commit-strict-flush-event-cap must be boolean-like: ${KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP}" >&2; exit 2 ;;
esac
if ! [[ "${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES}" =~ ^[0-9]+$ ]]; then
  echo "--keeper-journal-group-commit-large-payload-bytes must be a non-negative integer: ${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES}" >&2
  exit 2
fi
if ! [[ "${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--keeper-journal-group-commit-large-payload-flush-events must be a positive integer: ${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS}" >&2
  exit 2
fi
if ! [[ "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS}" =~ ^[0-9]+$ ]]; then
  echo "--keeper-journal-group-commit-queue-boundary-min-events must be a non-negative integer: ${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS}" >&2
  exit 2
fi
if ! [[ "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES}" =~ ^[0-9]+$ ]]; then
  echo "--keeper-journal-group-commit-queue-boundary-min-payload-bytes must be a non-negative integer: ${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES}" >&2
  exit 2
fi
if ! [[ "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--keeper-journal-group-commit-queue-boundary-wait-every-n must be a positive integer: ${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N}" >&2
  exit 2
fi
case "${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "--keeper-journal-group-commit-wait-for-active-admissions must be boolean-like: ${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS}" >&2; exit 2 ;;
esac
case "${KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY}" in
  0|1|true|false|on|off|yes|no) ;;
  *) echo "Unsupported --keeper-journal-notify-owner-only-on-empty value: ${KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY}" >&2; exit 2 ;;
esac
CHRONO_BENCH_EXTRA_ARGS=()
if [[ "${CHRONO_BENCH_BARRIER}" == "true" ]]; then
  CHRONO_BENCH_EXTRA_ARGS+=("-y")
fi
if [[ "${CHRONO_BENCH_SHARED_STORY}" == "true" ]]; then
  CHRONO_BENCH_EXTRA_ARGS+=("-o")
fi
case "${CHRONOLOG_BENCH_OWNED_BATCH}" in
  1|true|on|yes) CHRONO_BENCH_EXTRA_ARGS+=("-O") ;;
esac
if [[ "${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}" != "0" ]]; then
  CHRONO_BENCH_EXTRA_ARGS+=("--producer_owned_batch_min_event_size" "${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}")
fi
if [[ "${CHRONOLOG_PRODUCER_WAIT_MODE}" == "bounded_futures" ]]; then
  CHRONO_BENCH_EXTRA_ARGS+=("--producer_bound_keeper_futures")
fi
CHRONO_BENCH_EXTRA_ARGS+=("-q" "${CHRONOLOG_PRODUCER_OUTSTANDING}" "-z" "${CHRONOLOG_PRODUCER_BATCH_SIZE}")

RESULT_DIR="$(make_result_dir "${RESULT_DIR}")"
reject_tmpfs_storage_path "--result-dir" "${RESULT_DIR}"
export PHASE0_RESULT_DIR="${RESULT_DIR}"
export CHRONOLOG_CLIENT_MPI_OVERSUBSCRIBE="${CLIENT_MPI_OVERSUBSCRIBE}"
export CHRONOLOG_PRODUCER_OUTSTANDING="${CHRONOLOG_PRODUCER_OUTSTANDING}"
export CHRONOLOG_PRODUCER_BATCH_SIZE="${CHRONOLOG_PRODUCER_BATCH_SIZE}"
export CHRONOLOG_PRODUCER_WAIT_MODE="${CHRONOLOG_PRODUCER_WAIT_MODE}"
export CHRONOLOG_BENCH_OWNED_BATCH="${CHRONOLOG_BENCH_OWNED_BATCH}"
export CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE="${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}"
export CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION="${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}"
export CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS="${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS}"
export CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC="${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN="${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES="${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT="${CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES}"
export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES="${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES}"
export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER="${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER}"
export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES="${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES}"
export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER="${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER}"
export CHRONOLOG_CLIENT_EXECUTION_MODE="${CHRONOLOG_CLIENT_EXECUTION_MODE}"
export CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS="${CLIENT_APPEND_STATS_INTERVAL_EVENTS}"

if [[ -z "${SLURM_JOB_ID:-}" && "${INSIDE_ALLOCATION}" != "1" ]]; then
  SALLOC_NODELIST_ARGS=()
  if [[ -n "${NODELIST}" ]]; then
    SALLOC_NODELIST_ARGS=(--nodelist="${NODELIST}")
  fi
  RECURSIVE_ARGS=(
    --partition "${PARTITION}"
    --node-count "${NODE_COUNT}"
    --client-count "${CLIENT_COUNT}"
    --client-mpi-oversubscribe "${CLIENT_MPI_OVERSUBSCRIBE}"
    --slurm-time "${SLURM_TIME}"
    --record-groups "${RECORD_GROUPS}"
    --workflow "${WORKFLOW}"
    --completion-mode "${COMPLETION_MODE}"
    --operation-count "${OPERATION_COUNT}"
    --message-size-bytes "${MESSAGE_SIZE_BYTES}"
    --data-collection-poll-interval-us "${DATA_COLLECTION_POLL_INTERVAL_US}"
    --grapher-inactive-story-delay-seconds "${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}"
    --grapher-retire-on-stop "${GRAPHER_RETIRE_ON_STOP}"
    --grapher-stop-story-archive-drain "${GRAPHER_STOP_STORY_ARCHIVE_DRAIN}"
    --grapher-stop-story-archive-drain-timeout-ms "${GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS}"
    --grapher-stop-drain-wait-outside-lock "${GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK}"
    --hdf5-archive-atomic-rename "${HDF5_ARCHIVE_ATOMIC_RENAME}"
    --hdf5-archive-chunk-events "${HDF5_ARCHIVE_CHUNK_EVENTS}"
    --hdf5-archive-layout "${HDF5_ARCHIVE_LAYOUT}"
    --archive-event-count-poll-interval-seconds "${ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS}"
    --archive-event-count-wait-mode "${ARCHIVE_EVENT_COUNT_WAIT_MODE}"
    --archive-readback-mode "${ARCHIVE_READBACK_MODE}"
    --raw-blob-preallocate "${RAW_BLOB_PREALLOCATE}"
    --raw-blob-async-write "${RAW_BLOB_ASYNC_WRITE}"
    --raw-blob-async-close "${RAW_BLOB_ASYNC_CLOSE}"
    --raw-blob-async-publish "${RAW_BLOB_ASYNC_PUBLISH}"
    --raw-blob-async-publish-threads "${RAW_BLOB_ASYNC_PUBLISH_THREADS}"
    --raw-blob-publish-before-close "${RAW_BLOB_PUBLISH_BEFORE_CLOSE}"
    --raw-blob-fast-reserve "${RAW_BLOB_FAST_RESERVE}"
    --raw-blob-sidecar-meta "${RAW_BLOB_SIDECAR_META}"
    --chrono-bench-barrier "${CHRONO_BENCH_BARRIER}"
    --chrono-bench-shared-story "${CHRONO_BENCH_SHARED_STORY}"
    --chronolog-producer-outstanding "${CHRONOLOG_PRODUCER_OUTSTANDING}"
    --chronolog-producer-batch-size "${CHRONOLOG_PRODUCER_BATCH_SIZE}"
    --chronolog-producer-wait-mode "${CHRONOLOG_PRODUCER_WAIT_MODE}"
    --chronolog-bench-owned-batch "${CHRONOLOG_BENCH_OWNED_BATCH}"
    --chronolog-bench-owned-batch-min-message-size "${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}"
    --chronolog-client-batch-keeper-selection "${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}"
    --chronolog-client-keeper-time-bucket-ns "${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS}"
    --chronolog-client-parallel-tail-rpc "${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}"
    --chronolog-client-keeper-cursor-drain "${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN}"
    --chronolog-client-keeper-cursor-drain-max-batches "${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES}"
    --chronolog-client-keeper-cursor-packed-batch "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH}"
    --chronolog-client-keeper-cursor-metadata-only-output "${CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT}"
    --chronolog-client-keeper-cursor-packed-bulk "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK}"
    --chronolog-client-keeper-cursor-packed-bulk-stream "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM}"
    --chronolog-client-keeper-cursor-packed-bulk-stream-max-batches "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES}"
    --chronolog-client-keeper-cursor-packed-bulk-buffer-bytes "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES}"
    --chronolog-keeper-tail-packed-bulk-uninitialized-buffer "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER}"
    --chronolog-keeper-tail-packed-bulk-chunk-bytes "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES}"
    --chronolog-keeper-tail-packed-bulk-parallel-transfer "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER}"
    --chronolog-mixed-tail-read-mode "${CHRONOLOG_MIXED_TAIL_READ_MODE}"
    --chronolog-mixed-tail-reader-start-mode "${CHRONOLOG_MIXED_TAIL_READER_START_MODE}"
    --chronolog-mixed-tail-writer-release-story "${CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY}"
    --chronolog-client-execution-mode "${CHRONOLOG_CLIENT_EXECUTION_MODE}"
    --chronicle-count "${CHRONICLE_COUNT}"
    --story-count "${STORY_COUNT}"
    --install-dir "${INSTALL_DIR}"
    --profile-mode "${PROFILE_MODE}"
    --keeper-journal-shards "${KEEPER_JOURNAL_SHARDS}"
    --keeper-journal-shard-policy "${KEEPER_JOURNAL_SHARD_POLICY}"
    --keeper-journal-placement "${KEEPER_JOURNAL_PLACEMENT}"
    --keeper-journal-local-base "${KEEPER_JOURNAL_LOCAL_BASE}"
    --keeper-journal-tail-read-mode "${KEEPER_JOURNAL_TAIL_READ_MODE}"
    --keeper-journal-tail-direct-read-into-events "${KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS}"
    --keeper-tail-batch-max-events "${KEEPER_TAIL_BATCH_MAX_EVENTS}"
    --keeper-tail-batch-max-bytes "${KEEPER_TAIL_BATCH_MAX_BYTES}"
    --keeper-journal-fdatasync-batch-events "${KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS}"
    --keeper-journal-batch-writev "${KEEPER_JOURNAL_BATCH_WRITEV}"
    --keeper-journal-move-batch-payloads "${KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS}"
    --keeper-journal-group-commit-flush-events "${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS}"
    --keeper-journal-group-commit-flush-bytes "${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES}"
    --keeper-journal-group-commit-strict-flush-event-cap "${KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP}"
    --keeper-journal-group-commit-large-payload-bytes "${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES}"
    --keeper-journal-group-commit-large-payload-flush-events "${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS}"
    --keeper-journal-group-commit-wait-us "${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US}"
    --keeper-journal-group-commit-flush-wait-us "${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US}"
	    --keeper-journal-group-commit-queue-boundary-min-events "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS}"
	    --keeper-journal-group-commit-queue-boundary-min-payload-bytes "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES}"
	    --keeper-journal-group-commit-queue-boundary-wait-every-n "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N}"
	    --keeper-journal-group-commit-wait-for-active-admissions "${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS}"
	    --keeper-journal-owner-drain-yields "${KEEPER_JOURNAL_OWNER_DRAIN_YIELDS}"
	    --keeper-journal-notify-owner-only-on-empty "${KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY}"
	    --keeper-journal-async-callback-dispatch "${KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH}"
	    --keeper-journal-async-batch-completion-dispatch "${KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH}"
	    --keeper-journal-callback-dispatch-threads "${KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS}"
	    --keeper-journal-callback-batch-drain "${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN}"
	    --keeper-journal-callback-batch-drain-max "${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX}"
	    --keeper-journal-callback-batch-drain-min-payload-bytes "${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES}"
	    --keeper-journal-durable-complete-before-publish "${KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH}"
	    --keeper-journal-async-drain-threads "${KEEPER_JOURNAL_ASYNC_DRAIN_THREADS}"
    --keeper-recording-margo-xstreams "${KEEPER_RECORDING_MARGO_XSTREAMS}"
    --keeper-recording-margo-progress-thread "${KEEPER_RECORDING_MARGO_PROGRESS_THREAD}"
    --keeper-recording-margo-handlers "${KEEPER_RECORDING_MARGO_HANDLERS}"
    --grapher-recording-margo-xstreams "${GRAPHER_RECORDING_MARGO_XSTREAMS}"
    --grapher-recording-margo-handlers "${GRAPHER_RECORDING_MARGO_HANDLERS}"
    --grapher-direct-deserialize "${GRAPHER_DIRECT_DESERIALIZE}"
    --keeper-drain-margo-progress-thread "${KEEPER_DRAIN_MARGO_PROGRESS_THREAD}"
    --keeper-drain-margo-rpc-threads "${KEEPER_DRAIN_MARGO_RPC_THREADS}"
    --keeper-extraction-threads "${KEEPER_EXTRACTION_THREADS}"
    --keeper-direct-serialize "${KEEPER_DIRECT_SERIALIZE}"
    --keeper-fast-wire "${KEEPER_FAST_WIRE}"
    --keeper-wal-drain-batch-events "${KEEPER_WAL_DRAIN_BATCH_EVENTS}"
    --keeper-wal-drain-batch-wait-us "${KEEPER_WAL_DRAIN_BATCH_WAIT_US}"
    --keeper-data-collection-streams "${KEEPER_DATA_COLLECTION_STREAMS}"
    --keeper-data-collection-threads-per-stream "${KEEPER_DATA_COLLECTION_THREADS_PER_STREAM}"
    --grapher-data-collection-streams "${GRAPHER_DATA_COLLECTION_STREAMS}"
    --grapher-data-collection-threads-per-stream "${GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM}"
    --keeper-stop-story-flush-drain "${KEEPER_STOP_STORY_FLUSH_DRAIN}"
    --keeper-stop-story-flush-drain-timeout-ms "${KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS}"
    --visor-parallel-keeper-stop "${VISOR_PARALLEL_KEEPER_STOP}"
    --visor-parallel-recording-stop "${VISOR_PARALLEL_RECORDING_STOP}"
    --grapher-extraction-threads "${GRAPHER_EXTRACTION_THREADS}"
  )
  if [[ -n "${NODELIST}" ]]; then
    RECURSIVE_ARGS+=(--nodelist "${NODELIST}")
  fi
  if [[ -n "${PERF_BIN}" ]]; then
    RECURSIVE_ARGS+=(--perf-bin "${PERF_BIN}")
  fi
  RECURSIVE_ARGS+=(--perf-call-graph "${PERF_CALL_GRAPH}")
  SUBMIT_DIR="${RESULT_DIR}/slurm"
  mkdir -p "${SUBMIT_DIR}"
  BATCH_SCRIPT="${SUBMIT_DIR}/chronolog-recursive.sbatch.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'export CHRONOLOG_INSIDE_ALLOCATION=%q\n' 1
    printf 'export PHASE0_RESULT_DIR=%q\n' "${RESULT_DIR}"
    printf 'exec %q' "$0"
    printf ' %q' "${RECURSIVE_ARGS[@]}"
    printf '\n'
  } > "${BATCH_SCRIPT}"
  chmod +x "${BATCH_SCRIPT}"
  exec sbatch \
    --wait \
    --partition="${PARTITION}" \
    --nodes="${NODE_COUNT}" \
    "${SALLOC_NODELIST_ARGS[@]}" \
    --time="${SLURM_TIME}" \
    --job-name=phase0-chronolog \
    --output="${SUBMIT_DIR}/sbatch.stdout.log" \
    --error="${SUBMIT_DIR}/sbatch.stderr.log" \
    "${BATCH_SCRIPT}"
fi

CONFIG_DIR="${RESULT_DIR}/config"
CHRONOLOG_RESULT_DIR="${RESULT_DIR}/chronolog"
CHRONOLOG_LOG_DIR="${CHRONOLOG_RESULT_DIR}/logs"
CHRONOLOG_OUTPUT_DIR="${CHRONOLOG_RESULT_DIR}/output"
CHRONOLOG_RUNTIME_TMP_DIR="${CHRONOLOG_RESULT_DIR}/runtime-tmp"
KEEPER_JOURNAL_MODE="${CHRONOLOG_KEEPER_JOURNAL_MODE:-disabled}"
KEEPER_JOURNAL_ACK_BEFORE_INGEST="${CHRONOLOG_KEEPER_JOURNAL_ACK_BEFORE_INGEST:-0}"
KEEPER_JOURNAL_ASYNC_DRAIN="${CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN:-0}"
KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE="${CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE:-memory}"
KEEPER_JOURNAL_SKIP_INGEST="${CHRONOLOG_KEEPER_JOURNAL_SKIP_INGEST:-0}"
KEEPER_JOURNAL_DEFER_RPC_RESPONSE="${CHRONOLOG_KEEPER_JOURNAL_DEFER_RPC_RESPONSE:-0}"
case "${COMPLETION_MODE}" in
  keeper_journal_buffered) KEEPER_JOURNAL_MODE="buffered" ;;
  keeper_journal_fdatasync) KEEPER_JOURNAL_MODE="fdatasync" ;;
  keeper_journal_fdatasync_tail_only)
    KEEPER_JOURNAL_MODE="fdatasync"
    KEEPER_JOURNAL_ACK_BEFORE_INGEST="1"
    KEEPER_JOURNAL_SKIP_INGEST="1"
    ;;
  keeper_journal_group_fdatasync) KEEPER_JOURNAL_MODE="fdatasync_batch" ;;
  keeper_journal_group_fdatasync_early_ack)
    KEEPER_JOURNAL_MODE="fdatasync_batch"
    KEEPER_JOURNAL_ACK_BEFORE_INGEST="1"
    ;;
  keeper_journal_group_fdatasync_async_drain)
    KEEPER_JOURNAL_MODE="fdatasync_batch"
    KEEPER_JOURNAL_ACK_BEFORE_INGEST="1"
    KEEPER_JOURNAL_ASYNC_DRAIN="1"
    KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE="memory"
    ;;
  keeper_journal_group_fdatasync_wal_drain)
    KEEPER_JOURNAL_MODE="fdatasync_batch"
    KEEPER_JOURNAL_ACK_BEFORE_INGEST="1"
    KEEPER_JOURNAL_ASYNC_DRAIN="1"
    KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE="wal"
    ;;
  keeper_journal_group_fdatasync_tail_only)
    KEEPER_JOURNAL_MODE="fdatasync_batch"
    KEEPER_JOURNAL_ACK_BEFORE_INGEST="1"
    KEEPER_JOURNAL_SKIP_INGEST="1"
    ;;
  keeper_journal_group_commit_tail_only)
    KEEPER_JOURNAL_MODE="fdatasync_batch"
    KEEPER_JOURNAL_ACK_BEFORE_INGEST="1"
    KEEPER_JOURNAL_SKIP_INGEST="1"
    KEEPER_JOURNAL_SINGLE_WRITER="1"
    KEEPER_JOURNAL_GROUP_COMMIT_WAIT="1"
    ;;
  keeper_journal_group_commit_deferred_tail_only)
    KEEPER_JOURNAL_MODE="fdatasync_batch"
    KEEPER_JOURNAL_ACK_BEFORE_INGEST="1"
    KEEPER_JOURNAL_SKIP_INGEST="1"
    KEEPER_JOURNAL_SINGLE_WRITER="1"
    KEEPER_JOURNAL_GROUP_COMMIT_WAIT="1"
    KEEPER_JOURNAL_DEFER_RPC_RESPONSE="1"
    ;;
esac
if [[ "${KEEPER_JOURNAL_ACK_BEFORE_INGEST}" == "1" && "${KEEPER_APPEND_STATS_WAIT_SECONDS}" -lt 30 ]]; then
  KEEPER_APPEND_STATS_WAIT_SECONDS=30
  export CHRONOLOG_KEEPER_APPEND_STATS_WAIT_SECONDS="${KEEPER_APPEND_STATS_WAIT_SECONDS}"
fi
if [[ "${KEEPER_JOURNAL_DEFER_RPC_RESPONSE}" == "1" && "${KEEPER_STOP_STORY_STATS_WAIT_MS}" -lt 2000 ]]; then
  KEEPER_STOP_STORY_STATS_WAIT_MS=2000
fi
if [[ "${KEEPER_JOURNAL_DEFER_RPC_RESPONSE}" == "1" && "${DEPLOY_STOP_TIMEOUT_SECONDS}" -lt 180 ]]; then
  DEPLOY_STOP_TIMEOUT_SECONDS=180
fi
if [[ "${WORKFLOW}" == "keeper_restart_recovery" && "${DEPLOY_STOP_TIMEOUT_SECONDS}" -lt 360 ]]; then
  DEPLOY_STOP_TIMEOUT_SECONDS=360
fi
if [[ "${WORKFLOW}" == "keeper_restart_recovery" && "${KEEPER_APPEND_STATS_INTERVAL_EVENTS}" -gt 1000 ]]; then
  KEEPER_APPEND_STATS_INTERVAL_EVENTS=1000
  export CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS="${KEEPER_APPEND_STATS_INTERVAL_EVENTS}"
fi
case "${KEEPER_JOURNAL_PLACEMENT}" in
  shared|node_local) ;;
  *) echo "Unsupported --keeper-journal-placement value: ${KEEPER_JOURNAL_PLACEMENT}" >&2; exit 2 ;;
esac
case "${KEEPER_JOURNAL_TAIL_READ_MODE}" in
  auto|direct|coalesced|vectored) ;;
  *) echo "Unsupported --keeper-journal-tail-read-mode value: ${KEEPER_JOURNAL_TAIL_READ_MODE}" >&2; exit 2 ;;
esac
case "${KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS}" in
  0|1) ;;
  *) echo "Unsupported --keeper-journal-tail-direct-read-into-events value: ${KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS}" >&2; exit 2 ;;
esac
KEEPER_JOURNAL_RUN_ID="${SLURM_JOB_ID:-manual}-$(basename "${RESULT_DIR}")"
if [[ "${KEEPER_JOURNAL_PLACEMENT}" == "node_local" ]]; then
  KEEPER_JOURNAL_DIR="${CHRONOLOG_KEEPER_JOURNAL_DIR:-${CHRONOLOG_RESULT_DIR}/keeper-journal-collected}"
else
  KEEPER_JOURNAL_DIR="${CHRONOLOG_KEEPER_JOURNAL_DIR:-${CHRONOLOG_RESULT_DIR}/keeper-journal}"
fi
KEEPER_TAIL_SOURCE="${CHRONOLOG_KEEPER_TAIL_SOURCE:-timeline}"
if [[ -z "${CHRONOLOG_KEEPER_TAIL_SOURCE:-}" ]]; then
  if [[ "${WORKFLOW}" == "keeper_restart_recovery" && "${COMPLETION_MODE}" == keeper_journal* ]]; then
    KEEPER_TAIL_SOURCE="journal"
  elif [[ "${WORKFLOW}" =~ ^(range_retrieval|mixed_append_tail)$ && "${COMPLETION_MODE}" == *tail_only ]]; then
    KEEPER_TAIL_SOURCE="journal"
  fi
fi
KEEPER_TAIL_CURSOR_OVERLAP_NS="${CHRONOLOG_KEEPER_TAIL_CURSOR_OVERLAP_NS:-${CHRONOLOG_MIXED_TAIL_OVERLAP_NS:-1000000000}}"
export CHRONOLOG_KEEPER_JOURNAL_MODE="${KEEPER_JOURNAL_MODE}"
export CHRONOLOG_KEEPER_JOURNAL_DIR="${KEEPER_JOURNAL_DIR}"
export CHRONOLOG_KEEPER_JOURNAL_PLACEMENT="${KEEPER_JOURNAL_PLACEMENT}"
export CHRONOLOG_KEEPER_JOURNAL_LOCAL_BASE="${KEEPER_JOURNAL_LOCAL_BASE}"
export CHRONOLOG_KEEPER_JOURNAL_LOCAL_PRECLEAN="${KEEPER_JOURNAL_LOCAL_PRECLEAN}"
export CHRONOLOG_KEEPER_JOURNAL_RUN_ID="${KEEPER_JOURNAL_RUN_ID}"
export CHRONOLOG_KEEPER_TAIL_SOURCE="${KEEPER_TAIL_SOURCE}"
export CHRONOLOG_KEEPER_JOURNAL_TAIL_READ_MODE="${KEEPER_JOURNAL_TAIL_READ_MODE}"
export CHRONOLOG_KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS="${KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS}"
export CHRONOLOG_KEEPER_TAIL_BATCH_MAX_EVENTS="${KEEPER_TAIL_BATCH_MAX_EVENTS}"
export CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES="${KEEPER_TAIL_BATCH_MAX_BYTES}"
export CHRONOLOG_KEEPER_TAIL_CURSOR_OVERLAP_NS="${KEEPER_TAIL_CURSOR_OVERLAP_NS}"
export CHRONOLOG_WORKFLOW="${WORKFLOW}"
export CHRONOLOG_COMPLETION_MODE="${COMPLETION_MODE}"
export CHRONOLOG_PROFILE_MODE="${PROFILE_MODE}"
export CHRONOLOG_MIXED_TAIL_READ_MODE="${CHRONOLOG_MIXED_TAIL_READ_MODE}"
export CHRONOLOG_MIXED_TAIL_READER_START_MODE="${CHRONOLOG_MIXED_TAIL_READER_START_MODE}"
export CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY="${CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY}"
export CHRONOLOG_PERF_EVENT="${PERF_EVENT}"
export CHRONOLOG_PERF_CALL_GRAPH="${PERF_CALL_GRAPH}"
export CHRONOLOG_KEEPER_JOURNAL_ACK_BEFORE_INGEST="${KEEPER_JOURNAL_ACK_BEFORE_INGEST}"
export CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN="${KEEPER_JOURNAL_ASYNC_DRAIN}"
export CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE="${KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE}"
export CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_THREADS="${KEEPER_JOURNAL_ASYNC_DRAIN_THREADS}"
export CHRONOLOG_KEEPER_JOURNAL_SKIP_INGEST="${KEEPER_JOURNAL_SKIP_INGEST}"
export CHRONOLOG_KEEPER_JOURNAL_DEFER_RPC_RESPONSE="${KEEPER_JOURNAL_DEFER_RPC_RESPONSE}"
export CHRONOLOG_KEEPER_JOURNAL_SHARDS="${KEEPER_JOURNAL_SHARDS}"
export CHRONOLOG_KEEPER_JOURNAL_SHARD_POLICY="${KEEPER_JOURNAL_SHARD_POLICY}"
export CHRONOLOG_KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS="${KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS}"
export CHRONOLOG_KEEPER_JOURNAL_WRITEV="${KEEPER_JOURNAL_WRITEV}"
export CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV="${KEEPER_JOURNAL_BATCH_WRITEV}"
export CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS="${KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS}"
export CHRONOLOG_KEEPER_JOURNAL_ATOMIC_OFFSETS="${KEEPER_JOURNAL_ATOMIC_OFFSETS}"
export CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER="${KEEPER_JOURNAL_SINGLE_WRITER}"
export CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS="${KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT="${KEEPER_JOURNAL_GROUP_COMMIT_WAIT}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS="${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES="${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP="${KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES="${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS="${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US="${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US="${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS="${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES="${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N="${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N}"
export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS="${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS}"
export CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS="${KEEPER_JOURNAL_OWNER_DRAIN_YIELDS}"
export CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY="${KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY}"
export CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH="${KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH}"
export CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH="${KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH}"
export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS="${KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS}"
export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN="${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN}"
export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX="${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX}"
export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES="${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES}"
export CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH="${KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH}"
export CHRONOLOG_STARTUP_READY_TIMEOUT_SECONDS_EFFECTIVE="${STARTUP_READY_TIMEOUT_SECONDS}"
export CHRONOLOG_DEPLOY_STOP_TIMEOUT_SECONDS_EFFECTIVE="${DEPLOY_STOP_TIMEOUT_SECONDS}"
export CHRONOLOG_DATA_COLLECTION_POLL_INTERVAL_US="${DATA_COLLECTION_POLL_INTERVAL_US}"
export CHRONOLOG_ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS="${ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS}"
export CHRONOLOG_ARCHIVE_EVENT_COUNT_WAIT_MODE="${ARCHIVE_EVENT_COUNT_WAIT_MODE}"
export CHRONOLOG_ARCHIVE_READBACK_MODE="${ARCHIVE_READBACK_MODE}"
export CHRONOLOG_KEEPER_DATA_COLLECTION_STREAMS="${KEEPER_DATA_COLLECTION_STREAMS}"
export CHRONOLOG_KEEPER_DATA_COLLECTION_THREADS_PER_STREAM="${KEEPER_DATA_COLLECTION_THREADS_PER_STREAM}"
export CHRONOLOG_GRAPHER_DATA_COLLECTION_STREAMS="${GRAPHER_DATA_COLLECTION_STREAMS}"
export CHRONOLOG_GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM="${GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM}"
export CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_EVENTS="${KEEPER_WAL_DRAIN_BATCH_EVENTS}"
export CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_WAIT_US="${KEEPER_WAL_DRAIN_BATCH_WAIT_US}"
export CHRONOLOG_KEEPER_STOP_STORY_STATS_WAIT_MS="${KEEPER_STOP_STORY_STATS_WAIT_MS}"
export CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN="${KEEPER_STOP_STORY_FLUSH_DRAIN}"
export CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS="${KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS}"
export CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE="${KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE}"
export CHRONOLOG_KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS="${KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS}"
export CHRONOLOG_VISOR_PARALLEL_KEEPER_STOP="${VISOR_PARALLEL_KEEPER_STOP}"
export CHRONOLOG_VISOR_PARALLEL_RECORDING_STOP="${VISOR_PARALLEL_RECORDING_STOP}"
export CHRONOLOG_GRAPHER_RETIRE_ON_STOP="${GRAPHER_RETIRE_ON_STOP}"
export CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US="${GRAPHER_STOP_RETIRE_GRACE_US}"
export CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN="${GRAPHER_STOP_STORY_ARCHIVE_DRAIN}"
export CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS="${GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS}"
export CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT="${GRAPHER_STOP_DRAIN_COMPLETE_WAIT}"
export CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS="${GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS}"
export CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC="${GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC}"
export CHRONOLOG_GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK="${GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK}"
export CHRONOLOG_GRAPHER_EXTRACTION_THREADS="${GRAPHER_EXTRACTION_THREADS}"
export CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME="${HDF5_ARCHIVE_ATOMIC_RENAME}"
export CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS="${HDF5_ARCHIVE_CHUNK_EVENTS}"
export CHRONOLOG_HDF5_ARCHIVE_LAYOUT="${HDF5_ARCHIVE_LAYOUT}"
export CHRONOLOG_RAW_BLOB_PREALLOCATE="${RAW_BLOB_PREALLOCATE}"
export CHRONOLOG_RAW_BLOB_ASYNC_WRITE="${RAW_BLOB_ASYNC_WRITE}"
export CHRONOLOG_RAW_BLOB_ASYNC_CLOSE="${RAW_BLOB_ASYNC_CLOSE}"
export CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH="${RAW_BLOB_ASYNC_PUBLISH}"
export CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS="${RAW_BLOB_ASYNC_PUBLISH_THREADS}"
export CHRONOLOG_RAW_BLOB_PUBLISH_BEFORE_CLOSE="${RAW_BLOB_PUBLISH_BEFORE_CLOSE}"
export CHRONOLOG_RAW_BLOB_FAST_RESERVE="${RAW_BLOB_FAST_RESERVE}"
export CHRONOLOG_RAW_BLOB_SIDECAR_META="${RAW_BLOB_SIDECAR_META}"
export CHRONOLOG_GRAPHER_INACTIVE_STORY_DELAY_SECONDS_EFFECTIVE="${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}"
export CHRONOLOG_KEEPER_RECORDING_MARGO_XSTREAMS="${KEEPER_RECORDING_MARGO_XSTREAMS}"
export CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD="${KEEPER_RECORDING_MARGO_PROGRESS_THREAD}"
export CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS="${KEEPER_RECORDING_MARGO_HANDLERS}"
export CHRONOLOG_GRAPHER_RECORDING_MARGO_XSTREAMS="${GRAPHER_RECORDING_MARGO_XSTREAMS}"
export CHRONOLOG_GRAPHER_RECORDING_MARGO_HANDLERS="${GRAPHER_RECORDING_MARGO_HANDLERS}"
export CHRONOLOG_GRAPHER_DIRECT_DESERIALIZE="${GRAPHER_DIRECT_DESERIALIZE}"
export CHRONOLOG_KEEPER_DRAIN_MARGO_PROGRESS_THREAD="${KEEPER_DRAIN_MARGO_PROGRESS_THREAD}"
export CHRONOLOG_KEEPER_DRAIN_MARGO_RPC_THREADS="${KEEPER_DRAIN_MARGO_RPC_THREADS}"
export CHRONOLOG_KEEPER_EXTRACTION_THREADS="${KEEPER_EXTRACTION_THREADS}"
export CHRONOLOG_KEEPER_DIRECT_SERIALIZE="${KEEPER_DIRECT_SERIALIZE}"
export CHRONOLOG_KEEPER_FAST_WIRE="${KEEPER_FAST_WIRE}"
WRAPPER_DIR="${CHRONOLOG_RESULT_DIR}/wrappers"
DEPLOY_WORK_DIR="${CHRONOLOG_RESULT_DIR}/deploy-work"
DEPLOY_CONF_DIR="${DEPLOY_WORK_DIR}/conf"
mkdir -p "${CONFIG_DIR}" "${CHRONOLOG_RESULT_DIR}" "${CHRONOLOG_LOG_DIR}" "${CHRONOLOG_OUTPUT_DIR}" \
  "${CHRONOLOG_RUNTIME_TMP_DIR}" "${WRAPPER_DIR}" "${DEPLOY_CONF_DIR}"
export TMPDIR="${CHRONOLOG_RUNTIME_TMP_DIR}"
export PRTE_MCA_prte_tmpdir_base="${CHRONOLOG_RUNTIME_TMP_DIR}"
export PMIX_MCA_pmix_server_tmpdir="${CHRONOLOG_RUNTIME_TMP_DIR}"
if [[ "${KEEPER_JOURNAL_MODE}" != "disabled" ]]; then
  mkdir -p "${KEEPER_JOURNAL_DIR}"
fi

exec > >(tee -a "${CHRONOLOG_RESULT_DIR}/stdout.log")
exec 2> >(tee -a "${CHRONOLOG_RESULT_DIR}/stderr.log" >&2)

DEPLOY_SCRIPT="${INSTALL_DIR}/tools/deploy/deploy_cluster.sh"
BENCH_BIN="${INSTALL_DIR}/tools/benchmark/chrono-bench"
BASE_CONF="${INSTALL_DIR}/conf/default-chrono-conf.json"
BASE_CLIENT_CONF="${INSTALL_DIR}/conf/default-chrono-client-conf.json"
export CHRONOLOG_INSTALL_DIR_EFFECTIVE="${INSTALL_DIR}"
export CHRONOLOG_BIN_DIR_EFFECTIVE="${INSTALL_DIR}/bin"
export CHRONOLOG_DEPLOY_SCRIPT_EFFECTIVE="${DEPLOY_SCRIPT}"
export CHRONOLOG_BENCHMARK_BIN_EFFECTIVE="${BENCH_BIN}"
CONF_FILE="${CONFIG_DIR}/default-chrono-conf.json"
CLIENT_CONF_FILE="${CONFIG_DIR}/default-chrono-client-conf.json"

cp "${BASE_CONF}" "${CONF_FILE}"
cp "${BASE_CLIENT_CONF}" "${CLIENT_CONF_FILE}"

if [[ -n "${CHRONOLOG_RPC_PORT_OFFSET:-}" ]]; then
  RPC_PORT_OFFSET="${CHRONOLOG_RPC_PORT_OFFSET}"
elif [[ -n "${SLURM_JOB_ID:-}" ]]; then
  RPC_PORT_OFFSET=$(( (SLURM_JOB_ID % 400) * 100 ))
else
  RPC_PORT_OFFSET=0
fi
if ! [[ "${RPC_PORT_OFFSET}" =~ ^[0-9]+$ ]]; then
  echo "CHRONOLOG_RPC_PORT_OFFSET must be a non-negative integer: ${RPC_PORT_OFFSET}" >&2
  exit 2
fi
export CHRONOLOG_RPC_PORT_OFFSET="${RPC_PORT_OFFSET}"
if [[ "${RPC_PORT_OFFSET}" -gt 0 ]]; then
  for config_file in "${CONF_FILE}" "${CLIENT_CONF_FILE}"; do
    jq \
      --argjson port_offset "${RPC_PORT_OFFSET}" \
      'def walk(f):
         . as $in
         | if type == "object" then
             reduce keys_unsorted[] as $key ({}; . + {($key): ($in[$key] | walk(f))}) | f
           elif type == "array" then
             map(walk(f)) | f
           else
             f
           end;
       walk(if type == "object" and has("service_base_port")
            then .service_base_port = (.service_base_port + $port_offset)
            else .
            end)' \
      "${config_file}" > "${CONFIG_DIR}/$(basename "${config_file}").ports.tmp" \
      && mv "${CONFIG_DIR}/$(basename "${config_file}").ports.tmp" "${config_file}"
  done
fi

jq \
  --arg story_files_dir "${CHRONOLOG_OUTPUT_DIR}" \
  '.chrono_keeper.Extractors.story_files_dir = $story_files_dir
   | .chrono_grapher.Extractors.story_files_dir = $story_files_dir
   | .chrono_player.ArchiveReaders.story_files_dir = $story_files_dir' \
  "${CONF_FILE}" > "${CONFIG_DIR}/chrono-conf-story-files-dir.tmp" \
  && mv "${CONFIG_DIR}/chrono-conf-story-files-dir.tmp" "${CONF_FILE}"

if [[ -n "${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}" ]]; then
  jq \
    --argjson poll_interval_us "${DATA_COLLECTION_POLL_INTERVAL_US}" \
    --argjson grapher_inactive_story_delay_secs "${GRAPHER_INACTIVE_STORY_DELAY_SECONDS}" \
    '.chrono_keeper.DataStoreInternals.data_collection_poll_interval_us = $poll_interval_us
     | .chrono_grapher.DataStoreInternals.data_collection_poll_interval_us = $poll_interval_us
     | .chrono_player.DataStoreInternals.data_collection_poll_interval_us = $poll_interval_us
     | .chrono_grapher.DataStoreInternals.inactive_story_delay_secs = $grapher_inactive_story_delay_secs' \
    "${CONF_FILE}" > "${CONFIG_DIR}/chrono-conf-datastore-knobs.tmp" \
    && mv "${CONFIG_DIR}/chrono-conf-datastore-knobs.tmp" "${CONF_FILE}"
else
  jq \
    --argjson poll_interval_us "${DATA_COLLECTION_POLL_INTERVAL_US}" \
    '.chrono_keeper.DataStoreInternals.data_collection_poll_interval_us = $poll_interval_us
     | .chrono_grapher.DataStoreInternals.data_collection_poll_interval_us = $poll_interval_us
     | .chrono_player.DataStoreInternals.data_collection_poll_interval_us = $poll_interval_us' \
    "${CONF_FILE}" > "${CONFIG_DIR}/chrono-conf-datastore-knobs.tmp" \
    && mv "${CONFIG_DIR}/chrono-conf-datastore-knobs.tmp" "${CONF_FILE}"
fi

if [[ -n "${SLURM_JOB_NODELIST:-}" ]]; then
  scontrol show hostnames "${SLURM_JOB_NODELIST}" | head -n "${NODE_COUNT}" > "${CONFIG_DIR}/chronolog-slurm-nodes.txt"
else
  sinfo -N -h -p "${PARTITION}" -t idle -o '%N' | sort -u | head -n "${NODE_COUNT}" > "${CONFIG_DIR}/chronolog-slurm-nodes.txt"
fi

if [[ "$(wc -l < "${CONFIG_DIR}/chronolog-slurm-nodes.txt")" -lt "${NODE_COUNT}" ]]; then
  echo "Only found $(wc -l < "${CONFIG_DIR}/chronolog-slurm-nodes.txt") nodes for requested node count ${NODE_COUNT}" >&2
  exit 1
fi

head -n 1 "${CONFIG_DIR}/chronolog-slurm-nodes.txt" > "${CONFIG_DIR}/hosts_visor"
cp "${CONFIG_DIR}/chronolog-slurm-nodes.txt" "${CONFIG_DIR}/hosts_keeper"
tail -n "${RECORD_GROUPS}" "${CONFIG_DIR}/chronolog-slurm-nodes.txt" > "${CONFIG_DIR}/hosts_grapher"
tail -n "${RECORD_GROUPS}" "${CONFIG_DIR}/chronolog-slurm-nodes.txt" > "${CONFIG_DIR}/hosts_player"
cp "${CONFIG_DIR}/hosts_player" "${DEPLOY_CONF_DIR}/hosts_player"
CLIENT_NODE="$(head -n 1 "${CONFIG_DIR}/chronolog-slurm-nodes.txt")"
CLIENT_IP="$(dig +short "${CLIENT_NODE}-40g" | head -n 1)"
if [[ -z "${CLIENT_IP}" ]]; then
  CLIENT_IP="$(dig +short "${CLIENT_NODE}" | head -n 1)"
fi
if [[ -z "${CLIENT_IP}" ]]; then
  echo "Could not resolve client node IP for ${CLIENT_NODE}" >&2
  exit 1
fi

preclean_node_local_journals() {
  if [[ "${KEEPER_JOURNAL_MODE}" == "disabled" || "${KEEPER_JOURNAL_PLACEMENT}" != "node_local" ]]; then
    return 0
  fi
  if [[ "${KEEPER_JOURNAL_LOCAL_PRECLEAN}" != "1" ]]; then
    return 0
  fi
  if [[ -z "${KEEPER_JOURNAL_LOCAL_BASE}" || -z "${KEEPER_JOURNAL_RUN_ID}" ]]; then
    echo "Cannot pre-clean node-local journals without local base and run id" >&2
    return 1
  fi
  while IFS= read -r node; do
    [[ -n "${node}" ]] || continue
    local remote_dir="${KEEPER_JOURNAL_LOCAL_BASE}/${KEEPER_JOURNAL_RUN_ID}/${node}"
    env -u LD_LIBRARY_PATH ssh -n "${node}" "bash -lc 'rm -rf ${remote_dir@Q} && mkdir -p ${remote_dir@Q}'"
  done < "${CONFIG_DIR}/hosts_keeper"
}
preclean_node_local_journals

create_wrapper() {
  local role="$1"
  local real_bin="${INSTALL_DIR}/bin/${role}"
  local wrapper="${WRAPPER_DIR}/${role}"
  local role_profile_dir="${CHRONOLOG_RESULT_DIR}/profiles/${PROFILE_MODE}/${role}"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'if [[ "${CHRONOLOG_WRAPPER_ARGV0_SET:-0}" != "1" ]]; then'
    printf '%s\n' '  export CHRONOLOG_WRAPPER_ARGV0_SET=1'
    printf '%s\n' '  exec -a "$0" bash "$0" "$@"'
    printf '%s\n' 'fi'
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'profile_mode=%q\n' "${PROFILE_MODE}"
    printf 'keeper_journal_placement=%q\n' "${KEEPER_JOURNAL_PLACEMENT}"
    printf 'keeper_journal_local_base=%q\n' "${KEEPER_JOURNAL_LOCAL_BASE}"
    printf 'keeper_journal_run_id=%q\n' "${KEEPER_JOURNAL_RUN_ID}"
    printf 'role_profile_dir=%q\n' "${role_profile_dir}"
    printf 'perf_bin=%q\n' "${PERF_BIN}"
    printf 'perf_event=%q\n' "${PERF_EVENT}"
    printf 'perf_call_graph=%q\n' "${PERF_CALL_GRAPH}"
    printf 'gperftools_preload_lib=%q\n' "${GPERFTOOLS_PRELOAD_LIB}"
    printf 'gperftools_enable_cpu=%q\n' "${GPERFTOOLS_ENABLE_CPU}"
    printf 'gperftools_enable_heap=%q\n' "${GPERFTOOLS_ENABLE_HEAP}"
    printf 'gperftools_heap_allocation_interval=%q\n' "${GPERFTOOLS_HEAP_ALLOCATION_INTERVAL}"
    printf 'gperftools_cpu_profile_signal=%q\n' "${GPERFTOOLS_CPU_PROFILE_SIGNAL}"
    if [[ "${role}" == "chrono-keeper" ]]; then
      printf '%s\n' 'if [[ "${keeper_journal_placement}" == "node_local" ]]; then'
      printf '%s\n' '  export CHRONOLOG_KEEPER_JOURNAL_DIR="${keeper_journal_local_base}/${keeper_journal_run_id}/$(hostname)"'
      printf '%s\n' '  mkdir -p "${CHRONOLOG_KEEPER_JOURNAL_DIR}"'
      printf '%s\n' 'fi'
      printf 'export CHRONOLOG_KEEPER_JOURNAL_MODE=%q\n' "${KEEPER_JOURNAL_MODE}"
      if [[ "${KEEPER_JOURNAL_PLACEMENT}" == "shared" ]]; then
        printf 'export CHRONOLOG_KEEPER_JOURNAL_DIR=%q\n' "${KEEPER_JOURNAL_DIR}"
      fi
      printf 'export CHRONOLOG_KEEPER_TAIL_SOURCE=%q\n' "${KEEPER_TAIL_SOURCE}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_TAIL_READ_MODE=%q\n' "${KEEPER_JOURNAL_TAIL_READ_MODE}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS=%q\n' "${KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_TAIL_BATCH_MAX_EVENTS=%q\n' "${KEEPER_TAIL_BATCH_MAX_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES=%q\n' "${KEEPER_TAIL_BATCH_MAX_BYTES}"
      printf 'export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES=%q\n' "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES}"
      printf 'export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER=%q\n' "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_ACK_BEFORE_INGEST=%q\n' "${KEEPER_JOURNAL_ACK_BEFORE_INGEST}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN=%q\n' "${KEEPER_JOURNAL_ASYNC_DRAIN}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE=%q\n' "${KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_THREADS=%q\n' "${KEEPER_JOURNAL_ASYNC_DRAIN_THREADS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_SKIP_INGEST=%q\n' "${KEEPER_JOURNAL_SKIP_INGEST}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_DEFER_RPC_RESPONSE=%q\n' "${KEEPER_JOURNAL_DEFER_RPC_RESPONSE}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_SHARDS=%q\n' "${KEEPER_JOURNAL_SHARDS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_SHARD_POLICY=%q\n' "${KEEPER_JOURNAL_SHARD_POLICY}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS=%q\n' "${KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_WRITEV=%q\n' "${KEEPER_JOURNAL_WRITEV}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV=%q\n' "${KEEPER_JOURNAL_BATCH_WRITEV}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS=%q\n' "${KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_ATOMIC_OFFSETS=%q\n' "${KEEPER_JOURNAL_ATOMIC_OFFSETS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER=%q\n' "${KEEPER_JOURNAL_SINGLE_WRITER}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS=%q\n' "${KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_WAIT}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS=%q\n' "${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS=%q\n' "${KEEPER_JOURNAL_OWNER_DRAIN_YIELDS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY=%q\n' "${KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH=%q\n' "${KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH=%q\n' "${KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS=%q\n' "${KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN=%q\n' "${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX=%q\n' "${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES=%q\n' "${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES}"
      printf 'export CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH=%q\n' "${KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH}"
      printf 'export CHRONOLOG_KEEPER_RECORDING_MARGO_XSTREAMS=%q\n' "${KEEPER_RECORDING_MARGO_XSTREAMS}"
      printf 'export CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD=%q\n' "${KEEPER_RECORDING_MARGO_PROGRESS_THREAD}"
      printf 'export CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS=%q\n' "${KEEPER_RECORDING_MARGO_HANDLERS}"
      printf 'export CHRONOLOG_KEEPER_DATA_COLLECTION_STREAMS=%q\n' "${KEEPER_DATA_COLLECTION_STREAMS}"
      printf 'export CHRONOLOG_KEEPER_DATA_COLLECTION_THREADS_PER_STREAM=%q\n' "${KEEPER_DATA_COLLECTION_THREADS_PER_STREAM}"
      printf 'export CHRONOLOG_KEEPER_DRAIN_MARGO_PROGRESS_THREAD=%q\n' "${KEEPER_DRAIN_MARGO_PROGRESS_THREAD}"
      printf 'export CHRONOLOG_KEEPER_DRAIN_MARGO_RPC_THREADS=%q\n' "${KEEPER_DRAIN_MARGO_RPC_THREADS}"
      printf 'export CHRONOLOG_KEEPER_EXTRACTION_THREADS=%q\n' "${KEEPER_EXTRACTION_THREADS}"
      printf 'export CHRONOLOG_KEEPER_DIRECT_SERIALIZE=%q\n' "${KEEPER_DIRECT_SERIALIZE}"
      printf 'export CHRONOLOG_KEEPER_FAST_WIRE=%q\n' "${KEEPER_FAST_WIRE}"
      printf 'export CHRONOLOG_GRAPHER_RECORDING_MARGO_XSTREAMS=%q\n' "${GRAPHER_RECORDING_MARGO_XSTREAMS}"
      printf 'export CHRONOLOG_GRAPHER_RECORDING_MARGO_HANDLERS=%q\n' "${GRAPHER_RECORDING_MARGO_HANDLERS}"
      printf 'export CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_EVENTS=%q\n' "${KEEPER_WAL_DRAIN_BATCH_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_WAIT_US=%q\n' "${KEEPER_WAL_DRAIN_BATCH_WAIT_US}"
      printf 'export CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS=%q\n' "${KEEPER_APPEND_STATS_INTERVAL_EVENTS}"
      printf 'export CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS=%q\n' "${CLIENT_APPEND_STATS_INTERVAL_EVENTS}"
      printf 'export CHRONOLOG_KEEPER_STOP_STORY_STATS_WAIT_MS=%q\n' "${KEEPER_STOP_STORY_STATS_WAIT_MS}"
      printf 'export CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN=%q\n' "${KEEPER_STOP_STORY_FLUSH_DRAIN}"
      printf 'export CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS=%q\n' "${KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS}"
      printf 'export CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE=%q\n' "${KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE}"
    fi
    if [[ "${role}" == "chrono-visor" ]]; then
      printf 'export CHRONOLOG_VISOR_PARALLEL_KEEPER_STOP=%q\n' "${VISOR_PARALLEL_KEEPER_STOP}"
      printf 'export CHRONOLOG_VISOR_PARALLEL_RECORDING_STOP=%q\n' "${VISOR_PARALLEL_RECORDING_STOP}"
    fi
    if [[ "${role}" == "chrono-grapher" ]]; then
      printf 'export CHRONOLOG_GRAPHER_RETIRE_ON_STOP=%q\n' "${GRAPHER_RETIRE_ON_STOP}"
      printf 'export CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US=%q\n' "${GRAPHER_STOP_RETIRE_GRACE_US}"
      printf 'export CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN=%q\n' "${GRAPHER_STOP_STORY_ARCHIVE_DRAIN}"
      printf 'export CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS=%q\n' "${GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS}"
      printf 'export CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT=%q\n' "${GRAPHER_STOP_DRAIN_COMPLETE_WAIT}"
      printf 'export CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS=%q\n' "${GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS}"
      printf 'export CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC=%q\n' "${GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC}"
      printf 'export CHRONOLOG_GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK=%q\n' "${GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK}"
      printf 'export CHRONOLOG_GRAPHER_EXTRACTION_THREADS=%q\n' "${GRAPHER_EXTRACTION_THREADS}"
      printf 'export CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME=%q\n' "${HDF5_ARCHIVE_ATOMIC_RENAME}"
      printf 'export CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS=%q\n' "${HDF5_ARCHIVE_CHUNK_EVENTS}"
      printf 'export CHRONOLOG_HDF5_ARCHIVE_LAYOUT=%q\n' "${HDF5_ARCHIVE_LAYOUT}"
      printf 'export CHRONOLOG_RAW_BLOB_PREALLOCATE=%q\n' "${RAW_BLOB_PREALLOCATE}"
      printf 'export CHRONOLOG_RAW_BLOB_ASYNC_WRITE=%q\n' "${RAW_BLOB_ASYNC_WRITE}"
      printf 'export CHRONOLOG_RAW_BLOB_ASYNC_CLOSE=%q\n' "${RAW_BLOB_ASYNC_CLOSE}"
      printf 'export CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH=%q\n' "${RAW_BLOB_ASYNC_PUBLISH}"
      printf 'export CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS=%q\n' "${RAW_BLOB_ASYNC_PUBLISH_THREADS}"
      printf 'export CHRONOLOG_RAW_BLOB_PUBLISH_BEFORE_CLOSE=%q\n' "${RAW_BLOB_PUBLISH_BEFORE_CLOSE}"
      printf 'export CHRONOLOG_RAW_BLOB_FAST_RESERVE=%q\n' "${RAW_BLOB_FAST_RESERVE}"
      printf 'export CHRONOLOG_RAW_BLOB_SIDECAR_META=%q\n' "${RAW_BLOB_SIDECAR_META}"
      printf 'export CHRONOLOG_GRAPHER_RECORDING_MARGO_XSTREAMS=%q\n' "${GRAPHER_RECORDING_MARGO_XSTREAMS}"
      printf 'export CHRONOLOG_GRAPHER_RECORDING_MARGO_HANDLERS=%q\n' "${GRAPHER_RECORDING_MARGO_HANDLERS}"
      printf 'export CHRONOLOG_GRAPHER_DATA_COLLECTION_STREAMS=%q\n' "${GRAPHER_DATA_COLLECTION_STREAMS}"
      printf 'export CHRONOLOG_GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM=%q\n' "${GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM}"
      printf 'export CHRONOLOG_GRAPHER_DIRECT_DESERIALIZE=%q\n' "${GRAPHER_DIRECT_DESERIALIZE}"
    fi
    printf '%s\n' 'mkdir -p "${role_profile_dir}"'
    printf '%s\n' 'if [[ "${profile_mode}" == "tau" ]]; then'
    printf '%s\n' '  export PROFILEDIR="${role_profile_dir}/$(hostname)"'
    printf '%s\n' '  mkdir -p "${PROFILEDIR}"'
    printf '%s\n' 'elif [[ "${profile_mode}" == "gperftools" ]]; then'
    printf '%s\n' '  if [[ -z "${gperftools_preload_lib}" || ! -r "${gperftools_preload_lib}" ]]; then echo "gperftools preload library unavailable: ${gperftools_preload_lib}" >&2; exit 127; fi'
    printf '%s\n' '  export LD_PRELOAD="${gperftools_preload_lib}:${LD_PRELOAD:-}"'
    printf '%s\n' '  export CHRONOLOG_GPERFTOOLS_PRELOAD_LIB="${gperftools_preload_lib}"'
    printf '%s\n' '  if [[ -n "${gperftools_cpu_profile_signal}" ]]; then export CPUPROFILESIGNAL="${gperftools_cpu_profile_signal}"; fi'
    printf '%s\n' '  if [[ "${gperftools_enable_cpu}" == "1" ]]; then export CPUPROFILE="${role_profile_dir}/$(hostname).cpu.prof"; fi'
    printf '%s\n' '  if [[ "${gperftools_enable_heap}" == "1" ]]; then export HEAPPROFILE="${role_profile_dir}/$(hostname).heap"; export HEAP_PROFILE_ALLOCATION_INTERVAL="${gperftools_heap_allocation_interval}"; fi'
    printf '%s\n' 'elif [[ "${profile_mode}" == "darshan" ]]; then'
    printf '%s\n' '  export DARSHAN_LOG_DIR_PATH="${role_profile_dir}"'
    printf '%s\n' '  export DARSHAN_ENABLE_NONMPI=1'
    printf '%s\n' '  export LD_PRELOAD="/mnt/common/jcernudagarcia/spack/opt/spack/linux-ubuntu22.04-skylake_avx512/gcc-11.4.0/darshan-runtime-3.4.6-u7vfz6edqy4hzj6fnpx6g7inobzd32os/lib/libdarshan.so:${LD_PRELOAD:-}"'
    printf '%s\n' 'elif [[ "${profile_mode}" == "perf" ]]; then'
    printf '%s\n' '  if [[ -z "${perf_bin}" || ! -x "${perf_bin}" ]]; then echo "perf binary unavailable: ${perf_bin}" >&2; exit 127; fi'
    printf '%s\n' 'elif [[ "${profile_mode}" == "strace" ]]; then'
    printf '%s\n' '  command -v strace >/dev/null 2>&1 || { echo "strace unavailable on $(hostname)" >&2; exit 127; }'
    printf '%s\n' 'fi'
    printf 'real_bin=%q\n' "${real_bin}"
    printf '%s\n' 'if [[ "${profile_mode}" == "perf" ]]; then'
    printf '%s\n' '  "${perf_bin}" record --all-user -e "${perf_event}" -F "${CHRONOLOG_PERF_FREQ:-49}" --call-graph "${perf_call_graph}" -o "${role_profile_dir}/$(hostname).perf.data" -- "${real_bin}" "$@" 2>"${role_profile_dir}/$(hostname).perf.stderr" &'
    printf '%s\n' 'elif [[ "${profile_mode}" == "strace" ]]; then'
    printf '%s\n' '  strace -ff -ttt -T -o "${role_profile_dir}/$(hostname).strace" -- "${real_bin}" "$@" &'
    printf '%s\n' 'else'
    printf '%s\n' '  "${real_bin}" "$@" &'
    printf '%s\n' 'fi'
    printf '%s\n' 'child=$!'
    printf '%s\n' 'if [[ "${profile_mode}" == "gperftools" && "${gperftools_enable_cpu}" == "1" && -n "${gperftools_cpu_profile_signal}" ]]; then'
    printf '%s\n' '  sleep 1'
    printf '%s\n' '  kill "-${gperftools_cpu_profile_signal}" "${child}" 2>/dev/null || true'
    printf '%s\n' 'fi'
    printf '%s\n' 'term_child() {'
    printf '%s\n' '  if [[ "${profile_mode}" == "perf" ]]; then'
    printf '%s\n' '    kill -INT "${child}" 2>/dev/null || true'
    printf '%s\n' '  else'
    printf '%s\n' '    kill -TERM "${child}" 2>/dev/null || true'
    printf '%s\n' '  fi'
    printf '%s\n' '  wait "${child}" 2>/dev/null || true'
    printf '%s\n' '}'
    printf '%s\n' 'trap term_child TERM INT'
    printf '%s\n' 'wait "${child}"'
  } > "${wrapper}"
  chmod +x "${wrapper}"
}

create_wrapper chrono-visor
create_wrapper chrono-grapher
create_wrapper chrono-keeper
create_wrapper chrono-player

EBPF_PIDS_FILE="${CHRONOLOG_RESULT_DIR}/profiles/ebpf/collector-pids.txt"
start_ebpf_collectors() {
  if [[ "${PROFILE_MODE}" != "ebpf" ]]; then
    return 0
  fi
  mkdir -p "${CHRONOLOG_RESULT_DIR}/profiles/ebpf"
  : > "${EBPF_PIDS_FILE}"
  local nodes
  nodes="$(sort -u "${CONFIG_DIR}/hosts_keeper" "${CONFIG_DIR}/hosts_grapher" 2>/dev/null)"
  while IFS= read -r node; do
    [[ -n "${node}" ]] || continue
    local node_dir="${CHRONOLOG_RESULT_DIR}/profiles/ebpf/${node}"
    mkdir -p "${node_dir}"
    env -u LD_LIBRARY_PATH ssh -n "${node}" "bash -lc 'mkdir -p ${node_dir@Q}; keeper_pid=\$(pgrep -u ${USER@Q} -f \"chrono-keeper\" | head -n 1 || true); grapher_pid=\$(pgrep -u ${USER@Q} -f \"chrono-grapher\" | head -n 1 || true); echo keeper_pid=\${keeper_pid} grapher_pid=\${grapher_pid} > ${node_dir@Q}/pids.env; if [[ -n \${keeper_pid} ]]; then timeout $((EBPF_DURATION_SECONDS + 15))s sudo -n /usr/sbin/offcputime-bpfcc -p \${keeper_pid} -f ${EBPF_DURATION_SECONDS} > ${node_dir@Q}/keeper.offcputime.txt 2> ${node_dir@Q}/keeper.offcputime.err & echo \$! > ${node_dir@Q}/keeper.offcputime.pid; timeout $((EBPF_DURATION_SECONDS + 15))s sudo -n /usr/sbin/runqlat-bpfcc -p \${keeper_pid} ${EBPF_DURATION_SECONDS} > ${node_dir@Q}/keeper.runqlat.txt 2> ${node_dir@Q}/keeper.runqlat.err & echo \$! > ${node_dir@Q}/keeper.runqlat.pid; fi; if [[ -n \${grapher_pid} ]]; then timeout $((EBPF_DURATION_SECONDS + 15))s sudo -n /usr/sbin/runqlat-bpfcc -p \${grapher_pid} ${EBPF_DURATION_SECONDS} > ${node_dir@Q}/grapher.runqlat.txt 2> ${node_dir@Q}/grapher.runqlat.err & echo \$! > ${node_dir@Q}/grapher.runqlat.pid; fi'" \
      > "${node_dir}/collector-launch.log" 2> "${node_dir}/collector-launch.err" &
    echo "$!" >> "${EBPF_PIDS_FILE}"
  done <<< "${nodes}"
}

wait_ebpf_collectors() {
  if [[ "${PROFILE_MODE}" != "ebpf" || ! -f "${EBPF_PIDS_FILE}" ]]; then
    return 0
  fi
  while IFS= read -r pid; do
    [[ -n "${pid}" ]] || continue
    wait "${pid}" || true
  done < "${EBPF_PIDS_FILE}"
}

cat > "${CONFIG_DIR}/chronolog-config-manifest.env" <<EOF
deployment_mode=bare_metal
node_count=${NODE_COUNT}
nodelist=${NODELIST}
client_count=${CLIENT_COUNT}
workflow=${WORKFLOW}
completion_mode=${COMPLETION_MODE}
message_size_bytes=${MESSAGE_SIZE_BYTES}
data_collection_poll_interval_us=${DATA_COLLECTION_POLL_INTERVAL_US}
chrono_bench_barrier=${CHRONO_BENCH_BARRIER}
chrono_bench_shared_story=${CHRONO_BENCH_SHARED_STORY}
chronolog_producer_outstanding=${CHRONOLOG_PRODUCER_OUTSTANDING}
chronolog_producer_batch_size=${CHRONOLOG_PRODUCER_BATCH_SIZE}
chronolog_producer_wait_mode=${CHRONOLOG_PRODUCER_WAIT_MODE}
chronolog_bench_bound_keeper_futures=$([[ "${CHRONOLOG_PRODUCER_WAIT_MODE}" == "bounded_futures" ]] && echo 1 || echo 0)
chronolog_bench_owned_batch=${CHRONOLOG_BENCH_OWNED_BATCH}
chronolog_bench_owned_batch_min_message_size=${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}
chronolog_client_execution_mode=${CHRONOLOG_CLIENT_EXECUTION_MODE}
operation_count=${OPERATION_COUNT}
partition=${PARTITION}
slurm_job_id=${SLURM_JOB_ID:-}
slurm_time=${SLURM_TIME}
record_groups=${RECORD_GROUPS}
chronolog_install=${INSTALL_DIR}
chronolog_config=${CONF_FILE}
chronolog_client_config=${CLIENT_CONF_FILE}
chronolog_deploy_script=${DEPLOY_SCRIPT}
chronolog_benchmark=${BENCH_BIN}
chronolog_deploy_work_dir=${DEPLOY_WORK_DIR}
chronolog_runtime_tmp_dir=${CHRONOLOG_RUNTIME_TMP_DIR}
chronolog_rpc_port_offset=${RPC_PORT_OFFSET}
client_node=${CLIENT_NODE}
client_ip=${CLIENT_IP}
transport=ofi+sockets
profile_mode=${PROFILE_MODE}
perf_event=${PERF_EVENT}
perf_call_graph=${PERF_CALL_GRAPH}
client_mpi_oversubscribe=${CLIENT_MPI_OVERSUBSCRIBE}
keeper_journal_mode=${KEEPER_JOURNAL_MODE}
keeper_journal_dir=${KEEPER_JOURNAL_DIR}
keeper_journal_placement=${KEEPER_JOURNAL_PLACEMENT}
keeper_journal_local_base=${KEEPER_JOURNAL_LOCAL_BASE}
keeper_journal_run_id=${KEEPER_JOURNAL_RUN_ID}
keeper_tail_source=${KEEPER_TAIL_SOURCE}
keeper_tail_cursor_overlap_ns=${KEEPER_TAIL_CURSOR_OVERLAP_NS}
chronolog_keeper_tail_packed_bulk_chunk_bytes=${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES}
chronolog_keeper_tail_packed_bulk_parallel_transfer=${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER}
keeper_journal_callback_batch_drain=${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN}
keeper_journal_callback_batch_drain_max=${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX}
keeper_journal_callback_batch_drain_min_payload_bytes=${KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES}
keeper_recording_margo_xstreams=${KEEPER_RECORDING_MARGO_XSTREAMS}
keeper_recording_margo_progress_thread=${KEEPER_RECORDING_MARGO_PROGRESS_THREAD}
keeper_recording_margo_handlers=${KEEPER_RECORDING_MARGO_HANDLERS}
grapher_recording_margo_xstreams=${GRAPHER_RECORDING_MARGO_XSTREAMS}
grapher_recording_margo_handlers=${GRAPHER_RECORDING_MARGO_HANDLERS}
grapher_direct_deserialize=${GRAPHER_DIRECT_DESERIALIZE}
grapher_extraction_threads=${GRAPHER_EXTRACTION_THREADS}
keeper_drain_margo_progress_thread=${KEEPER_DRAIN_MARGO_PROGRESS_THREAD}
keeper_drain_margo_rpc_threads=${KEEPER_DRAIN_MARGO_RPC_THREADS}
keeper_extraction_threads=${KEEPER_EXTRACTION_THREADS}
keeper_direct_serialize=${KEEPER_DIRECT_SERIALIZE}
keeper_fast_wire=${KEEPER_FAST_WIRE}
keeper_journal_async_drain_source=${KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE}
keeper_journal_async_drain_threads=${KEEPER_JOURNAL_ASYNC_DRAIN_THREADS}
keeper_journal_skip_ingest=${KEEPER_JOURNAL_SKIP_INGEST}
keeper_journal_defer_rpc_response=${KEEPER_JOURNAL_DEFER_RPC_RESPONSE}
keeper_journal_shards=${KEEPER_JOURNAL_SHARDS}
keeper_journal_shard_policy=${KEEPER_JOURNAL_SHARD_POLICY}
keeper_journal_fdatasync_batch_events=${KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS}
keeper_journal_writev=${KEEPER_JOURNAL_WRITEV}
keeper_journal_batch_writev=${KEEPER_JOURNAL_BATCH_WRITEV}
keeper_journal_move_batch_payloads=${KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS}
keeper_journal_group_commit_wait=${KEEPER_JOURNAL_GROUP_COMMIT_WAIT}
keeper_journal_group_commit_flush_events=${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS}
keeper_journal_group_commit_flush_bytes=${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES}
keeper_journal_group_commit_strict_flush_event_cap=${KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP}
keeper_journal_group_commit_large_payload_bytes=${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES}
keeper_journal_group_commit_large_payload_flush_events=${KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS}
keeper_journal_group_commit_wait_us=${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US}
keeper_journal_group_commit_flush_wait_us=${KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US}
keeper_journal_group_commit_queue_boundary_min_events=${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS}
keeper_journal_group_commit_queue_boundary_min_payload_bytes=${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES}
keeper_journal_group_commit_queue_boundary_wait_every_n=${KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N}
keeper_journal_group_commit_wait_for_active_admissions=${KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS}
keeper_journal_owner_drain_yields=${KEEPER_JOURNAL_OWNER_DRAIN_YIELDS}
keeper_journal_notify_owner_only_on_empty=${KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY}
keeper_journal_async_callback_dispatch=${KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH}
keeper_journal_async_batch_completion_dispatch=${KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH}
keeper_journal_callback_dispatch_threads=${KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS}
keeper_journal_durable_complete_before_publish=${KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH}
keeper_wal_drain_batch_events=${KEEPER_WAL_DRAIN_BATCH_EVENTS}
keeper_wal_drain_batch_wait_us=${KEEPER_WAL_DRAIN_BATCH_WAIT_US}
keeper_data_collection_streams=${KEEPER_DATA_COLLECTION_STREAMS}
keeper_data_collection_threads_per_stream=${KEEPER_DATA_COLLECTION_THREADS_PER_STREAM}
grapher_data_collection_streams=${GRAPHER_DATA_COLLECTION_STREAMS}
grapher_data_collection_threads_per_stream=${GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM}
keeper_stop_story_flush_drain=${KEEPER_STOP_STORY_FLUSH_DRAIN}
keeper_stop_story_flush_drain_timeout_ms=${KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS}
keeper_restart_pre_crash_stats_wait_seconds=${KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS}
visor_parallel_keeper_stop=${VISOR_PARALLEL_KEEPER_STOP}
visor_parallel_recording_stop=${VISOR_PARALLEL_RECORDING_STOP}
grapher_retire_on_stop=${GRAPHER_RETIRE_ON_STOP}
grapher_stop_story_archive_drain=${GRAPHER_STOP_STORY_ARCHIVE_DRAIN}
grapher_stop_story_archive_drain_timeout_ms=${GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS}
hdf5_archive_atomic_rename=${HDF5_ARCHIVE_ATOMIC_RENAME}
hdf5_archive_chunk_events=${HDF5_ARCHIVE_CHUNK_EVENTS}
hdf5_archive_layout=${HDF5_ARCHIVE_LAYOUT}
raw_blob_preallocate=${RAW_BLOB_PREALLOCATE}
raw_blob_async_write=${RAW_BLOB_ASYNC_WRITE}
raw_blob_async_close=${RAW_BLOB_ASYNC_CLOSE}
raw_blob_async_publish=${RAW_BLOB_ASYNC_PUBLISH}
raw_blob_async_publish_threads=${RAW_BLOB_ASYNC_PUBLISH_THREADS}
raw_blob_publish_before_close=${RAW_BLOB_PUBLISH_BEFORE_CLOSE}
raw_blob_fast_reserve=${RAW_BLOB_FAST_RESERVE}
raw_blob_sidecar_meta=${RAW_BLOB_SIDECAR_META}
hdf5_archive_reader_bin=${HDF5_ARCHIVE_READER_BIN}
archive_range_event_count=${ARCHIVE_RANGE_EVENT_COUNT}
archive_event_count_poll_interval_seconds=${ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS}
archive_event_count_wait_mode=${ARCHIVE_EVENT_COUNT_WAIT_MODE}
archive_readback_mode=${ARCHIVE_READBACK_MODE}
startup_ready_timeout_seconds=${STARTUP_READY_TIMEOUT_SECONDS}
perf_bin=${PERF_BIN}
gperftools_preload_lib=${GPERFTOOLS_PRELOAD_LIB}
gperftools_enable_cpu=${GPERFTOOLS_ENABLE_CPU}
gperftools_enable_heap=${GPERFTOOLS_ENABLE_HEAP}
gperftools_heap_allocation_interval=${GPERFTOOLS_HEAP_ALLOCATION_INTERVAL}
startup_sleep_seconds=${STARTUP_SLEEP_SECONDS}
deploy_stop_timeout_seconds=${DEPLOY_STOP_TIMEOUT_SECONDS}
keeper_append_stats_wait_seconds=${KEEPER_APPEND_STATS_WAIT_SECONDS}
EOF

CLEANED_UP=0
cleanup() {
  if [[ "${CLEANED_UP}" == "1" ]]; then
    return 0
  fi
  CLEANED_UP=1
  set +e
  (
    cd "${DEPLOY_WORK_DIR}"
    env -u LD_LIBRARY_PATH timeout "${DEPLOY_STOP_TIMEOUT_SECONDS}s" "${DEPLOY_SCRIPT}" --stop \
      --work-dir "${DEPLOY_WORK_DIR}" \
      --visor-hosts "${CONFIG_DIR}/hosts_visor" \
      --keeper-hosts "${CONFIG_DIR}/hosts_keeper" \
      --grapher-hosts "${CONFIG_DIR}/hosts_grapher" \
      --conf-file "${CONF_FILE}" \
      --client-conf-file "${CLIENT_CONF_FILE}" \
      --visor-bin "${WRAPPER_DIR}/chrono-visor" \
      --keeper-bin "${WRAPPER_DIR}/chrono-keeper" \
      --grapher-bin "${WRAPPER_DIR}/chrono-grapher" \
      --player-bin "${WRAPPER_DIR}/chrono-player"
  ) > "${CHRONOLOG_LOG_DIR}/deploy-stop.log" 2>&1
  stop_status=$?
  if [[ "${stop_status}" -eq 124 ]]; then
    echo "ChronoLog deploy stop timed out after ${DEPLOY_STOP_TIMEOUT_SECONDS}s; relying on SLURM job teardown for remaining processes" \
      >> "${CHRONOLOG_LOG_DIR}/deploy-stop.log"
  fi
}
trap cleanup EXIT

wait_for_service_registrations() {
  local role="$1"
  local hosts_file="$2"
  local pattern="Successfully registered with ChronoVisor"
  local expected=0
  local deadline=$((SECONDS + STARTUP_READY_TIMEOUT_SECONDS))
  while IFS= read -r _host; do
    [[ -n "${_host}" ]] && expected=$((expected + 1))
  done < "${hosts_file}"
  if [[ "${expected}" -eq 0 ]]; then
    return 0
  fi
  while (( SECONDS <= deadline )); do
    local observed=0
    while IFS= read -r host; do
      [[ -n "${host}" ]] || continue
      local log_path="${CHRONOLOG_LOG_DIR}/chrono-${role}-${host}.log"
      if [[ -f "${log_path}" ]] && grep -q "${pattern}" "${log_path}"; then
        observed=$((observed + 1))
      fi
    done < "${hosts_file}"
    if [[ "${observed}" -ge "${expected}" ]]; then
      echo "Observed ${observed}/${expected} chrono-${role} registrations"
      return 0
    fi
    sleep 1
  done
  echo "Timed out waiting for chrono-${role} registrations after ${STARTUP_READY_TIMEOUT_SECONDS}s" >&2
  return 1
}

echo "ChronoLog distributed result directory: ${RESULT_DIR}"
echo "SLURM job: ${SLURM_JOB_ID:-none}"
echo "Nodes:"
cat "${CONFIG_DIR}/chronolog-slurm-nodes.txt"

(
  cd "${DEPLOY_WORK_DIR}"
  env -u LD_LIBRARY_PATH "${DEPLOY_SCRIPT}" --start \
    --work-dir "${DEPLOY_WORK_DIR}" \
    --record-groups "${RECORD_GROUPS}" \
    --visor-hosts "${CONFIG_DIR}/hosts_visor" \
    --keeper-hosts "${CONFIG_DIR}/hosts_keeper" \
    --grapher-hosts "${CONFIG_DIR}/hosts_grapher" \
    --conf-file "${CONF_FILE}" \
    --client-conf-file "${CLIENT_CONF_FILE}" \
    --monitor-dir "${CHRONOLOG_LOG_DIR}" \
    --output-dir "${CHRONOLOG_OUTPUT_DIR}" \
    --visor-bin "${WRAPPER_DIR}/chrono-visor" \
    --keeper-bin "${WRAPPER_DIR}/chrono-keeper" \
    --grapher-bin "${WRAPPER_DIR}/chrono-grapher" \
    --player-bin "${WRAPPER_DIR}/chrono-player"
)

jq ".chrono_client.ClientQueryService.rpc.service_ip = \"${CLIENT_IP}\"" "${CLIENT_CONF_FILE}" > "${CONFIG_DIR}/client-conf.tmp" \
  && mv "${CONFIG_DIR}/client-conf.tmp" "${CLIENT_CONF_FILE}"

sleep "${STARTUP_SLEEP_SECONDS}"
wait_for_service_registrations "keeper" "${CONFIG_DIR}/hosts_keeper"
wait_for_service_registrations "grapher" "${CONFIG_DIR}/hosts_grapher"
start_ebpf_collectors

MIN_EVENT_SIZE=$((MESSAGE_SIZE_BYTES / 2))
MAX_EVENT_SIZE=$((MESSAGE_SIZE_BYTES * 2))

if [[ "${WORKFLOW}" == "append_throughput" ]]; then
  set +e
  CLIENT_PROFILE_DIR="${CHRONOLOG_RESULT_DIR}/profiles/${PROFILE_MODE}/client"
  mkdir -p "${CLIENT_PROFILE_DIR}"
  CLIENT_MPI_ENV=""
  if [[ "${PROFILE_MODE}" == "tau" ]]; then
    CLIENT_PROFILE_EXPORTS="export PROFILEDIR='${CLIENT_PROFILE_DIR}'; mkdir -p \"\${PROFILEDIR}\";"
  elif [[ "${PROFILE_MODE}" == "gperftools" ]]; then
    CLIENT_PROFILE_EXPORTS=""
    if [[ -z "${GPERFTOOLS_PRELOAD_LIB}" || ! -r "${GPERFTOOLS_PRELOAD_LIB}" ]]; then
      echo "gperftools preload library unavailable: ${GPERFTOOLS_PRELOAD_LIB}" >&2
      exit 127
    fi
    CLIENT_MPI_ENV="-x LD_PRELOAD=${GPERFTOOLS_PRELOAD_LIB}"
    if [[ "${GPERFTOOLS_ENABLE_CPU}" == "1" ]]; then
      CLIENT_MPI_ENV+=" -x CPUPROFILE=${CLIENT_PROFILE_DIR}/chrono-bench.cpu.prof"
    fi
    if [[ "${GPERFTOOLS_ENABLE_HEAP}" == "1" ]]; then
      CLIENT_MPI_ENV+=" -x HEAPPROFILE=${CLIENT_PROFILE_DIR}/chrono-bench.heap -x HEAP_PROFILE_ALLOCATION_INTERVAL=${GPERFTOOLS_HEAP_ALLOCATION_INTERVAL}"
    fi
  elif [[ "${PROFILE_MODE}" == "darshan" ]]; then
    CLIENT_PROFILE_EXPORTS=""
    CLIENT_MPI_ENV="-x DARSHAN_LOG_DIR_PATH=${CLIENT_PROFILE_DIR} -x DARSHAN_ENABLE_NONMPI=1 -x LD_PRELOAD=/mnt/common/jcernudagarcia/spack/opt/spack/linux-ubuntu22.04-skylake_avx512/gcc-11.4.0/darshan-runtime-3.4.6-u7vfz6edqy4hzj6fnpx6g7inobzd32os/lib/libdarshan.so"
  elif [[ "${PROFILE_MODE}" == "strace" ]]; then
    CLIENT_PROFILE_EXPORTS=""
    CLIENT_MPI_ENV=""
  else
    CLIENT_PROFILE_EXPORTS=""
  fi
  if [[ "${PROFILE_MODE}" == "perf" ]]; then
    CLIENT_PROFILE_EXPORTS=""
    CLIENT_MPI_ENV=""
    CLIENT_MPI_SLOT_ARGS=""
    if [[ "${CLIENT_MPI_OVERSUBSCRIBE}" == "1" ]]; then
      CLIENT_MPI_SLOT_ARGS="--map-by :OVERSUBSCRIBE"
    fi
    CLIENT_CMD="'${PERF_BIN}' stat -d -d -d -o '${CLIENT_PROFILE_DIR}/chrono-bench.perf-stat.txt' -- '${PERF_BIN}' record --all-user -F '${CHRONOLOG_PERF_FREQ:-49}' --call-graph '${PERF_CALL_GRAPH}' -o '${CLIENT_PROFILE_DIR}/chrono-bench.perf.data' -- mpirun ${CLIENT_MPI_SLOT_ARGS} -n '${CLIENT_COUNT}' '${BENCH_BIN}' -c '${CLIENT_CONF_FILE}' -w -h '${CHRONICLE_COUNT}' -t '${STORY_COUNT}' -a '${MIN_EVENT_SIZE}' -s '${MESSAGE_SIZE_BYTES}' -b '${MAX_EVENT_SIZE}' -n '${OPERATION_COUNT}' ${CHRONO_BENCH_EXTRA_ARGS[*]@Q} -p"
  elif [[ "${PROFILE_MODE}" == "strace" ]]; then
    command -v strace >/dev/null 2>&1 || { echo "strace unavailable on client node" >&2; exit 127; }
    CLIENT_MPI_SLOT_ARGS=""
    if [[ "${CLIENT_MPI_OVERSUBSCRIBE}" == "1" ]]; then
      CLIENT_MPI_SLOT_ARGS="--map-by :OVERSUBSCRIBE"
    fi
    CLIENT_CMD="strace -ff -ttt -T -o '${CLIENT_PROFILE_DIR}/chrono-bench.strace' -- mpirun ${CLIENT_MPI_SLOT_ARGS} -n '${CLIENT_COUNT}' '${BENCH_BIN}' -c '${CLIENT_CONF_FILE}' -w -h '${CHRONICLE_COUNT}' -t '${STORY_COUNT}' -a '${MIN_EVENT_SIZE}' -s '${MESSAGE_SIZE_BYTES}' -b '${MAX_EVENT_SIZE}' -n '${OPERATION_COUNT}' ${CHRONO_BENCH_EXTRA_ARGS[*]@Q} -p"
  else
    CLIENT_MPI_SLOT_ARGS=""
    if [[ "${CLIENT_MPI_OVERSUBSCRIBE}" == "1" ]]; then
      CLIENT_MPI_SLOT_ARGS="--map-by :OVERSUBSCRIBE"
    fi
    CLIENT_CMD="mpirun ${CLIENT_MPI_SLOT_ARGS} ${CLIENT_MPI_ENV} -n '${CLIENT_COUNT}' '${BENCH_BIN}' -c '${CLIENT_CONF_FILE}' -w -h '${CHRONICLE_COUNT}' -t '${STORY_COUNT}' -a '${MIN_EVENT_SIZE}' -s '${MESSAGE_SIZE_BYTES}' -b '${MAX_EVENT_SIZE}' -n '${OPERATION_COUNT}' ${CHRONO_BENCH_EXTRA_ARGS[*]@Q} -p"
  fi
  if [[ "${PROFILE_MODE}" == "perf" ]]; then
    srun --nodes=1 --ntasks=1 --nodelist="${CLIENT_NODE}" bash -lc "$(module_loads); ${CLIENT_PROFILE_EXPORTS} ${CLIENT_CMD}" \
      > "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.log" \
      2> "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.stderr.log"
  else
    bash -lc "$(module_loads); ${CLIENT_PROFILE_EXPORTS} ${CLIENT_CMD}" \
      > "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.log" \
      2> "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.stderr.log"
  fi
  BENCH_STATUS=$?
  set -e

  python3 - "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.log" "${CHRONOLOG_RESULT_DIR}/chrono-bench-append-throughput.stderr.log" "${CHRONOLOG_RESULT_DIR}/metrics.json" "${NODE_COUNT}" "${CLIENT_COUNT}" "${OPERATION_COUNT}" "${MESSAGE_SIZE_BYTES}" "${BENCH_STATUS}" <<'PY'
import json
import re
import sys

log_path, stderr_path, metrics_path, node_count, client_count, operation_count, message_size, status = sys.argv[1:]
text = open(log_path, encoding="utf-8", errors="replace").read()
stderr_text = open(stderr_path, encoding="utf-8", errors="replace").read()
throughput_match = re.search(r"Record-event \(incl\. metadata time\) throughput:\s+([0-9.eE+-]+)\s+events/s", text)
bandwidth_match = re.search(r"Record-event \(incl\. metadata time\) bandwidth:\s+([0-9.eE+-]+)\s+MB/s", text)
payload_prep_count_match = re.search(r"Payload preparation count:\s+([0-9]+)\s+events", text)
payload_prep_total_match = re.search(r"Payload preparation total local time:\s+([0-9.eE+-]+)\s+s", text)
payload_prep_max_match = re.search(r"Payload preparation max rank time:\s+([0-9.eE+-]+)\s+s", text)
payload_prep_throughput_match = re.search(
    r"Payload preparation aggregate throughput:\s+([0-9.eE+-]+)\s+events/s", text
)
payload_prep_bandwidth_match = re.search(
    r"Payload preparation aggregate bandwidth:\s+([0-9.eE+-]+)\s+MB/s", text
)
raw_throughput = float(throughput_match.group(1)) if throughput_match else 0.0
# chrono-bench prints event throughput divided by 1e6 while labeling it events/s.
# Normalize to comparable ops/sec for ProfileForge metrics.
throughput = raw_throughput * 1_000_000.0 if throughput_match else 0.0
total_operation_count = int(operation_count) * int(client_count)
duration = total_operation_count / throughput if throughput else 0.0
metrics = {
    "system": "chronolog",
    "workflow": "append_throughput",
    "node_count": int(node_count),
    "nodes": int(node_count),
    "client_count": int(client_count),
    "parallel_clients": int(client_count),
    "message_size_bytes": int(message_size),
    "operation_count": int(operation_count),
    "operation_count_per_client": int(operation_count),
    "message_count_per_client": int(operation_count),
    "messages_per_client": int(operation_count),
    "total_operation_count": total_operation_count,
    "total_message_count": total_operation_count,
    "total_messages": total_operation_count,
    "total_payload_bytes": total_operation_count * int(message_size),
    "parallel_client_count": int(client_count),
    "duration_seconds": duration,
    "throughput_ops_per_sec": throughput,
    "avg_latency_ms": None,
    "p50_latency_ms": None,
    "p95_latency_ms": None,
    "p99_latency_ms": None,
    "benchmark_exit_status": int(status),
    "success": int(status) == 0 and bool(throughput_match),
}
error_patterns = [
    "CL_ERR_NO_KEEPERS",
    "Failed to acquire story",
    "prterun detected",
    "Exit code:",
    "HG_Register_data failed",
]
matched_errors = []
combined_error_text = "\n".join([text, stderr_text])
for pattern in error_patterns:
    if pattern in combined_error_text:
        matched_errors.append(pattern)
if matched_errors:
    metrics["benchmark_error_patterns"] = matched_errors
    metrics["benchmark_error_excerpt"] = "\n".join(
        line[:240]
        for line in combined_error_text.splitlines()
        if any(pattern in line for pattern in matched_errors)
    )[:1200]
if throughput_match:
    metrics["chrono_bench_raw_record_event_throughput"] = raw_throughput
    metrics["chrono_bench_raw_record_event_throughput_units"] = "million_events_per_sec_labeled_events_per_sec"
if bandwidth_match:
    metrics["record_event_bandwidth_mb_per_sec"] = float(bandwidth_match.group(1))
if payload_prep_count_match:
    metrics["chrono_bench_payload_prep_count"] = int(payload_prep_count_match.group(1))
if payload_prep_total_match:
    metrics["chrono_bench_payload_prep_total_local_seconds"] = float(payload_prep_total_match.group(1))
if payload_prep_max_match:
    metrics["chrono_bench_payload_prep_max_rank_seconds"] = float(payload_prep_max_match.group(1))
if payload_prep_throughput_match:
    metrics["chrono_bench_payload_prep_aggregate_ops_per_sec"] = float(payload_prep_throughput_match.group(1))
if payload_prep_bandwidth_match:
    metrics["chrono_bench_payload_prep_aggregate_mb_per_sec"] = float(payload_prep_bandwidth_match.group(1))
open(metrics_path, "w").write(json.dumps(metrics, indent=2) + "\n")
print(json.dumps(metrics, indent=2))
PY
elif [[ "${WORKFLOW}" == "append_latency" ]]; then
  set +e
  bash -lc "$(module_loads); export LD_LIBRARY_PATH='${INSTALL_DIR}/lib':\${LD_LIBRARY_PATH:-}; export PYTHONPATH='${INSTALL_DIR}/lib':\${PYTHONPATH:-}; python3 '${SCRIPT_DIR}/chronolog_append_latency.py' --config '${CLIENT_CONF_FILE}' --result-dir '${RESULT_DIR}' --operation-count '${OPERATION_COUNT}' --message-size-bytes '${MESSAGE_SIZE_BYTES}' --node-count '${NODE_COUNT}' --client-count '${CLIENT_COUNT}' --workflow append_latency" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-append-latency.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-append-latency.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
elif [[ "${WORKFLOW}" == "range_retrieval" ]]; then
  CLIENT_COMMAND="${CONFIG_DIR}/chronolog-range-client.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'cd %q\n' "${REPO_ROOT}"
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'export LD_LIBRARY_PATH=%q:${LD_LIBRARY_PATH:-}\n' "${INSTALL_DIR}/lib"
    printf 'export PYTHONPATH=%q:${PYTHONPATH:-}\n' "${INSTALL_DIR}/lib"
    if [[ "${PROFILE_MODE}" != "none" || -n "${CHRONOLOG_CLIENT_REPLAY_STATS:-}" || "${CHRONOLOG_MIXED_TAIL_READ_MODE}" == keeper_cursor* ]]; then
      printf '%s\n' 'export CHRONOLOG_CLIENT_REPLAY_STATS="${CHRONOLOG_CLIENT_REPLAY_STATS:-1}"'
      printf '%s\n' 'export CHRONOLOG_CLIENT_TAIL_RPC_STATS="${CHRONOLOG_CLIENT_TAIL_RPC_STATS:-1}"'
    fi
    printf 'export CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION=%q\n' "${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS=%q\n' "${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS}"
    printf 'export CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC=%q\n' "${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT}"
    printf 'export CHRONOLOG_MIXED_TAIL_READ_MODE=%q\n' "${CHRONOLOG_MIXED_TAIL_READ_MODE}"
    printf 'export CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS=%q\n' "${CLIENT_APPEND_STATS_INTERVAL_EVENTS}"
    printf 'timeout 600s python3 %q --config %q --result-dir %q --operation-count %q --message-size-bytes %q --node-count %q --client-count %q --archive-wait-seconds %q --noise-story-count %q --noise-events-per-story %q\n' \
      "${SCRIPT_DIR}/chronolog_range_retrieval.py" \
      "${CLIENT_CONF_FILE}" \
      "${RESULT_DIR}" \
      "${OPERATION_COUNT}" \
      "${MESSAGE_SIZE_BYTES}" \
      "${NODE_COUNT}" \
      "${CLIENT_COUNT}" \
      "${ARCHIVE_WAIT_SECONDS}" \
      "${CHRONOLOG_RANGE_NOISE_STORY_COUNT:-0}" \
      "${CHRONOLOG_RANGE_NOISE_EVENTS_PER_STORY:-0}"
  } > "${CLIENT_COMMAND}"
  chmod +x "${CLIENT_COMMAND}"
  set +e
  env -u LD_LIBRARY_PATH ssh -n "${CLIENT_NODE}" "bash '${CLIENT_COMMAND}'" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-range-retrieval.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-range-retrieval.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
elif [[ "${WORKFLOW}" == "archive_range_retrieval" ]]; then
  CLIENT_COMMAND="${CONFIG_DIR}/chronolog-archive-range-client.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'cd %q\n' "${REPO_ROOT}"
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'export LD_LIBRARY_PATH=%q:%q:${LD_LIBRARY_PATH:-}\n' "${INSTALL_DIR}/lib" "$(dirname "${HDF5_ARCHIVE_READER_BIN}")"
    printf 'export PYTHONPATH=%q:%q:${PYTHONPATH:-}\n' "${INSTALL_DIR}/lib" "${SCRIPT_DIR}"
    if [[ "${PROFILE_MODE}" == "tau" ]]; then
      printf 'export PROFILEDIR=%q\n' "${CHRONOLOG_RESULT_DIR}/profiles/tau/client"
      printf '%s\n' 'mkdir -p "${PROFILEDIR}"'
    fi
    printf 'timeout 1200s python3 %q --client-config %q --player-config %q --result-dir %q --archive-reader-bin %q --operation-count %q --message-size-bytes %q --range-event-count %q --node-count %q --client-count %q --archive-wait-seconds %q --archive-event-count-poll-interval-seconds %q --archive-event-count-wait-mode %q --archive-readback-mode %q\n' \
      "${SCRIPT_DIR}/chronolog_archive_range_retrieval.py" \
      "${CLIENT_CONF_FILE}" \
      "${CONF_FILE}" \
      "${RESULT_DIR}" \
      "${HDF5_ARCHIVE_READER_BIN}" \
      "${OPERATION_COUNT}" \
      "${MESSAGE_SIZE_BYTES}" \
      "${ARCHIVE_RANGE_EVENT_COUNT}" \
      "${NODE_COUNT}" \
      "${CLIENT_COUNT}" \
      "${ARCHIVE_WAIT_SECONDS}" \
      "${ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS}" \
      "${ARCHIVE_EVENT_COUNT_WAIT_MODE}" \
      "${ARCHIVE_READBACK_MODE}"
  } > "${CLIENT_COMMAND}"
  chmod +x "${CLIENT_COMMAND}"
  set +e
  env -u LD_LIBRARY_PATH ssh -n "${CLIENT_NODE}" "bash '${CLIENT_COMMAND}'" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-archive-range-retrieval.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-archive-range-retrieval.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
elif [[ "${WORKFLOW}" == "mixed_append_tail" ]]; then
  CLIENT_COMMAND="${CONFIG_DIR}/chronolog-mixed-append-tail-client.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'cd %q\n' "${REPO_ROOT}"
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'export LD_LIBRARY_PATH=%q:${LD_LIBRARY_PATH:-}\n' "${INSTALL_DIR}/lib"
    printf 'export PYTHONPATH=%q:${PYTHONPATH:-}\n' "${INSTALL_DIR}/lib"
    if [[ "${PROFILE_MODE}" == "tau" ]]; then
      printf 'export PROFILEDIR=%q\n' "${CHRONOLOG_RESULT_DIR}/profiles/tau/client"
      printf '%s\n' 'mkdir -p "${PROFILEDIR}"'
    elif [[ "${PROFILE_MODE}" == "gperftools" ]]; then
      printf 'export CHRONOLOG_GPERFTOOLS_PRELOAD_LIB=%q\n' "${GPERFTOOLS_PRELOAD_LIB}"
      printf 'export GPERFTOOLS_ENABLE_CPU=%q\n' "${GPERFTOOLS_ENABLE_CPU}"
      printf 'export GPERFTOOLS_ENABLE_HEAP=%q\n' "${GPERFTOOLS_ENABLE_HEAP}"
      printf 'export GPERFTOOLS_HEAP_ALLOCATION_INTERVAL=%q\n' "${GPERFTOOLS_HEAP_ALLOCATION_INTERVAL}"
      printf 'export CHRONOLOG_MIXED_CLIENT_PROFILE_DIR=%q\n' "${CHRONOLOG_RESULT_DIR}/profiles/gperftools/client"
      printf '%s\n' 'mkdir -p "${CHRONOLOG_MIXED_CLIENT_PROFILE_DIR}"'
      printf '%s\n' 'if [[ -z "${CHRONOLOG_GPERFTOOLS_PRELOAD_LIB}" || ! -r "${CHRONOLOG_GPERFTOOLS_PRELOAD_LIB}" ]]; then echo "gperftools preload library unavailable: ${CHRONOLOG_GPERFTOOLS_PRELOAD_LIB}" >&2; exit 127; fi'
      printf '%s\n' 'export LD_PRELOAD="${CHRONOLOG_GPERFTOOLS_PRELOAD_LIB}:${LD_PRELOAD:-}"'
      printf '%s\n' 'if [[ "${GPERFTOOLS_ENABLE_CPU}" == "1" ]]; then export CPUPROFILE="${CHRONOLOG_MIXED_CLIENT_PROFILE_DIR}/mixed-append-tail.cpu.prof"; fi'
      printf '%s\n' 'if [[ "${GPERFTOOLS_ENABLE_HEAP}" == "1" ]]; then export HEAPPROFILE="${CHRONOLOG_MIXED_CLIENT_PROFILE_DIR}/mixed-append-tail.heap"; export HEAP_PROFILE_ALLOCATION_INTERVAL="${GPERFTOOLS_HEAP_ALLOCATION_INTERVAL}"; fi'
    fi
    if [[ "${PROFILE_MODE}" != "none" || -n "${CHRONOLOG_CLIENT_REPLAY_STATS:-}" || "${CHRONOLOG_MIXED_TAIL_READ_MODE}" == keeper_cursor* ]]; then
      printf '%s\n' 'export CHRONOLOG_CLIENT_REPLAY_STATS="${CHRONOLOG_CLIENT_REPLAY_STATS:-1}"'
      printf '%s\n' 'export CHRONOLOG_CLIENT_TAIL_RPC_STATS="${CHRONOLOG_CLIENT_TAIL_RPC_STATS:-1}"'
    fi
    printf 'export CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION=%q\n' "${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS=%q\n' "${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS}"
    printf 'export CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC=%q\n' "${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES=%q\n' "${CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES}"
    printf 'export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER=%q\n' "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER}"
    printf 'export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES=%q\n' "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES}"
    printf 'export CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER=%q\n' "${CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER}"
    printf 'export CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS=%q\n' "${CLIENT_APPEND_STATS_INTERVAL_EVENTS}"
    printf 'timeout 900s python3 %q --config %q --result-dir %q --operation-count %q --message-size-bytes %q --node-count %q --client-count %q --tail-interval-ms %q --tail-read-mode %q --tail-reader-start-mode %q --tail-overlap-ns %q --tail-final-deadline-seconds %q --reader-join-timeout-seconds %q --producer-outstanding %q --producer-batch-size %q --writer-release-story %q\n' \
      "${SCRIPT_DIR}/chronolog_mixed_append_tail.py" \
      "${CLIENT_CONF_FILE}" \
      "${RESULT_DIR}" \
      "${OPERATION_COUNT}" \
      "${MESSAGE_SIZE_BYTES}" \
      "${NODE_COUNT}" \
      "${CLIENT_COUNT}" \
      "${CHRONOLOG_MIXED_TAIL_INTERVAL_MS:-10}" \
      "${CHRONOLOG_MIXED_TAIL_READ_MODE}" \
      "${CHRONOLOG_MIXED_TAIL_READER_START_MODE}" \
      "${CHRONOLOG_MIXED_TAIL_OVERLAP_NS:-1000000000}" \
      "${CHRONOLOG_MIXED_TAIL_FINAL_DEADLINE_SECONDS:-60}" \
      "${CHRONOLOG_MIXED_TAIL_READER_JOIN_TIMEOUT_SECONDS:-180}" \
      "${CHRONOLOG_PRODUCER_OUTSTANDING}" \
      "${CHRONOLOG_PRODUCER_BATCH_SIZE}" \
      "${CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY}"
  } > "${CLIENT_COMMAND}"
  chmod +x "${CLIENT_COMMAND}"
  set +e
  env -u LD_LIBRARY_PATH ssh -n "${CLIENT_NODE}" "bash '${CLIENT_COMMAND}'" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-mixed-append-tail.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-mixed-append-tail.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
elif [[ "${WORKFLOW}" == "keeper_restart_recovery" ]]; then
  CLIENT_COMMAND="${CONFIG_DIR}/chronolog-keeper-restart-recovery-client.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'cd %q\n' "${REPO_ROOT}"
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'export LD_LIBRARY_PATH=%q:${LD_LIBRARY_PATH:-}\n' "${INSTALL_DIR}/lib"
    printf 'export PYTHONPATH=%q:${PYTHONPATH:-}\n' "${INSTALL_DIR}/lib"
    if [[ "${PROFILE_MODE}" == "tau" ]]; then
      printf 'export PROFILEDIR=%q\n' "${CHRONOLOG_RESULT_DIR}/profiles/tau/client"
      printf '%s\n' 'mkdir -p "${PROFILEDIR}"'
    fi
    if [[ "${PROFILE_MODE}" != "none" || -n "${CHRONOLOG_CLIENT_REPLAY_STATS:-}" ]]; then
      printf '%s\n' 'export CHRONOLOG_CLIENT_REPLAY_STATS="${CHRONOLOG_CLIENT_REPLAY_STATS:-1}"'
    fi
    printf 'export CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION=%q\n' "${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS=%q\n' "${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS}"
    printf 'export CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC=%q\n' "${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}"
    printf 'export CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS=%q\n' "${CLIENT_APPEND_STATS_INTERVAL_EVENTS}"
    printf 'export CHRONOLOG_GPERFTOOLS_CPU_PROFILE_SIGNAL=%q\n' "${GPERFTOOLS_CPU_PROFILE_SIGNAL}"
    printf 'timeout 900s python3 %q --config %q --result-dir %q --operation-count %q --message-size-bytes %q --completion-mode %q --node-count %q --client-count %q --keeper-hosts %q --keeper-wrapper %q --config-dir %q --log-dir %q --producer-outstanding %q --producer-batch-size %q --producer-wait-mode %q --client-execution-mode %q --pre-crash-stats-snapshot-wait %q\n' \
      "${SCRIPT_DIR}/chronolog_keeper_restart_recovery.py" \
      "${CLIENT_CONF_FILE}" \
      "${RESULT_DIR}" \
      "${OPERATION_COUNT}" \
      "${MESSAGE_SIZE_BYTES}" \
      "${COMPLETION_MODE}" \
      "${NODE_COUNT}" \
      "${CLIENT_COUNT}" \
      "${CONFIG_DIR}/hosts_keeper" \
      "${WRAPPER_DIR}/chrono-keeper" \
      "${CONFIG_DIR}" \
      "${CHRONOLOG_LOG_DIR}" \
      "${CHRONOLOG_PRODUCER_OUTSTANDING}" \
      "${CHRONOLOG_PRODUCER_BATCH_SIZE}" \
      "${CHRONOLOG_PRODUCER_WAIT_MODE}" \
      "${CHRONOLOG_CLIENT_EXECUTION_MODE}" \
      "${KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS}"
  } > "${CLIENT_COMMAND}"
  chmod +x "${CLIENT_COMMAND}"
  set +e
  env -u LD_LIBRARY_PATH ssh -n "${CLIENT_NODE}" "bash '${CLIENT_COMMAND}'" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-keeper-restart-recovery.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-keeper-restart-recovery.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
elif [[ "${WORKFLOW}" == "append_durable" ]]; then
  CLIENT_COMMAND="${CONFIG_DIR}/chronolog-durable-append-client.sh"
  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf 'cd %q\n' "${REPO_ROOT}"
    printf '%s\n' 'source /etc/profile.d/lmod.sh 2>/dev/null || source /etc/profile.d/modules.sh 2>/dev/null || true'
    printf '%s\n' 'type module >/dev/null 2>&1 || { echo "module command unavailable on $(hostname)" >&2; exit 127; }'
    module_loads
    printf 'export LD_LIBRARY_PATH=%q:${LD_LIBRARY_PATH:-}\n' "${INSTALL_DIR}/lib"
    printf 'export PYTHONPATH=%q:${PYTHONPATH:-}\n' "${INSTALL_DIR}/lib"
    if [[ "${PROFILE_MODE}" == "tau" ]]; then
      printf 'export PROFILEDIR=%q\n' "${CHRONOLOG_RESULT_DIR}/profiles/tau/client"
      printf '%s\n' 'mkdir -p "${PROFILEDIR}"'
    fi
    if [[ "${PROFILE_MODE}" != "none" || -n "${CHRONOLOG_CLIENT_REPLAY_STATS:-}" ]]; then
      printf '%s\n' 'export CHRONOLOG_CLIENT_REPLAY_STATS="${CHRONOLOG_CLIENT_REPLAY_STATS:-1}"'
    fi
    printf 'export CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION=%q\n' "${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}"
    printf 'export CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS=%q\n' "${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS}"
    printf 'export CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC=%q\n' "${CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC}"
    printf 'export CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS=%q\n' "${CLIENT_APPEND_STATS_INTERVAL_EVENTS}"
    printf 'timeout 900s python3 %q --config %q --result-dir %q --operation-count %q --message-size-bytes %q --node-count %q --client-count %q --archive-wait-seconds %q --archive-event-count-poll-interval-seconds %q --completion-mode %q\n' \
      "${SCRIPT_DIR}/chronolog_append_durable.py" \
      "${CLIENT_CONF_FILE}" \
      "${RESULT_DIR}" \
      "${OPERATION_COUNT}" \
      "${MESSAGE_SIZE_BYTES}" \
      "${NODE_COUNT}" \
      "${CLIENT_COUNT}" \
      "${ARCHIVE_WAIT_SECONDS}" \
      "${ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS}" \
      "${COMPLETION_MODE}"
  } > "${CLIENT_COMMAND}"
  chmod +x "${CLIENT_COMMAND}"
  set +e
  env -u LD_LIBRARY_PATH ssh -n "${CLIENT_NODE}" "bash '${CLIENT_COMMAND}'" \
    > "${CHRONOLOG_RESULT_DIR}/chronolog-append-durable.log" \
    2> "${CHRONOLOG_RESULT_DIR}/chronolog-append-durable.stderr.log"
  BENCH_STATUS=$?
  set -e
  if [[ "${BENCH_STATUS}" -ne 0 ]]; then
    exit "${BENCH_STATUS}"
  fi
else
  echo "Unsupported ChronoLog workflow: ${WORKFLOW}" >&2
  exit 2
fi
wait_ebpf_collectors
cleanup

generate_perf_reports() {
  if [[ "${PROFILE_MODE}" != "perf" || -z "${PERF_BIN}" || ! -x "${PERF_BIN}" ]]; then
    return 0
  fi
  while IFS= read -r -d '' perf_data; do
    local report_path="${perf_data%.perf.data}.perf-report.txt"
    local flat_report_path="${perf_data%.perf.data}.perf-report-flat.txt"
    local report_stderr="${perf_data%.perf.data}.perf-report.stderr.log"
    local flat_report_stderr="${perf_data%.perf.data}.perf-report-flat.stderr.log"
    local report_timeout="${CHRONOLOG_PERF_REPORT_TIMEOUT_SECONDS:-30}"
    if [[ -s "${perf_data}" ]]; then
      timeout "${report_timeout}s" "${PERF_BIN}" report --stdio --no-children --call-graph none -i "${perf_data}" > "${flat_report_path}" 2> "${flat_report_stderr}" || true
      timeout "${report_timeout}s" "${PERF_BIN}" report --stdio -i "${perf_data}" > "${report_path}" 2> "${report_stderr}" || true
    fi
  done < <(find "${CHRONOLOG_RESULT_DIR}/profiles" -type f -name '*.perf.data' -print0 2>/dev/null)
}

generate_perf_reports
collect_node_local_journals() {
  if [[ "${KEEPER_JOURNAL_MODE}" == "disabled" || "${KEEPER_JOURNAL_PLACEMENT}" != "node_local" ]]; then
    return 0
  fi
  mkdir -p "${KEEPER_JOURNAL_DIR}"
  while IFS= read -r node; do
    [[ -n "${node}" ]] || continue
    local remote_dir="${KEEPER_JOURNAL_LOCAL_BASE}/${KEEPER_JOURNAL_RUN_ID}/${node}"
    local node_dir="${KEEPER_JOURNAL_DIR}/${node}"
    mkdir -p "${node_dir}"
    env -u LD_LIBRARY_PATH ssh -n "${node}" "bash -lc 'if [[ -d ${remote_dir@Q} ]]; then tar -C ${remote_dir@Q} -cf - .; fi'" \
      | tar -C "${node_dir}" -xf - || true
    if [[ "${CHRONOLOG_KEEPER_JOURNAL_LOCAL_CLEANUP:-0}" == "1" ]]; then
      env -u LD_LIBRARY_PATH ssh -n "${node}" "bash -lc 'rm -rf ${remote_dir@Q}'" || true
    fi
  done < "${CONFIG_DIR}/hosts_keeper"
}
collect_node_local_journals
sleep "${CHRONOLOG_POST_STOP_SLEEP_SECONDS:-2}"

python3 - "${CHRONOLOG_RESULT_DIR}/metrics.json" "${CHRONOLOG_LOG_DIR}" <<'PY'
import json
import os
import re
import struct
import sys
import time
from pathlib import Path

metrics_path = Path(sys.argv[1])
log_dir = Path(sys.argv[2])
if not metrics_path.exists() or not log_dir.exists():
    raise SystemExit(0)

scripts_dir = Path(".agent/scripts").resolve()
if scripts_dir.exists():
    sys.path.insert(0, str(scripts_dir))

stats_re = re.compile(r"\[KeeperAppendStats\]\s+(.*)")
kv_re = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
wait_seconds = float(os.environ.get("CHRONOLOG_KEEPER_APPEND_STATS_WAIT_SECONDS", "10"))

def percentile(values, pct):
    clean = sorted(float(value) for value in values)
    if not clean:
        return 0.0
    if len(clean) == 1:
        return clean[0]
    rank = (pct / 100.0) * (len(clean) - 1)
    lower = int(rank)
    upper = min(lower + 1, len(clean) - 1)
    fraction = rank - lower
    return clean[lower] + (clean[upper] - clean[lower]) * fraction

def parse_keeper_logs():
    per_keeper = []
    orphan_warning_count = 0
    keeper_log_groups = {}
    keeper_log_re = re.compile(r"^chrono-keeper-(?P<host>.+?)(?:\.\d+)?\.log$")
    for log_path in sorted(log_dir.glob("chrono-keeper-*.log")):
        if ".launch." in log_path.name or ".restart." in log_path.name:
            continue
        match = keeper_log_re.match(log_path.name)
        if not match:
            continue
        keeper_log_groups.setdefault(match.group("host"), []).append(log_path)
    for host, host_logs in sorted(keeper_log_groups.items()):
        keeper_stats = None
        for log_path in host_logs:
            for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
                if "[IngestionQueue] Orphan event" in line:
                    orphan_warning_count += 1
                match = stats_re.search(line)
                if not match:
                    continue
                parsed = {"log": str(log_path), "keeper_host": host}
                for key, value in kv_re.findall(match.group(1)):
                    if key == "context":
                        parsed[key] = value
                    else:
                        try:
                            parsed[key] = float(value) if "." in value else int(value)
                        except ValueError:
                            parsed[key] = value
                if "record_event_count" in parsed and (
                    keeper_stats is None
                    or int(parsed.get("record_event_count", 0)) >= int(keeper_stats.get("record_event_count", 0))
                ):
                    keeper_stats = parsed
        if keeper_stats:
            per_keeper.append(keeper_stats)
    keeper_logs = [paths[0] for _, paths in sorted(keeper_log_groups.items())]
    return keeper_logs, per_keeper, orphan_warning_count

def parse_keeper_tail_bulk_stats():
    stats_re = re.compile(r"\[(KeeperTailBulkStats|KeeperTailBulkStreamStats)\]\s+(.*)")
    rows = []
    for log_path in sorted(log_dir.glob("chrono-keeper-*.log")):
        if ".launch." in log_path.name or ".restart." in log_path.name:
            continue
        try:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            stats_match = stats_re.search(line)
            if not stats_match:
                continue
            parsed = {"log": str(log_path), "kind": stats_match.group(1)}
            for key, value in kv_re.findall(stats_match.group(2)):
                try:
                    parsed[key] = float(value) if "." in value else int(value)
                except ValueError:
                    parsed[key] = value
            rows.append(parsed)
    return rows

def refresh_grapher_archive_metrics(metrics):
    if metrics.get("workflow") not in {"append_durable", "archive_range_retrieval"}:
        return
    try:
        from chronolog_append_durable import (
            control_path_profile_metrics,
            grapher_archive_stage_metrics,
            grapher_hdf5_write_profile_metrics,
        )
    except Exception as exc:
        metrics["grapher_archive_stage_postprocess_error"] = str(exc)
        return
    story_specs = []
    for result in metrics.get("per_story_results", []):
        chronicle = result.get("chronicle")
        story = result.get("story")
        if chronicle and story:
            story_specs.append((chronicle, story))
    if not story_specs and metrics.get("chronicle") and metrics.get("story"):
        story_specs.append((metrics["chronicle"], metrics["story"]))
    if story_specs:
        metrics.update(
            grapher_archive_stage_metrics(
                log_dir,
                story_specs,
                metrics.get("release_returned_epoch_seconds"),
            )
        )
    metrics.update(grapher_hdf5_write_profile_metrics(log_dir))
    metrics.update(control_path_profile_metrics(log_dir))
    drain_pattern = re.compile(
        r"Stop story archive drain completed\. StoryId=(?P<story>\d+) "
        r"drainEnabled=(?P<enabled>[01]) drained=(?P<drained>[01]) "
        r"queuedChunks=(?P<queued>\d+) inFlightChunks=(?P<inflight>\d+) "
        r"finalize_us=(?P<finalize>[0-9.]+) drain_wait_us=(?P<wait>[0-9.]+)"
    )
    drain_count = 0
    drain_success_count = 0
    drain_finalize_us = []
    drain_wait_us = []
    queued_nonzero_count = 0
    inflight_nonzero_count = 0
    drain_complete_pattern = re.compile(
        r"\[GrapherRecordingService\] Story drain complete received\. StoryID=(?P<story>\d+) "
        r"completionCount=(?P<count>\d+)"
    )
    drain_complete_count = 0
    drain_complete_max_count = 0
    drain_complete_wait_pattern = re.compile(
        r"Stop drain-complete wait finished\. StoryId=(?P<story>\d+) enabled=1 "
        r"(?:async=(?P<async>[01]) )?expectedKeeperDrains=(?P<expected>\d+) complete=(?P<complete>[01]) "
        r"countBefore=(?P<before>\d+) countAfter=(?P<after>\d+) timeout_ms=(?P<timeout>\d+) "
        r"wait_us=(?P<wait>\d+)"
    )
    drain_complete_wait_count = 0
    drain_complete_wait_success_count = 0
    drain_complete_wait_async_count = 0
    drain_complete_wait_us = []
    async_retire_pattern = re.compile(
        r"Async stop story retirement completed\. StoryId=(?P<story>\d+) "
        r"drainEnabled=(?P<enabled>[01]) drained=(?P<drained>[01]) "
        r"queuedChunks=(?P<queued>\d+) inFlightChunks=(?P<inflight>\d+) "
        r"finalize_us=(?P<finalize>[0-9.]+) drain_wait_us=(?P<drain_wait>[0-9.]+) "
        r"wait_us=(?P<wait>\d+) token=(?P<token>\d+)"
    )
    stop_retire_profile_pattern = re.compile(
        r"\[GrapherStopRetireProfile\] story_id=(?P<story>\d+) async=(?P<async>[01]) "
        r"initial_lock_wait_us=(?P<initial_lock_wait>[0-9.]+) "
        r"initial_lock_hold_us=(?P<initial_lock_hold>[0-9.]+) "
        r"completion_wait_us=(?P<completion_wait>[0-9.]+) "
        r"collect_erase_us=(?P<collect_erase>[0-9.]+) "
        r"async_lock_wait_us=(?P<async_lock_wait>[0-9.]+) "
        r"finalize_us=(?P<finalize>[0-9.]+) "
        r"archive_drain_wait_us=(?P<archive_drain_wait>[0-9.]+) "
        r"total_us=(?P<total>[0-9.]+)"
    )
    async_retire_count = 0
    async_retire_success_count = 0
    async_retire_wait_us = []
    stop_retire_profile_count = 0
    stop_retire_profile_async_count = 0
    stop_retire_initial_lock_wait_us = []
    stop_retire_initial_lock_hold_us = []
    stop_retire_completion_wait_us = []
    stop_retire_collect_erase_us = []
    stop_retire_async_lock_wait_us = []
    stop_retire_finalize_us = []
    stop_retire_archive_drain_wait_us = []
    stop_retire_total_us = []
    for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
        if ".launch." in path.name:
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            match = drain_pattern.search(line)
            if match:
                drain_count += 1
                if match.group("drained") == "1":
                    drain_success_count += 1
                drain_finalize_us.append(float(match.group("finalize")))
                drain_wait_us.append(float(match.group("wait")))
                if int(match.group("queued")) > 0:
                    queued_nonzero_count += 1
                if int(match.group("inflight")) > 0:
                    inflight_nonzero_count += 1
                continue
            complete_match = drain_complete_pattern.search(line)
            if complete_match:
                drain_complete_count += 1
                drain_complete_max_count = max(drain_complete_max_count, int(complete_match.group("count")))
                continue
            wait_match = drain_complete_wait_pattern.search(line)
            if wait_match:
                drain_complete_wait_count += 1
                if wait_match.group("complete") == "1":
                    drain_complete_wait_success_count += 1
                if wait_match.group("async") == "1":
                    drain_complete_wait_async_count += 1
                drain_complete_wait_us.append(float(wait_match.group("wait")))
                continue
            retire_match = async_retire_pattern.search(line)
            if not retire_match:
                profile_match = stop_retire_profile_pattern.search(line)
                if not profile_match:
                    continue
                stop_retire_profile_count += 1
                if profile_match.group("async") == "1":
                    stop_retire_profile_async_count += 1
                stop_retire_initial_lock_wait_us.append(float(profile_match.group("initial_lock_wait")))
                stop_retire_initial_lock_hold_us.append(float(profile_match.group("initial_lock_hold")))
                stop_retire_completion_wait_us.append(float(profile_match.group("completion_wait")))
                stop_retire_collect_erase_us.append(float(profile_match.group("collect_erase")))
                stop_retire_async_lock_wait_us.append(float(profile_match.group("async_lock_wait")))
                stop_retire_finalize_us.append(float(profile_match.group("finalize")))
                stop_retire_archive_drain_wait_us.append(float(profile_match.group("archive_drain_wait")))
                stop_retire_total_us.append(float(profile_match.group("total")))
                continue
            async_retire_count += 1
            if retire_match.group("drained") == "1":
                async_retire_success_count += 1
            async_retire_wait_us.append(float(retire_match.group("wait")))
    metrics["grapher_archive_drain_count"] = drain_count
    metrics["grapher_archive_drain_success_count"] = drain_success_count
    metrics["grapher_archive_drain_finalize_max_us"] = max(drain_finalize_us) if drain_finalize_us else None
    metrics["grapher_archive_drain_wait_max_us"] = max(drain_wait_us) if drain_wait_us else None
    metrics["grapher_archive_drain_wait_total_us"] = sum(drain_wait_us)
    metrics["grapher_archive_drain_queued_nonzero_count"] = queued_nonzero_count
    metrics["grapher_archive_drain_inflight_nonzero_count"] = inflight_nonzero_count
    metrics["grapher_story_drain_complete_count"] = drain_complete_count
    metrics["grapher_story_drain_complete_max_count"] = drain_complete_max_count
    metrics["grapher_stop_drain_complete_wait_count"] = drain_complete_wait_count
    metrics["grapher_stop_drain_complete_wait_success_count"] = drain_complete_wait_success_count
    metrics["grapher_stop_drain_complete_wait_async_count"] = drain_complete_wait_async_count
    metrics["grapher_stop_drain_complete_wait_max_us"] = max(drain_complete_wait_us) if drain_complete_wait_us else None
    metrics["grapher_async_stop_retire_count"] = async_retire_count
    metrics["grapher_async_stop_retire_success_count"] = async_retire_success_count
    metrics["grapher_async_stop_retire_wait_max_us"] = max(async_retire_wait_us) if async_retire_wait_us else None
    metrics["grapher_stop_retire_profile_count"] = stop_retire_profile_count
    metrics["grapher_stop_retire_profile_async_count"] = stop_retire_profile_async_count
    metrics["grapher_stop_retire_initial_lock_wait_max_us"] = (
        max(stop_retire_initial_lock_wait_us) if stop_retire_initial_lock_wait_us else None
    )
    metrics["grapher_stop_retire_initial_lock_hold_max_us"] = (
        max(stop_retire_initial_lock_hold_us) if stop_retire_initial_lock_hold_us else None
    )
    metrics["grapher_stop_retire_completion_wait_max_us"] = (
        max(stop_retire_completion_wait_us) if stop_retire_completion_wait_us else None
    )
    metrics["grapher_stop_retire_collect_erase_max_us"] = (
        max(stop_retire_collect_erase_us) if stop_retire_collect_erase_us else None
    )
    metrics["grapher_stop_retire_async_lock_wait_max_us"] = (
        max(stop_retire_async_lock_wait_us) if stop_retire_async_lock_wait_us else None
    )
    metrics["grapher_stop_retire_finalize_max_us"] = max(stop_retire_finalize_us) if stop_retire_finalize_us else None
    metrics["grapher_stop_retire_archive_drain_wait_max_us"] = (
        max(stop_retire_archive_drain_wait_us) if stop_retire_archive_drain_wait_us else None
    )
    metrics["grapher_stop_retire_total_max_us"] = max(stop_retire_total_us) if stop_retire_total_us else None

metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
workflow = os.environ.get("CHRONOLOG_WORKFLOW", metrics.get("workflow", ""))
completion_mode = os.environ.get("CHRONOLOG_COMPLETION_MODE", "")
metrics["profile_mode"] = os.environ.get("CHRONOLOG_PROFILE_MODE", "none")

def collect_profile_artifact_status(result_dir, profile_mode):
    profile_root = result_dir / "profiles"
    status = {
        "chronolog_profile_artifact_count": 0,
        "chronolog_profile_nonempty_artifact_count": 0,
        "chronolog_profile_error_file_count": 0,
        "chronolog_profile_error_nonempty_count": 0,
        "chronolog_profile_valid": True,
        "chronolog_profile_notes": "",
    }
    if profile_mode == "none":
        status["chronolog_profile_valid"] = True
        status["chronolog_profile_notes"] = "profiling disabled"
        return status
    if not profile_root.exists():
        status["chronolog_profile_valid"] = False
        status["chronolog_profile_notes"] = "profile root missing"
        return status

    artifacts = []
    error_files = []
    for path in profile_root.rglob("*"):
        if not path.is_file():
            continue
        name = path.name
        if name.endswith((".pid", ".env")) or name == "collector-pids.txt":
            continue
        artifacts.append(path)
        if (
            name.endswith((".err", ".stderr", ".stderr.log"))
            or "stderr" in name
            or "error" in name.lower()
        ):
            error_files.append(path)

    nonempty = [path for path in artifacts if path.stat().st_size > 0]
    nonempty_errors = [path for path in error_files if path.stat().st_size > 0]
    status["chronolog_profile_artifact_count"] = len(artifacts)
    status["chronolog_profile_nonempty_artifact_count"] = len(nonempty)
    status["chronolog_profile_error_file_count"] = len(error_files)
    status["chronolog_profile_error_nonempty_count"] = len(nonempty_errors)

    notes = []
    if profile_mode == "perf":
        perf_data = list(profile_root.rglob("*.perf.data"))
        nonempty_perf_data = [path for path in perf_data if path.stat().st_size > 0]
        status["chronolog_perf_data_file_count"] = len(perf_data)
        status["chronolog_perf_nonempty_data_file_count"] = len(nonempty_perf_data)
        status["chronolog_profile_valid"] = bool(nonempty_perf_data)
        if not nonempty_perf_data:
            notes.append("no nonempty perf.data files")
    elif profile_mode == "ebpf":
        ebpf_outputs = [
            path for path in profile_root.rglob("*.txt")
            if path.name.endswith((".runqlat.txt", ".offcputime.txt"))
        ]
        nonempty_ebpf_outputs = [path for path in ebpf_outputs if path.stat().st_size > 0]
        status["chronolog_ebpf_output_file_count"] = len(ebpf_outputs)
        status["chronolog_ebpf_nonempty_output_file_count"] = len(nonempty_ebpf_outputs)
        status["chronolog_profile_valid"] = bool(nonempty_ebpf_outputs)
        if not nonempty_ebpf_outputs:
            notes.append("no nonempty eBPF output files")
    elif profile_mode == "gperftools":
        gperf_outputs = [
            path for path in profile_root.rglob("*")
            if path.is_file() and (".prof" in path.name or ".heap" in path.name)
        ]
        nonempty_gperf_outputs = [path for path in gperf_outputs if path.stat().st_size > 0]
        status["chronolog_gperftools_output_file_count"] = len(gperf_outputs)
        status["chronolog_gperftools_nonempty_output_file_count"] = len(nonempty_gperf_outputs)
        status["chronolog_profile_valid"] = bool(nonempty_gperf_outputs)
        if not nonempty_gperf_outputs:
            notes.append("no nonempty gperftools profile files")
    else:
        status["chronolog_profile_valid"] = bool(nonempty)
        if not nonempty:
            notes.append("no nonempty profile artifacts")

    error_samples = []
    for path in nonempty_errors[:4]:
        try:
            text = path.read_text(encoding="utf-8", errors="replace").strip().splitlines()
        except OSError:
            text = []
        if text:
            error_samples.append(f"{path.relative_to(profile_root)}: {text[0][:120]}")
    if error_samples:
        notes.append("; ".join(error_samples))
    status["chronolog_profile_notes"] = " | ".join(notes)
    if error_samples and profile_mode in {"perf", "ebpf"}:
        status["chronolog_profile_valid"] = False
    return status

metrics.update(collect_profile_artifact_status(metrics_path.parent, metrics["profile_mode"]))
metrics["chronolog_completion_mode"] = completion_mode
if workflow in {"append_throughput", "append_latency"}:
    semantic_labels = {
        "keeper_journal_buffered": (
            "append_keeper_journal_buffered",
            "record_event_return_after_keeper_journal_write",
            "keeper_local_journal_buffered_not_fsync",
        ),
        "keeper_journal_fdatasync": (
            "append_keeper_journal_fdatasync",
            "record_event_return_after_keeper_journal_fdatasync",
            "keeper_local_journal_fdatasync",
        ),
        "keeper_journal_fdatasync_tail_only": (
            "append_keeper_journal_fdatasync_tail_only",
            "record_event_return_after_keeper_journal_fdatasync_no_timeline_ingest",
            "keeper_local_journal_fdatasync",
        ),
        "keeper_journal_group_fdatasync": (
            "append_keeper_journal_periodic_fdatasync",
            "record_event_return_after_keeper_journal_write_group_fsync_periodic",
            "keeper_local_journal_periodic_fdatasync_not_per_record",
        ),
        "keeper_journal_group_fdatasync_early_ack": (
            "append_keeper_journal_periodic_fdatasync_early_ack",
            "record_event_return_after_keeper_journal_write_periodic_fsync_before_ingestion_drain",
            "keeper_local_journal_periodic_fdatasync_not_per_record",
        ),
        "keeper_journal_group_fdatasync_async_drain": (
            "append_keeper_journal_periodic_fdatasync_async_drain",
            "record_event_return_after_keeper_journal_write_periodic_fsync_and_memory_drain_enqueue",
            "keeper_local_journal_periodic_fdatasync_not_per_record",
        ),
        "keeper_journal_group_fdatasync_wal_drain": (
            "append_keeper_journal_periodic_fdatasync_wal_drain",
            "record_event_return_after_keeper_journal_write_periodic_fsync_and_wal_cursor_enqueue",
            "keeper_local_journal_periodic_fdatasync_not_per_record",
        ),
        "keeper_journal_group_fdatasync_tail_only": (
            "append_keeper_journal_periodic_fdatasync_tail_only",
            "record_event_return_after_keeper_journal_write_periodic_fsync_no_timeline_ingest",
            "keeper_local_journal_periodic_fdatasync_not_per_record",
        ),
        "keeper_journal_group_commit_tail_only": (
            "append_keeper_journal_group_commit_tail_only",
            "record_event_return_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest",
            "keeper_local_journal_group_commit_fdatasync",
        ),
        "keeper_journal_group_commit_deferred_tail_only": (
            "append_keeper_journal_group_commit_deferred_rpc_tail_only",
            "record_event_response_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest",
            "keeper_local_journal_group_commit_fdatasync",
        ),
    }
    semantic_boundary, append_ack_boundary, durability_boundary = semantic_labels.get(
        completion_mode,
        (
            "append_enqueue_async",
            "chrono_bench_record_event_return",
            "not_proven_durable",
        ),
    )
    metrics["semantic_boundary"] = semantic_boundary
    metrics["append_ack_boundary"] = append_ack_boundary
    metrics["durability_boundary"] = durability_boundary
    if completion_mode.startswith("keeper_journal_"):
        metrics["storage_backend"] = "chronolog_keeper_local_journal"
    else:
        metrics["storage_backend"] = "not_proven_durable"
else:
    workflow = metrics.get("workflow")
    metrics["keeper_tail_source"] = os.environ.get("CHRONOLOG_KEEPER_TAIL_SOURCE", "timeline")
    if workflow == "mixed_append_tail" and metrics.get("keeper_tail_source") == "journal":
        mixed_tail_mode = metrics.get("tail_read_mode") or os.environ.get("CHRONOLOG_MIXED_TAIL_READ_MODE", "full")
        mixed_tail_start = metrics.get("tail_reader_start_mode") or os.environ.get(
            "CHRONOLOG_MIXED_TAIL_READER_START_MODE",
            "ready",
        )
        metrics["semantic_boundary"] = (
            f"mixed_append_concurrent_keeper_journal_tail_{mixed_tail_mode}"
            if mixed_tail_start == "ready"
            else f"append_then_keeper_journal_tail_catchup_{mixed_tail_mode}"
        )
        durable_before_publish = os.environ.get(
            "CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH",
            "0",
        )
        metrics["append_ack_boundary"] = (
            "record_event_response_after_keeper_journal_group_commit_fdatasync_before_tail_publish"
            if durable_before_publish == "1"
            else "record_event_response_after_keeper_journal_group_commit_fdatasync_after_tail_publish"
        )
        metrics["durability_boundary"] = "keeper_local_journal_group_commit_fdatasync"
        metrics["read_path"] = (
            "concurrent_keeper_local_journal_tail"
            if mixed_tail_start == "ready"
            else "keeper_local_journal_tail_catchup_after_append"
        )
        metrics["storage_backend"] = "chronolog_keeper_local_journal"
        metrics["semantic_notes"] = (
            "ChronoLog mixed_append_tail uses Keeper-local journal tail records. If "
            "keeper_journal_durable_complete_before_publish=1, append acknowledgment is WAL-durable before "
            "tail-index publication, so this row is tail catch-up/readability evidence rather than immediate "
            "tail-visibility evidence. For catch-up comparisons, use tail_retrieval_throughput_ops_per_sec or "
            "tail_rpc_active_throughput_ops_per_sec, not whole-workflow throughput."
        )
    elif workflow == "mixed_append_tail":
        mixed_tail_mode = metrics.get("tail_read_mode") or os.environ.get("CHRONOLOG_MIXED_TAIL_READ_MODE", "full")
        mixed_tail_start = metrics.get("tail_reader_start_mode") or os.environ.get(
            "CHRONOLOG_MIXED_TAIL_READER_START_MODE",
            "ready",
        )
        metrics["semantic_boundary"] = (
            f"mixed_append_concurrent_live_tail_{mixed_tail_mode}"
            if mixed_tail_start == "ready"
            else f"append_then_live_tail_catchup_{mixed_tail_mode}"
        )
        metrics["append_ack_boundary"] = "chrono_bench_record_event_return"
        metrics["durability_boundary"] = "not_proven_durable"
        metrics["read_path"] = (
            "concurrent_live_tail"
            if mixed_tail_start == "ready"
            else "live_tail_catchup_after_append"
        )
        metrics["storage_backend"] = "not_proven_durable"
        metrics["semantic_notes"] = (
            "ChronoLog mixed_append_tail uses the live ReplayStory/tail path with no durable/storage "
            "completion proof. For catch-up comparisons, use tail_retrieval_throughput_ops_per_sec or "
            "tail_rpc_active_throughput_ops_per_sec, not whole-workflow throughput."
        )
    if workflow == "mixed_append_tail":
        metrics["chronolog_mixed_tail_writer_release_story"] = os.environ.get(
            "CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY",
            "yes",
        )
producer_outstanding = int(os.environ.get("CHRONOLOG_PRODUCER_OUTSTANDING", "1"))
producer_batch_size = int(os.environ.get("CHRONOLOG_PRODUCER_BATCH_SIZE", "1"))
producer_wait_policy = os.environ.get("CHRONOLOG_PRODUCER_WAIT_MODE", "auto")
metrics["chronolog_producer_outstanding"] = producer_outstanding
metrics["chronolog_producer_batch_size"] = producer_batch_size
metrics["chronolog_producer_wait_policy"] = producer_wait_policy
metrics["chronolog_bench_bound_keeper_futures"] = "1" if producer_wait_policy == "bounded_futures" else "0"
metrics["chronolog_bench_owned_batch"] = os.environ.get("CHRONOLOG_BENCH_OWNED_BATCH", "0")
metrics["chronolog_bench_owned_batch_min_message_size"] = os.environ.get(
    "CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE", "0"
)
metrics["chronolog_client_batch_keeper_selection"] = os.environ.get(
    "CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION", "per_event"
)
metrics["chronolog_client_keeper_time_bucket_ns"] = os.environ.get("CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS", "0")
metrics["chronolog_client_parallel_tail_rpc"] = os.environ.get("CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC", "1")
metrics["chronolog_client_keeper_cursor_drain"] = os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN", "0")
metrics["chronolog_client_keeper_cursor_drain_max_batches"] = os.environ.get(
    "CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES", "0"
)
metrics["chronolog_client_keeper_cursor_packed_batch"] = os.environ.get(
    "CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH", "0"
)
metrics["chronolog_client_keeper_cursor_metadata_only_output"] = os.environ.get(
    "CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT", "0"
)
metrics["chronolog_client_keeper_cursor_packed_bulk"] = os.environ.get(
    "CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK", "0"
)
metrics["chronolog_client_keeper_cursor_packed_bulk_stream"] = os.environ.get(
    "CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM", "0"
)
metrics["chronolog_client_keeper_cursor_packed_bulk_stream_max_batches"] = os.environ.get(
    "CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES", "0"
)
metrics["chronolog_client_keeper_cursor_packed_bulk_buffer_bytes"] = os.environ.get(
    "CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES",
    os.environ.get("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES", "0"),
)
metrics["chronolog_keeper_tail_packed_bulk_uninitialized_buffer"] = os.environ.get(
    "CHRONOLOG_KEEPER_TAIL_PACKED_BULK_UNINITIALIZED_BUFFER", "0"
)
metrics["chronolog_keeper_tail_packed_bulk_chunk_bytes"] = os.environ.get(
    "CHRONOLOG_KEEPER_TAIL_PACKED_BULK_CHUNK_BYTES", "0"
)
metrics["chronolog_keeper_tail_packed_bulk_parallel_transfer"] = os.environ.get(
    "CHRONOLOG_KEEPER_TAIL_PACKED_BULK_PARALLEL_TRANSFER", "0"
)
metrics["chronolog_mixed_tail_read_mode"] = os.environ.get("CHRONOLOG_MIXED_TAIL_READ_MODE", "full")
metrics["chronolog_mixed_tail_reader_start_mode"] = os.environ.get("CHRONOLOG_MIXED_TAIL_READER_START_MODE", "ready")
metrics["chronolog_client_execution_mode"] = os.environ.get("CHRONOLOG_CLIENT_EXECUTION_MODE", "sequential")
metrics["chronolog_rpc_port_offset"] = os.environ.get("CHRONOLOG_RPC_PORT_OFFSET", "0")
metrics["chronolog_install_dir"] = os.environ.get("CHRONOLOG_INSTALL_DIR_EFFECTIVE", "")
metrics["chronolog_bin_dir"] = os.environ.get("CHRONOLOG_BIN_DIR_EFFECTIVE", "")
metrics["chronolog_deploy_script"] = os.environ.get("CHRONOLOG_DEPLOY_SCRIPT_EFFECTIVE", "")
metrics["chronolog_benchmark_bin"] = os.environ.get("CHRONOLOG_BENCHMARK_BIN_EFFECTIVE", "")
metrics["chronolog_client_append_stats_interval_events"] = os.environ.get(
    "CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS", ""
)
metrics["chronolog_data_collection_poll_interval_us"] = os.environ.get("CHRONOLOG_DATA_COLLECTION_POLL_INTERVAL_US", "")
metrics["chronolog_startup_ready_timeout_seconds"] = os.environ.get(
    "CHRONOLOG_STARTUP_READY_TIMEOUT_SECONDS_EFFECTIVE", ""
)
metrics["chronolog_archive_event_count_poll_interval_seconds"] = os.environ.get(
    "CHRONOLOG_ARCHIVE_EVENT_COUNT_POLL_INTERVAL_SECONDS", ""
)
metrics["chronolog_archive_event_count_wait_mode"] = os.environ.get(
    "CHRONOLOG_ARCHIVE_EVENT_COUNT_WAIT_MODE", "inline"
)
metrics["chronolog_archive_readback_mode"] = os.environ.get("CHRONOLOG_ARCHIVE_READBACK_MODE", "inline")
metrics["chronolog_keeper_data_collection_streams"] = os.environ.get("CHRONOLOG_KEEPER_DATA_COLLECTION_STREAMS", "3")
metrics["chronolog_keeper_data_collection_threads_per_stream"] = os.environ.get(
    "CHRONOLOG_KEEPER_DATA_COLLECTION_THREADS_PER_STREAM", "2"
)
metrics["chronolog_grapher_data_collection_streams"] = os.environ.get("CHRONOLOG_GRAPHER_DATA_COLLECTION_STREAMS", "3")
metrics["chronolog_grapher_data_collection_threads_per_stream"] = os.environ.get(
    "CHRONOLOG_GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM", "2"
)
metrics["chronolog_grapher_inactive_story_delay_seconds"] = os.environ.get(
    "CHRONOLOG_GRAPHER_INACTIVE_STORY_DELAY_SECONDS_EFFECTIVE", ""
)
metrics["chronolog_grapher_retire_on_stop"] = os.environ.get("CHRONOLOG_GRAPHER_RETIRE_ON_STOP", "0")
metrics["chronolog_grapher_stop_retire_grace_us"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US", "0"
)
metrics["chronolog_grapher_stop_story_archive_drain"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN", "0"
)
metrics["chronolog_grapher_stop_story_archive_drain_timeout_ms"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS", "30000"
)
metrics["chronolog_grapher_stop_drain_complete_wait"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT", "0"
)
metrics["chronolog_grapher_stop_drain_complete_wait_timeout_ms"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS", "30000"
)
metrics["chronolog_grapher_stop_drain_complete_wait_async"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC", "0"
)
metrics["chronolog_grapher_stop_drain_wait_outside_lock"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK", "0"
)
metrics["chronolog_grapher_extraction_threads"] = os.environ.get("CHRONOLOG_GRAPHER_EXTRACTION_THREADS", "2")
metrics["chronolog_hdf5_archive_atomic_rename"] = os.environ.get("CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME", "0")
metrics["chronolog_hdf5_archive_chunk_events"] = os.environ.get("CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS", "0")
metrics["chronolog_hdf5_archive_layout"] = os.environ.get("CHRONOLOG_HDF5_ARCHIVE_LAYOUT", "vlen")
metrics["chronolog_raw_blob_preallocate"] = os.environ.get("CHRONOLOG_RAW_BLOB_PREALLOCATE", "0")
metrics["chronolog_raw_blob_async_write"] = os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_WRITE", "0")
metrics["chronolog_raw_blob_async_close"] = os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_CLOSE", "0")
metrics["chronolog_raw_blob_async_publish"] = os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH", "0")
metrics["chronolog_raw_blob_async_publish_threads"] = os.environ.get(
    "CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS", "1"
)
metrics["chronolog_raw_blob_publish_before_close"] = os.environ.get(
    "CHRONOLOG_RAW_BLOB_PUBLISH_BEFORE_CLOSE", "0"
)
metrics["chronolog_raw_blob_fast_reserve"] = os.environ.get("CHRONOLOG_RAW_BLOB_FAST_RESERVE", "0")
metrics["chronolog_raw_blob_sidecar_meta"] = os.environ.get("CHRONOLOG_RAW_BLOB_SIDECAR_META", "0")
metrics["chronolog_perf_event"] = os.environ.get("CHRONOLOG_PERF_EVENT", "cycles")
metrics["chronolog_perf_call_graph"] = os.environ.get("CHRONOLOG_PERF_CALL_GRAPH", "fp")
if producer_wait_policy == "after_loop":
    metrics["chronolog_producer_wait_mode"] = (
        "after_loop_wait" if producer_batch_size <= 1 else f"after_loop_wait_batch_{producer_batch_size}_records"
    )
elif producer_wait_policy == "bounded_api":
    metrics["chronolog_producer_wait_mode"] = (
        f"bounded_api_wait_after_{producer_outstanding}_calls_batch_{producer_batch_size}"
    )
elif producer_wait_policy == "bounded_per_keeper":
    metrics["chronolog_producer_wait_mode"] = (
        f"bounded_per_keeper_wait_after_{producer_outstanding}_keeper_futures_batch_{producer_batch_size}"
    )
elif producer_wait_policy == "bounded_futures":
    metrics["chronolog_producer_wait_mode"] = (
        f"bounded_futures_wait_after_{producer_outstanding}_keeper_futures_batch_{producer_batch_size}"
    )
elif producer_wait_policy == "per_event" or (producer_wait_policy == "auto" and producer_outstanding <= 1):
    metrics["chronolog_producer_wait_mode"] = (
        "per_event_wait" if producer_batch_size <= 1 else f"batch_wait_after_{producer_batch_size}_records"
    )
else:
    metrics["chronolog_producer_wait_mode"] = (
        f"bounded_outstanding_wait_after_{producer_outstanding}_calls_batch_{producer_batch_size}"
    )
expected_record_event_count = int(
    metrics.get("total_operation_count")
    or (int(metrics.get("operation_count") or 0) * int(metrics.get("client_count") or 1))
)
keeper_journal_dir = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_DIR", "")
keeper_journal_mode = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_MODE", "disabled")
metrics["client_mpi_oversubscribe"] = os.environ.get("CHRONOLOG_CLIENT_MPI_OVERSUBSCRIBE", "0")
metrics["keeper_journal_ack_before_ingest"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ACK_BEFORE_INGEST", "0")
metrics["keeper_journal_async_drain"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN", "0")
metrics["keeper_journal_async_drain_source"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_SOURCE", "memory")
metrics["keeper_journal_async_drain_threads"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_THREADS", "1")
metrics["keeper_journal_skip_ingest"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_SKIP_INGEST", "0")
metrics["keeper_journal_defer_rpc_response"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_DEFER_RPC_RESPONSE", "0")
metrics["keeper_journal_callback_batch_drain"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN", "0")
metrics["keeper_journal_callback_batch_drain_max"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX", "0"
)
metrics["keeper_journal_callback_batch_drain_min_payload_bytes"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES", "0"
)
metrics["keeper_recording_margo_xstreams"] = os.environ.get("CHRONOLOG_KEEPER_RECORDING_MARGO_XSTREAMS", "1")
metrics["keeper_recording_margo_progress_thread"] = os.environ.get(
    "CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD", "1"
)
metrics["keeper_recording_margo_handlers"] = os.environ.get("CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS", "1")
metrics["grapher_recording_margo_xstreams"] = os.environ.get("CHRONOLOG_GRAPHER_RECORDING_MARGO_XSTREAMS", "1")
metrics["grapher_recording_margo_handlers"] = os.environ.get("CHRONOLOG_GRAPHER_RECORDING_MARGO_HANDLERS", "1")
metrics["grapher_direct_deserialize"] = os.environ.get("CHRONOLOG_GRAPHER_DIRECT_DESERIALIZE", "0")
metrics["keeper_drain_margo_progress_thread"] = os.environ.get("CHRONOLOG_KEEPER_DRAIN_MARGO_PROGRESS_THREAD", "0")
metrics["keeper_drain_margo_rpc_threads"] = os.environ.get("CHRONOLOG_KEEPER_DRAIN_MARGO_RPC_THREADS", "0")
metrics["keeper_extraction_threads"] = os.environ.get("CHRONOLOG_KEEPER_EXTRACTION_THREADS", "2")
metrics["keeper_direct_serialize"] = os.environ.get("CHRONOLOG_KEEPER_DIRECT_SERIALIZE", "0")
metrics["keeper_fast_wire"] = os.environ.get("CHRONOLOG_KEEPER_FAST_WIRE", "0")
metrics["keeper_journal_mode"] = keeper_journal_mode
metrics["keeper_journal_dir"] = keeper_journal_dir
metrics["keeper_journal_placement"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_PLACEMENT", "shared")
metrics["keeper_journal_local_base"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_LOCAL_BASE", "")
metrics["keeper_journal_local_preclean"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_LOCAL_PRECLEAN", "1")
metrics["keeper_journal_run_id"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_RUN_ID", "")
metrics["keeper_journal_shards"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_SHARDS", "1")
metrics["keeper_journal_shard_policy"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_SHARD_POLICY", "mixed")
metrics["keeper_journal_fdatasync_batch_events"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS", "64")
metrics["keeper_journal_writev"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_WRITEV", "0")
metrics["keeper_journal_batch_writev"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV", "0")
metrics["keeper_journal_move_batch_payloads"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS", "0")
metrics["keeper_journal_tail_read_mode"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_TAIL_READ_MODE", "auto")
metrics["keeper_journal_tail_direct_read_into_events"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_TAIL_DIRECT_READ_INTO_EVENTS", "0"
)
metrics["keeper_tail_batch_max_events"] = os.environ.get("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_EVENTS", "0")
metrics["keeper_tail_batch_max_bytes"] = os.environ.get("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES", "0")
metrics["keeper_tail_cursor_overlap_ns"] = os.environ.get("CHRONOLOG_KEEPER_TAIL_CURSOR_OVERLAP_NS", "0")
metrics["keeper_journal_atomic_offsets"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ATOMIC_OFFSETS", "0")
metrics["keeper_journal_single_writer"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER", "0")
metrics["keeper_journal_single_writer_batch_events"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_SINGLE_WRITER_BATCH_EVENTS", "64")
metrics["keeper_journal_group_commit_wait"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT", "0")
metrics["keeper_journal_group_commit_flush_events"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS", "1"
)
metrics["keeper_journal_group_commit_flush_bytes"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES", "0"
)
metrics["keeper_journal_group_commit_strict_flush_event_cap"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP", "0"
)
metrics["keeper_journal_group_commit_large_payload_bytes"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES", "0"
)
metrics["keeper_journal_group_commit_large_payload_flush_events"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS", "64"
)
metrics["keeper_journal_group_commit_wait_us"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US", "0")
metrics["keeper_journal_group_commit_flush_wait_us"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US", "0"
)
metrics["keeper_journal_group_commit_queue_boundary_min_events"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_EVENTS", "0"
)
metrics["keeper_journal_group_commit_queue_boundary_min_payload_bytes"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_MIN_PAYLOAD_BYTES", "0"
)
metrics["keeper_journal_group_commit_queue_boundary_wait_every_n"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_QUEUE_BOUNDARY_WAIT_EVERY_N", "1"
)
metrics["keeper_journal_group_commit_wait_for_active_admissions"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_FOR_ACTIVE_ADMISSIONS", "0"
)
metrics["keeper_journal_owner_drain_yields"] = os.environ.get("CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS", "0")
metrics["keeper_journal_notify_owner_only_on_empty"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY", "0"
)
metrics["keeper_journal_async_callback_dispatch"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH", "0"
)
metrics["keeper_journal_async_batch_completion_dispatch"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH", "1"
)
metrics["keeper_journal_callback_dispatch_threads"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS", "1"
)
metrics["keeper_journal_durable_complete_before_publish"] = os.environ.get(
    "CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH", "0"
)
metrics["keeper_append_stats_interval_events"] = os.environ.get("CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS", "10000")
metrics["keeper_stop_story_stats_wait_ms"] = os.environ.get("CHRONOLOG_KEEPER_STOP_STORY_STATS_WAIT_MS", "0")
metrics["keeper_stop_story_flush_drain"] = os.environ.get("CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN", "0")
metrics["keeper_stop_story_flush_drain_timeout_ms"] = os.environ.get(
    "CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS", "30000"
)
metrics["keeper_stop_story_flush_drain_async_complete"] = os.environ.get(
    "CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_ASYNC_COMPLETE", "0"
)
metrics["keeper_restart_pre_crash_stats_wait_seconds"] = os.environ.get(
    "CHRONOLOG_KEEPER_RESTART_PRE_CRASH_STATS_WAIT_SECONDS", "2"
)
metrics["visor_parallel_keeper_stop"] = os.environ.get("CHRONOLOG_VISOR_PARALLEL_KEEPER_STOP", "0")
metrics["visor_parallel_recording_stop"] = os.environ.get("CHRONOLOG_VISOR_PARALLEL_RECORDING_STOP", "0")
metrics["grapher_retire_on_stop"] = os.environ.get("CHRONOLOG_GRAPHER_RETIRE_ON_STOP", "0")
metrics["grapher_stop_story_archive_drain"] = os.environ.get("CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN", "0")
metrics["grapher_stop_story_archive_drain_timeout_ms"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS", "30000"
)
metrics["grapher_stop_drain_complete_wait"] = os.environ.get("CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT", "0")
metrics["grapher_stop_drain_complete_wait_timeout_ms"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_TIMEOUT_MS", "30000"
)
metrics["grapher_stop_drain_complete_wait_async"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_DRAIN_COMPLETE_WAIT_ASYNC", "0"
)
metrics["grapher_stop_drain_wait_outside_lock"] = os.environ.get(
    "CHRONOLOG_GRAPHER_STOP_DRAIN_WAIT_OUTSIDE_LOCK", "0"
)
metrics["grapher_extraction_threads"] = os.environ.get("CHRONOLOG_GRAPHER_EXTRACTION_THREADS", "2")
metrics["hdf5_archive_atomic_rename"] = os.environ.get("CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME", "0")
metrics["hdf5_archive_chunk_events"] = os.environ.get("CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS", "0")
metrics["hdf5_archive_layout"] = os.environ.get("CHRONOLOG_HDF5_ARCHIVE_LAYOUT", "vlen")
metrics["raw_blob_preallocate"] = os.environ.get("CHRONOLOG_RAW_BLOB_PREALLOCATE", "0")
metrics["raw_blob_async_write"] = os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_WRITE", "0")
metrics["raw_blob_async_close"] = os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_CLOSE", "0")
metrics["raw_blob_async_publish"] = os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH", "0")
metrics["raw_blob_async_publish_threads"] = os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS", "1")
metrics["raw_blob_publish_before_close"] = os.environ.get("CHRONOLOG_RAW_BLOB_PUBLISH_BEFORE_CLOSE", "0")
metrics["raw_blob_fast_reserve"] = os.environ.get("CHRONOLOG_RAW_BLOB_FAST_RESERVE", "0")
metrics["raw_blob_sidecar_meta"] = os.environ.get("CHRONOLOG_RAW_BLOB_SIDECAR_META", "0")
metrics["deploy_stop_timeout_seconds"] = os.environ.get("CHRONOLOG_DEPLOY_STOP_TIMEOUT_SECONDS_EFFECTIVE", "")
deploy_stop_log = log_dir / "deploy-stop.log"
if deploy_stop_log.exists():
    deploy_stop_text = deploy_stop_log.read_text(encoding="utf-8", errors="replace")
    metrics["deploy_stop_timed_out"] = "deploy stop timed out" in deploy_stop_text.lower()
else:
    metrics["deploy_stop_timed_out"] = None

def parse_client_append_stats():
    stats_re = re.compile(r"\[ClientAppendStats\]\s+(.*)")
    kv_re = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
    per_pid = {}
    for log_path in sorted(log_dir.glob("*.log")):
        try:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            stats_match = stats_re.search(line)
            if not stats_match:
                continue
            parsed = {"log": str(log_path)}
            for key, value in kv_re.findall(stats_match.group(1)):
                if key == "context":
                    parsed[key] = value
                    continue
                try:
                    parsed[key] = float(value) if "." in value else int(value)
                except ValueError:
                    parsed[key] = value
            pid = str(parsed.get("pid") or parsed.get("log"))
            current = per_pid.get(pid)
            if current is None or int(parsed.get("event_count", 0)) >= int(current.get("event_count", 0)):
                per_pid[pid] = parsed
    return list(per_pid.values())

client_append_stats = parse_client_append_stats()
if client_append_stats:
    def client_total(key):
        return sum(float(item.get(key, 0) or 0) for item in client_append_stats)

    client_event_count = client_total("event_count")
    client_batch_count = client_total("batch_count")
    client_future_wait_count = client_total("future_wait_count")
    client_summary = {
        "client_process_count_with_append_stats": len(client_append_stats),
        "client_append_batch_count": int(client_batch_count),
        "client_append_event_count": int(client_event_count),
        "client_append_payload_bytes": int(client_total("payload_bytes")),
        "client_append_future_count": int(client_total("future_count")),
        "client_append_future_wait_count": int(client_future_wait_count),
    }
    for key in ("event_build_avg_us", "keeper_select_avg_us"):
        client_summary[f"client_append_{key}"] = (
            sum(float(item.get(key, 0) or 0) * float(item.get("event_count", 0) or 0)
                for item in client_append_stats)
            / client_event_count
            if client_event_count
            else 0.0
        )
    client_summary["client_append_rpc_submit_avg_us"] = (
        sum(float(item.get("rpc_submit_avg_us", 0) or 0) * float(item.get("batch_count", 0) or 0)
            for item in client_append_stats)
        / client_batch_count
        if client_batch_count
        else 0.0
    )
    client_summary["client_append_rpc_submit_max_us"] = max(
        float(item.get("rpc_submit_max_us", 0) or 0) for item in client_append_stats
    )
    client_summary["client_append_future_wait_avg_us"] = (
        sum(float(item.get("future_wait_avg_us", 0) or 0) * float(item.get("future_wait_count", 0) or 0)
            for item in client_append_stats)
        / client_future_wait_count
        if client_future_wait_count
        else 0.0
    )
    client_summary["client_append_future_wait_max_us"] = max(
        float(item.get("future_wait_max_us", 0) or 0) for item in client_append_stats
    )
    client_summary["client_append_expected_event_count"] = expected_record_event_count
    client_summary["client_append_event_count_matches_total"] = (
        expected_record_event_count == 0 or int(client_event_count) == expected_record_event_count
    )
    metrics.update(client_summary)
    metrics["client_append_stats"] = {
        "summary": client_summary,
        "per_process": client_append_stats,
    }

def parse_client_tail_rpc_stats():
    stats_re = re.compile(r"\[ClientTailRpcStats\]\s+(.*)")
    client_log_dir = metrics_path.parent
    rows = []
    for log_path in sorted(client_log_dir.glob("*.log")):
        if "mixed-append-tail" not in log_path.name and "range-retrieval" not in log_path.name:
            continue
        try:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            stats_match = stats_re.search(line)
            if not stats_match:
                continue
            parsed = {"log": str(log_path)}
            for key, value in kv_re.findall(stats_match.group(1)):
                try:
                    parsed[key] = float(value) if "." in value else int(value)
                except ValueError:
                    parsed[key] = value
            rows.append(parsed)
    return rows

client_tail_rpc_stats = parse_client_tail_rpc_stats()
if client_tail_rpc_stats:
    tail_rpc_count = len(client_tail_rpc_stats)
    tail_rpc_payload_bytes = sum(int(item.get("payload_bytes", 0) or 0) for item in client_tail_rpc_stats)
    tail_rpc_event_count = sum(int(item.get("event_count", 0) or 0) for item in client_tail_rpc_stats)
    tail_rpc_us = sum(float(item.get("rpc_us", 0) or 0) for item in client_tail_rpc_stats)
    tail_rpc_max_us = max(float(item.get("rpc_us", 0) or 0) for item in client_tail_rpc_stats)
    tail_rpc_success_count = sum(1 for item in client_tail_rpc_stats if int(item.get("ok", 0) or 0) == 1)
    metrics["client_tail_rpc_count"] = tail_rpc_count
    metrics["client_tail_rpc_success_count"] = tail_rpc_success_count
    metrics["client_tail_rpc_event_count"] = tail_rpc_event_count
    metrics["client_tail_rpc_payload_bytes"] = tail_rpc_payload_bytes
    metrics["client_tail_rpc_total_us"] = tail_rpc_us
    metrics["client_tail_rpc_avg_us"] = tail_rpc_us / tail_rpc_count if tail_rpc_count else 0.0
    metrics["client_tail_rpc_max_us"] = tail_rpc_max_us
    tail_rpc_values = [float(item.get("rpc_us", 0) or 0) for item in client_tail_rpc_stats]
    for pct in (50, 95, 99):
        metrics[f"client_tail_rpc_p{pct}_us"] = percentile(tail_rpc_values, pct)
    metrics["client_tail_rpc_payload_mb_per_sec"] = (
        (tail_rpc_payload_bytes / (1024.0 * 1024.0)) / (tail_rpc_us / 1000000.0)
        if tail_rpc_us > 0
        else 0.0
    )
    for key in ("buffer_alloc_us", "bulk_expose_us", "payload_move_us", "total_us"):
        values = [float(item.get(key, 0) or 0) for item in client_tail_rpc_stats]
        metrics[f"client_tail_rpc_{key}_total"] = sum(values)
        metrics[f"client_tail_rpc_{key}_avg"] = sum(values) / tail_rpc_count if tail_rpc_count else 0.0
        metrics[f"client_tail_rpc_{key}_max"] = max(values)
        for pct in (50, 95, 99):
            metrics[f"client_tail_rpc_{key}_p{pct}"] = percentile(values, pct)
    metrics["client_tail_rpc_stats"] = {
        "summary": {
            "count": tail_rpc_count,
            "success_count": tail_rpc_success_count,
            "event_count": tail_rpc_event_count,
            "payload_bytes": tail_rpc_payload_bytes,
            "total_us": tail_rpc_us,
            "avg_us": metrics["client_tail_rpc_avg_us"],
            "p50_us": metrics["client_tail_rpc_p50_us"],
            "p95_us": metrics["client_tail_rpc_p95_us"],
            "p99_us": metrics["client_tail_rpc_p99_us"],
            "max_us": tail_rpc_max_us,
            "payload_mb_per_sec": metrics["client_tail_rpc_payload_mb_per_sec"],
        },
        "per_rpc": client_tail_rpc_stats,
    }

def parse_client_replay_stats():
    stats_re = re.compile(r"\[ClientReplayStats\]\s+(.*)")
    client_log_dir = metrics_path.parent
    rows = []
    for log_path in sorted(client_log_dir.glob("*.log")):
        if "mixed-append-tail" not in log_path.name and "range-retrieval" not in log_path.name:
            continue
        try:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            stats_match = stats_re.search(line)
            if not stats_match:
                continue
            parsed = {"log": str(log_path)}
            for key, value in kv_re.findall(stats_match.group(1)):
                try:
                    parsed[key] = float(value) if "." in value else int(value)
                except ValueError:
                    parsed[key] = value
            rows.append(parsed)
    return rows

client_replay_stats = parse_client_replay_stats()
if client_replay_stats:
    replay_count = len(client_replay_stats)

    def replay_total(key):
        return sum(float(item.get(key, 0) or 0) for item in client_replay_stats)

    def replay_max(key):
        return max(float(item.get(key, 0) or 0) for item in client_replay_stats)

    replay_event_count = int(replay_total("event_count"))
    replay_payload_bytes = int(replay_total("payload_bytes"))
    metrics["client_replay_stats_count"] = replay_count
    metrics["client_replay_event_count"] = replay_event_count
    metrics["client_replay_payload_bytes"] = replay_payload_bytes
    for key in (
        "rpc_collect_us",
        "sort_us",
        "output_build_us",
        "request_attempt_count",
        "request_success_count",
        "request_event_count",
        "request_payload_bytes",
        "request_batch_count",
        "request_sum_us",
        "request_max_us",
    ):
        values = [float(item.get(key, 0) or 0) for item in client_replay_stats]
        metrics[f"client_replay_{key}_avg"] = replay_total(key) / replay_count if replay_count else 0.0
        metrics[f"client_replay_{key}_max"] = replay_max(key)
        for pct in (50, 95, 99):
            metrics[f"client_replay_{key}_p{pct}"] = percentile(values, pct)
    metrics["client_replay_request_sum_us_total"] = replay_total("request_sum_us")
    metrics["client_replay_request_max_us_overall"] = replay_max("request_max_us")
    replay_wall_gaps = [
        float(item.get("rpc_collect_us", 0) or 0) - float(item.get("request_max_us", 0) or 0)
        for item in client_replay_stats
    ]
    metrics["client_replay_request_wall_gap_us_avg"] = (
        sum(replay_wall_gaps) / replay_count if replay_count else 0.0
    )
    metrics["client_replay_request_wall_gap_us_max"] = max(replay_wall_gaps) if replay_wall_gaps else 0.0
    for pct in (50, 95, 99):
        metrics[f"client_replay_request_wall_gap_us_p{pct}"] = percentile(replay_wall_gaps, pct)
    metrics["client_replay_stats"] = {
        "summary": {
            "count": replay_count,
            "event_count": replay_event_count,
            "payload_bytes": replay_payload_bytes,
            "rpc_collect_us_avg": metrics["client_replay_rpc_collect_us_avg"],
            "rpc_collect_us_max": metrics["client_replay_rpc_collect_us_max"],
            "sort_us_avg": metrics["client_replay_sort_us_avg"],
            "sort_us_max": metrics["client_replay_sort_us_max"],
            "output_build_us_avg": metrics["client_replay_output_build_us_avg"],
            "output_build_us_max": metrics["client_replay_output_build_us_max"],
            "request_attempt_count_avg": metrics["client_replay_request_attempt_count_avg"],
            "request_success_count_avg": metrics["client_replay_request_success_count_avg"],
            "request_sum_us_avg": metrics["client_replay_request_sum_us_avg"],
            "request_max_us_avg": metrics["client_replay_request_max_us_avg"],
            "request_wall_gap_us_avg": metrics["client_replay_request_wall_gap_us_avg"],
            "request_max_us_p95": metrics["client_replay_request_max_us_p95"],
            "request_wall_gap_us_p95": metrics["client_replay_request_wall_gap_us_p95"],
        },
        "per_replay": client_replay_stats,
    }
metrics["keeper_wal_drain_batch_events"] = os.environ.get("CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_EVENTS", "1")
metrics["keeper_wal_drain_batch_wait_us"] = os.environ.get("CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_WAIT_US", "0")
if keeper_journal_dir:
    journal_paths = sorted(Path(keeper_journal_dir).rglob("*.journal"))
    metrics["keeper_journal_file_count"] = len(journal_paths)
    metrics["keeper_journal_total_bytes"] = sum(path.stat().st_size for path in journal_paths)
    header_struct = struct.Struct("<QIIQQQIIQ")
    journal_record_count = 0
    journal_payload_bytes = 0
    journal_valid = True
    journal_error = ""
    for journal_path in journal_paths:
        data = journal_path.read_bytes()
        offset = 0
        while offset < len(data):
            if len(data) - offset < header_struct.size:
                journal_valid = False
                journal_error = f"{journal_path}: truncated header at offset {offset}"
                break
            magic, version, header_size, story_id, event_time, client_id, event_index, reserved, payload_size = (
                header_struct.unpack_from(data, offset)
            )
            if magic != 0x43484C4B4A4E4C31 or version != 1 or header_size != header_struct.size:
                journal_valid = False
                journal_error = f"{journal_path}: invalid header at offset {offset}"
                break
            offset += header_size
            if len(data) - offset < payload_size:
                journal_valid = False
                journal_error = f"{journal_path}: truncated payload at offset {offset}"
                break
            offset += payload_size
            journal_record_count += 1
            journal_payload_bytes += payload_size
        if not journal_valid:
            break
    metrics["keeper_journal_record_count"] = journal_record_count
    metrics["keeper_journal_payload_bytes"] = journal_payload_bytes
    metrics["keeper_journal_valid"] = journal_valid
    metrics["keeper_journal_record_count_matches_total"] = (
        expected_record_event_count == 0 or journal_record_count == expected_record_event_count
    )
    if journal_error:
        metrics["keeper_journal_error"] = journal_error
deadline = time.monotonic() + wait_seconds
keeper_logs = []
per_keeper = []
orphan_warning_count = 0
while True:
    keeper_logs, per_keeper, orphan_warning_count = parse_keeper_logs()
    observed = sum(int(item.get("record_event_count", 0)) for item in per_keeper)
    if per_keeper and (expected_record_event_count == 0 or observed >= expected_record_event_count):
        break
    if time.monotonic() >= deadline:
        break
    time.sleep(1.0)

if not per_keeper:
    raise SystemExit(0)

keeper_tail_bulk_stats = parse_keeper_tail_bulk_stats()
if keeper_tail_bulk_stats:
    tail_bulk_count = len(keeper_tail_bulk_stats)
    tail_bulk_payload_bytes = sum(int(item.get("payload_bytes", 0) or 0) for item in keeper_tail_bulk_stats)
    tail_bulk_event_count = sum(int(item.get("event_count", 0) or 0) for item in keeper_tail_bulk_stats)
    tail_bulk_success_count = sum(1 for item in keeper_tail_bulk_stats if int(item.get("ok", 0) or 0) == 1)
    metrics["keeper_tail_bulk_count"] = tail_bulk_count
    metrics["keeper_tail_bulk_success_count"] = tail_bulk_success_count
    metrics["keeper_tail_bulk_event_count"] = tail_bulk_event_count
    metrics["keeper_tail_bulk_payload_bytes"] = tail_bulk_payload_bytes
    for key in (
        "collect_us",
        "bulk_expose_us",
        "bulk_transfer_us",
        "response_us",
        "total_us",
        "batch_count",
        "transfer_count",
        "parallel_transfer",
    ):
        values = [float(item.get(key, 0) or 0) for item in keeper_tail_bulk_stats]
        metrics[f"keeper_tail_bulk_{key}_total"] = sum(values)
        metrics[f"keeper_tail_bulk_{key}_avg"] = sum(values) / tail_bulk_count if tail_bulk_count else 0.0
        metrics[f"keeper_tail_bulk_{key}_max"] = max(values)
        for pct in (50, 95, 99):
            metrics[f"keeper_tail_bulk_{key}_p{pct}"] = percentile(values, pct)
    chunk_values = [int(item.get("chunk_bytes", 0) or 0) for item in keeper_tail_bulk_stats]
    metrics["keeper_tail_bulk_chunk_bytes"] = max(chunk_values) if chunk_values else 0
    stream_rows = [item for item in keeper_tail_bulk_stats if item.get("kind") == "KeeperTailBulkStreamStats"]
    metrics["keeper_tail_bulk_stream_count"] = len(stream_rows)
    metrics["keeper_tail_bulk_stream_batch_count_total"] = sum(
        int(item.get("batch_count", 0) or 0) for item in stream_rows
    )
    transfer_us = metrics.get("keeper_tail_bulk_bulk_transfer_us_total", 0.0)
    metrics["keeper_tail_bulk_payload_mb_per_sec"] = (
        (tail_bulk_payload_bytes / (1024.0 * 1024.0)) / (transfer_us / 1000000.0)
        if transfer_us > 0
        else 0.0
    )
    per_log_tail_bulk = {}
    for item in keeper_tail_bulk_stats:
        log_name = Path(str(item.get("log", "unknown"))).name
        per_log_tail_bulk.setdefault(
            log_name,
            {
                "rpc_count": 0,
                "event_count": 0,
                "payload_bytes": 0,
                "bulk_transfer_us_total": 0.0,
                "total_us_total": 0.0,
            },
        )
        grouped = per_log_tail_bulk[log_name]
        grouped["rpc_count"] += 1
        grouped["event_count"] += int(item.get("event_count", 0) or 0)
        grouped["payload_bytes"] += int(item.get("payload_bytes", 0) or 0)
        grouped["bulk_transfer_us_total"] += float(item.get("bulk_transfer_us", 0) or 0)
        grouped["total_us_total"] += float(item.get("total_us", 0) or 0)
    for grouped in per_log_tail_bulk.values():
        transfer_us_for_log = grouped["bulk_transfer_us_total"]
        grouped["bulk_transfer_payload_mb_per_sec"] = (
            (grouped["payload_bytes"] / (1024.0 * 1024.0)) / (transfer_us_for_log / 1000000.0)
            if transfer_us_for_log > 0
            else 0.0
        )
        grouped["bulk_transfer_us_avg"] = (
            grouped["bulk_transfer_us_total"] / grouped["rpc_count"] if grouped["rpc_count"] else 0.0
        )
        grouped["total_us_avg"] = grouped["total_us_total"] / grouped["rpc_count"] if grouped["rpc_count"] else 0.0
    per_keeper_rows = [
        {"keeper_log": log_name, **values}
        for log_name, values in sorted(per_log_tail_bulk.items())
    ]
    if per_keeper_rows:
        payload_mbps_values = [float(item["bulk_transfer_payload_mb_per_sec"]) for item in per_keeper_rows]
        slowest_transfer = max(per_keeper_rows, key=lambda item: float(item["bulk_transfer_us_total"]))
        slowest_total = max(per_keeper_rows, key=lambda item: float(item["total_us_total"]))
        min_payload_mbps = min(payload_mbps_values)
        max_payload_mbps = max(payload_mbps_values)
        metrics["keeper_tail_bulk_per_keeper_count"] = len(per_keeper_rows)
        metrics["keeper_tail_bulk_per_keeper_payload_mb_per_sec_min"] = min_payload_mbps
        metrics["keeper_tail_bulk_per_keeper_payload_mb_per_sec_max"] = max_payload_mbps
        metrics["keeper_tail_bulk_per_keeper_payload_mb_per_sec_ratio"] = (
            max_payload_mbps / min_payload_mbps if min_payload_mbps > 0 else 0.0
        )
        metrics["keeper_tail_bulk_slowest_transfer_log"] = slowest_transfer["keeper_log"]
        metrics["keeper_tail_bulk_slowest_transfer_us_total"] = slowest_transfer["bulk_transfer_us_total"]
        metrics["keeper_tail_bulk_slowest_total_log"] = slowest_total["keeper_log"]
        metrics["keeper_tail_bulk_slowest_total_us_total"] = slowest_total["total_us_total"]
    metrics["keeper_tail_bulk_stats"] = {
        "summary": {
            "count": tail_bulk_count,
            "success_count": tail_bulk_success_count,
            "event_count": tail_bulk_event_count,
            "payload_bytes": tail_bulk_payload_bytes,
            "payload_mb_per_sec": metrics["keeper_tail_bulk_payload_mb_per_sec"],
            "bulk_transfer_us_p50": metrics["keeper_tail_bulk_bulk_transfer_us_p50"],
            "bulk_transfer_us_p95": metrics["keeper_tail_bulk_bulk_transfer_us_p95"],
            "bulk_transfer_us_p99": metrics["keeper_tail_bulk_bulk_transfer_us_p99"],
            "total_us_p95": metrics["keeper_tail_bulk_total_us_p95"],
            "chunk_bytes": metrics["keeper_tail_bulk_chunk_bytes"],
            "transfer_count_avg": metrics["keeper_tail_bulk_transfer_count_avg"],
            "parallel_transfer_avg": metrics["keeper_tail_bulk_parallel_transfer_avg"],
            "per_keeper_count": metrics.get("keeper_tail_bulk_per_keeper_count", 0),
            "per_keeper_payload_mb_per_sec_min": metrics.get(
                "keeper_tail_bulk_per_keeper_payload_mb_per_sec_min", 0.0
            ),
            "per_keeper_payload_mb_per_sec_max": metrics.get(
                "keeper_tail_bulk_per_keeper_payload_mb_per_sec_max", 0.0
            ),
            "per_keeper_payload_mb_per_sec_ratio": metrics.get(
                "keeper_tail_bulk_per_keeper_payload_mb_per_sec_ratio", 0.0
            ),
            "slowest_transfer_log": metrics.get("keeper_tail_bulk_slowest_transfer_log", ""),
            "slowest_total_log": metrics.get("keeper_tail_bulk_slowest_total_log", ""),
        },
        "per_keeper": per_keeper_rows,
        "per_rpc": keeper_tail_bulk_stats,
    }

def sum_key(key):
    return sum(float(item.get(key, 0)) for item in per_keeper)

def max_key(key):
    return max(float(item.get(key, 0)) for item in per_keeper)

def weighted_avg(avg_key, count_key):
    total_count = sum_key(count_key)
    if total_count == 0:
        return 0.0
    return sum(float(item.get(avg_key, 0)) * float(item.get(count_key, 0)) for item in per_keeper) / total_count

summary = {
    "keeper_count_with_append_stats": len(per_keeper),
    "record_event_count": int(sum_key("record_event_count")),
    "payload_bytes": int(sum_key("payload_bytes")),
    "record_event_avg_us": weighted_avg("record_event_avg_us", "record_event_count"),
    "record_event_max_us": max_key("record_event_max_us"),
    "record_event_le_100us_count": int(sum_key("record_event_le_100us_count")),
    "record_event_le_250us_count": int(sum_key("record_event_le_250us_count")),
    "record_event_le_500us_count": int(sum_key("record_event_le_500us_count")),
    "record_event_le_1000us_count": int(sum_key("record_event_le_1000us_count")),
    "record_event_le_2500us_count": int(sum_key("record_event_le_2500us_count")),
    "record_event_le_5000us_count": int(sum_key("record_event_le_5000us_count")),
    "record_event_le_10000us_count": int(sum_key("record_event_le_10000us_count")),
    "record_event_gt_10000us_count": int(sum_key("record_event_gt_10000us_count")),
    "rpc_batch_phase_count": int(sum_key("rpc_batch_phase_count")),
    "rpc_batch_submit_avg_us": weighted_avg("rpc_batch_submit_avg_us", "rpc_batch_phase_count"),
    "rpc_batch_submit_max_us": max_key("rpc_batch_submit_max_us"),
    "rpc_batch_completion_wait_avg_us": weighted_avg(
        "rpc_batch_completion_wait_avg_us", "rpc_batch_phase_count"
    ),
    "rpc_batch_completion_wait_max_us": max_key("rpc_batch_completion_wait_max_us"),
    "ingestion_queue_count": int(sum_key("ingestion_queue_count")),
    "ingestion_queue_avg_us": weighted_avg("ingestion_queue_avg_us", "ingestion_queue_count"),
    "ingestion_lookup_avg_us": weighted_avg("ingestion_lookup_avg_us", "ingestion_queue_count"),
    "ingestion_queue_max_us": max_key("ingestion_queue_max_us"),
    "orphan_events": int(sum_key("orphan_events")),
    "handle_ingest_count": int(sum_key("handle_ingest_count")),
    "handle_lock_wait_avg_us": weighted_avg("handle_lock_wait_avg_us", "handle_ingest_count"),
    "handle_lock_wait_max_us": max_key("handle_lock_wait_max_us"),
    "handle_lock_hold_avg_us": weighted_avg("handle_lock_hold_avg_us", "handle_ingest_count"),
    "handle_lock_hold_max_us": max_key("handle_lock_hold_max_us"),
    "handle_push_avg_us": weighted_avg("handle_push_avg_us", "handle_ingest_count"),
    "handle_push_max_us": max_key("handle_push_max_us"),
    "handle_queue_depth_max": int(max_key("handle_queue_depth_max")),
    "handle_collect_count": int(sum_key("handle_collect_count")),
    "handle_collect_events": int(sum_key("handle_collect_events")),
    "handle_collect_lock_wait_avg_us": weighted_avg("handle_collect_lock_wait_avg_us", "handle_collect_count"),
    "handle_collect_lock_wait_max_us": max_key("handle_collect_lock_wait_max_us"),
    "handle_collect_lock_hold_avg_us": weighted_avg("handle_collect_lock_hold_avg_us", "handle_collect_count"),
    "handle_collect_lock_hold_max_us": max_key("handle_collect_lock_hold_max_us"),
    "handle_lane_registry_lock_wait_avg_us": weighted_avg(
        "handle_lane_registry_lock_wait_avg_us", "handle_collect_count"
    ),
    "handle_lane_registry_lock_wait_max_us": max_key("handle_lane_registry_lock_wait_max_us"),
    "handle_lane_registry_lock_hold_avg_us": weighted_avg(
        "handle_lane_registry_lock_hold_avg_us", "handle_collect_count"
    ),
    "handle_lane_registry_lock_hold_max_us": max_key("handle_lane_registry_lock_hold_max_us"),
    "journal_append_count": int(sum_key("journal_append_count")),
    "journal_payload_bytes": int(sum_key("journal_payload_bytes")),
    "journal_lock_wait_avg_us": weighted_avg("journal_lock_wait_avg_us", "journal_append_count"),
    "journal_lock_wait_max_us": max_key("journal_lock_wait_max_us"),
    "journal_write_avg_us": weighted_avg("journal_write_avg_us", "journal_append_count"),
    "journal_write_max_us": max_key("journal_write_max_us"),
    "journal_fdatasync_avg_us": weighted_avg("journal_fdatasync_avg_us", "journal_append_count"),
    "journal_fdatasync_max_us": max_key("journal_fdatasync_max_us"),
    "journal_fdatasync_le_100us_count": int(sum_key("journal_fdatasync_le_100us_count")),
    "journal_fdatasync_le_250us_count": int(sum_key("journal_fdatasync_le_250us_count")),
    "journal_fdatasync_le_500us_count": int(sum_key("journal_fdatasync_le_500us_count")),
    "journal_fdatasync_le_1000us_count": int(sum_key("journal_fdatasync_le_1000us_count")),
    "journal_fdatasync_le_2500us_count": int(sum_key("journal_fdatasync_le_2500us_count")),
    "journal_fdatasync_le_5000us_count": int(sum_key("journal_fdatasync_le_5000us_count")),
    "journal_fdatasync_le_10000us_count": int(sum_key("journal_fdatasync_le_10000us_count")),
    "journal_fdatasync_gt_10000us_count": int(sum_key("journal_fdatasync_gt_10000us_count")),
    "journal_publish_avg_us": weighted_avg("journal_publish_avg_us", "journal_append_count"),
    "journal_publish_max_us": max_key("journal_publish_max_us"),
    "journal_admission_batch_count": int(sum_key("journal_admission_batch_count")),
    "journal_admission_event_count": int(sum_key("journal_admission_event_count")),
    "journal_admission_payload_bytes": int(sum_key("journal_admission_payload_bytes")),
    "journal_admission_avg_events": weighted_avg(
        "journal_admission_avg_events", "journal_admission_batch_count"
    ),
    "journal_admission_max_events": max_key("journal_admission_max_events"),
    "journal_admission_avg_shard_groups": weighted_avg(
        "journal_admission_avg_shard_groups", "journal_admission_batch_count"
    ),
    "journal_admission_max_shard_groups": max_key("journal_admission_max_shard_groups"),
    "journal_admission_work_build_avg_us": weighted_avg(
        "journal_admission_work_build_avg_us", "journal_admission_batch_count"
    ),
    "journal_admission_work_build_max_us": max_key("journal_admission_work_build_max_us"),
    "journal_admission_enqueue_avg_us": weighted_avg(
        "journal_admission_enqueue_avg_us", "journal_admission_batch_count"
    ),
    "journal_admission_enqueue_max_us": max_key("journal_admission_enqueue_max_us"),
    "journal_owner_count": int(sum_key("journal_owner_count")),
    "journal_owner_queue_wait_avg_us": weighted_avg("journal_owner_queue_wait_avg_us", "journal_owner_count"),
    "journal_owner_queue_wait_max_us": max_key("journal_owner_queue_wait_max_us"),
    "journal_owner_service_avg_us": weighted_avg("journal_owner_service_avg_us", "journal_owner_count"),
    "journal_owner_service_max_us": max_key("journal_owner_service_max_us"),
    "journal_owner_pending_flush_count": int(sum_key("journal_owner_pending_flush_count")),
    "journal_owner_pending_flush_wait_avg_us": weighted_avg(
        "journal_owner_pending_flush_wait_avg_us", "journal_owner_pending_flush_count"
    ),
    "journal_owner_pending_flush_wait_max_us": max_key("journal_owner_pending_flush_wait_max_us"),
    "journal_owner_flush_count": int(sum_key("journal_owner_flush_count")),
    "journal_owner_flush_records": int(sum_key("journal_owner_flush_records")),
    "journal_owner_flush_avg_records": weighted_avg("journal_owner_flush_avg_records", "journal_owner_flush_count"),
    "journal_owner_flush_max_records": max_key("journal_owner_flush_max_records"),
    "journal_owner_flush_wall_avg_us": weighted_avg("journal_owner_flush_wall_avg_us", "journal_owner_flush_count"),
    "journal_owner_flush_wall_max_us": max_key("journal_owner_flush_wall_max_us"),
    "journal_owner_flush_fdatasync_avg_us": weighted_avg(
        "journal_owner_flush_fdatasync_avg_us", "journal_owner_flush_count"
    ),
    "journal_owner_flush_fdatasync_max_us": max_key("journal_owner_flush_fdatasync_max_us"),
    "journal_owner_flush_publish_avg_us": weighted_avg(
        "journal_owner_flush_publish_avg_us", "journal_owner_flush_count"
    ),
    "journal_owner_flush_publish_max_us": max_key("journal_owner_flush_publish_max_us"),
    "journal_owner_flush_early_completion_avg_us": weighted_avg(
        "journal_owner_flush_early_completion_avg_us", "journal_owner_flush_count"
    ),
    "journal_owner_flush_early_completion_max_us": max_key("journal_owner_flush_early_completion_max_us"),
    "journal_owner_flush_event_limit_count": int(sum_key("journal_owner_flush_event_limit_count")),
    "journal_owner_flush_byte_limit_count": int(sum_key("journal_owner_flush_byte_limit_count")),
    "journal_owner_flush_queue_boundary_count": int(sum_key("journal_owner_flush_queue_boundary_count")),
    "journal_owner_flush_stop_sync_count": int(sum_key("journal_owner_flush_stop_sync_count")),
    "journal_owner_flush_unknown_count": int(sum_key("journal_owner_flush_unknown_count")),
    "journal_owner_flush_active_admission_count": int(sum_key("journal_owner_flush_active_admission_count")),
    "journal_owner_flush_active_admission_records": int(sum_key("journal_owner_flush_active_admission_records")),
    "journal_owner_flush_active_admission_max": max_key("journal_owner_flush_active_admission_max"),
    "journal_owner_flush_queue_boundary_active_admission_count": int(
        sum_key("journal_owner_flush_queue_boundary_active_admission_count")
    ),
    "journal_owner_queue_boundary_wait_count": int(sum_key("journal_owner_queue_boundary_wait_count")),
    "journal_owner_queue_boundary_wait_avg_us": weighted_avg(
        "journal_owner_queue_boundary_wait_avg_us", "journal_owner_queue_boundary_wait_count"
    ),
    "journal_owner_queue_boundary_wait_max_us": max_key("journal_owner_queue_boundary_wait_max_us"),
    "journal_owner_queue_boundary_wait_woke_for_work_count": int(
        sum_key("journal_owner_queue_boundary_wait_woke_for_work_count")
    ),
    "journal_owner_queue_boundary_wait_timeout_count": int(
        sum_key("journal_owner_queue_boundary_wait_timeout_count")
    ),
    "journal_owner_queue_boundary_admission_wait_count": int(
        sum_key("journal_owner_queue_boundary_admission_wait_count")
    ),
    "journal_owner_queue_boundary_admission_wait_avg_us": weighted_avg(
        "journal_owner_queue_boundary_admission_wait_avg_us",
        "journal_owner_queue_boundary_admission_wait_count",
    ),
    "journal_owner_queue_boundary_admission_wait_max_us": max_key(
        "journal_owner_queue_boundary_admission_wait_max_us"
    ),
    "journal_owner_queue_boundary_admission_wait_woke_for_work_count": int(
        sum_key("journal_owner_queue_boundary_admission_wait_woke_for_work_count")
    ),
    "journal_owner_queue_boundary_admission_wait_drained_count": int(
        sum_key("journal_owner_queue_boundary_admission_wait_drained_count")
    ),
    "journal_completion_dispatch_count": int(sum_key("journal_completion_dispatch_count")),
    "journal_completion_dispatch_records": int(sum_key("journal_completion_dispatch_records")),
    "journal_completion_dispatch_avg_records": weighted_avg(
        "journal_completion_dispatch_avg_records", "journal_completion_dispatch_count"
    ),
    "journal_completion_dispatch_max_records": max_key("journal_completion_dispatch_max_records"),
    "journal_completion_dispatch_avg_us": weighted_avg(
        "journal_completion_dispatch_avg_us", "journal_completion_dispatch_count"
    ),
    "journal_completion_dispatch_max_us": max_key("journal_completion_dispatch_max_us"),
    "journal_batch_completion_count": int(sum_key("journal_batch_completion_count")),
    "journal_batch_completion_records": int(sum_key("journal_batch_completion_records")),
    "journal_batch_completion_avg_records": weighted_avg(
        "journal_batch_completion_avg_records", "journal_batch_completion_count"
    ),
    "journal_batch_completion_max_records": max_key("journal_batch_completion_max_records"),
    "journal_batch_completion_final_callback_count": int(sum_key("journal_batch_completion_final_callback_count")),
    "journal_batch_completion_avg_remaining_before": weighted_avg(
        "journal_batch_completion_avg_remaining_before", "journal_batch_completion_count"
    ),
    "journal_batch_completion_max_remaining_before": max_key("journal_batch_completion_max_remaining_before"),
    "journal_batch_completion_eq_1_count": int(sum_key("journal_batch_completion_eq_1_count")),
    "journal_batch_completion_le_2_count": int(sum_key("journal_batch_completion_le_2_count")),
    "journal_batch_completion_le_4_count": int(sum_key("journal_batch_completion_le_4_count")),
    "journal_batch_completion_le_8_count": int(sum_key("journal_batch_completion_le_8_count")),
    "journal_batch_completion_le_16_count": int(sum_key("journal_batch_completion_le_16_count")),
    "journal_batch_completion_le_32_count": int(sum_key("journal_batch_completion_le_32_count")),
    "journal_batch_completion_le_64_count": int(sum_key("journal_batch_completion_le_64_count")),
    "journal_batch_completion_gt_64_count": int(sum_key("journal_batch_completion_gt_64_count")),
    "journal_owner_service_le_100us_count": int(sum_key("journal_owner_service_le_100us_count")),
    "journal_owner_service_le_250us_count": int(sum_key("journal_owner_service_le_250us_count")),
    "journal_owner_service_le_500us_count": int(sum_key("journal_owner_service_le_500us_count")),
    "journal_owner_service_le_1000us_count": int(sum_key("journal_owner_service_le_1000us_count")),
    "journal_owner_service_le_2500us_count": int(sum_key("journal_owner_service_le_2500us_count")),
    "journal_owner_service_le_5000us_count": int(sum_key("journal_owner_service_le_5000us_count")),
    "journal_owner_service_le_10000us_count": int(sum_key("journal_owner_service_le_10000us_count")),
    "journal_owner_service_gt_10000us_count": int(sum_key("journal_owner_service_gt_10000us_count")),
    "journal_owner_publish_lock_wait_avg_us": weighted_avg("journal_owner_publish_lock_wait_avg_us", "journal_owner_count"),
    "journal_owner_publish_lock_wait_max_us": max_key("journal_owner_publish_lock_wait_max_us"),
    "journal_owner_queue_wait_le_100us_count": int(sum_key("journal_owner_queue_wait_le_100us_count")),
    "journal_owner_queue_wait_le_250us_count": int(sum_key("journal_owner_queue_wait_le_250us_count")),
    "journal_owner_queue_wait_le_500us_count": int(sum_key("journal_owner_queue_wait_le_500us_count")),
    "journal_owner_queue_wait_le_1000us_count": int(sum_key("journal_owner_queue_wait_le_1000us_count")),
    "journal_owner_queue_wait_le_2500us_count": int(sum_key("journal_owner_queue_wait_le_2500us_count")),
    "journal_owner_queue_wait_le_5000us_count": int(sum_key("journal_owner_queue_wait_le_5000us_count")),
    "journal_owner_queue_wait_le_10000us_count": int(sum_key("journal_owner_queue_wait_le_10000us_count")),
    "journal_owner_queue_wait_gt_10000us_count": int(sum_key("journal_owner_queue_wait_gt_10000us_count")),
    "journal_owner_batch_count": int(sum_key("journal_owner_batch_count")),
    "journal_owner_batch_avg_records": weighted_avg("journal_owner_batch_avg_records", "journal_owner_batch_count"),
    "journal_owner_batch_max_records": max_key("journal_owner_batch_max_records"),
    "journal_owner_batch_eq_1_count": int(sum_key("journal_owner_batch_eq_1_count")),
    "journal_owner_batch_le_2_count": int(sum_key("journal_owner_batch_le_2_count")),
    "journal_owner_batch_le_4_count": int(sum_key("journal_owner_batch_le_4_count")),
    "journal_owner_batch_le_8_count": int(sum_key("journal_owner_batch_le_8_count")),
    "journal_owner_batch_le_16_count": int(sum_key("journal_owner_batch_le_16_count")),
    "journal_owner_batch_le_32_count": int(sum_key("journal_owner_batch_le_32_count")),
    "journal_owner_batch_le_64_count": int(sum_key("journal_owner_batch_le_64_count")),
    "journal_owner_batch_gt_64_count": int(sum_key("journal_owner_batch_gt_64_count")),
    "journal_owner_group_commit_count": int(sum_key("journal_owner_group_commit_count")),
    "journal_owner_group_commit_avg_records": weighted_avg(
        "journal_owner_group_commit_avg_records", "journal_owner_group_commit_count"
    ),
    "journal_owner_group_commit_max_records": max_key("journal_owner_group_commit_max_records"),
    "journal_owner_group_commit_eq_1_count": int(sum_key("journal_owner_group_commit_eq_1_count")),
    "journal_owner_group_commit_le_2_count": int(sum_key("journal_owner_group_commit_le_2_count")),
    "journal_owner_group_commit_le_4_count": int(sum_key("journal_owner_group_commit_le_4_count")),
    "journal_owner_group_commit_le_8_count": int(sum_key("journal_owner_group_commit_le_8_count")),
    "journal_owner_group_commit_le_16_count": int(sum_key("journal_owner_group_commit_le_16_count")),
    "journal_owner_group_commit_le_32_count": int(sum_key("journal_owner_group_commit_le_32_count")),
    "journal_owner_group_commit_le_64_count": int(sum_key("journal_owner_group_commit_le_64_count")),
    "journal_owner_group_commit_gt_64_count": int(sum_key("journal_owner_group_commit_gt_64_count")),
    "journal_owner_queue_enqueue_count": int(sum_key("journal_owner_queue_enqueue_count")),
    "journal_owner_queue_enqueue_avg_records": weighted_avg(
        "journal_owner_queue_enqueue_avg_records", "journal_owner_queue_enqueue_count"
    ),
    "journal_owner_queue_enqueue_lock_wait_avg_us": weighted_avg(
        "journal_owner_queue_enqueue_lock_wait_avg_us", "journal_owner_queue_enqueue_count"
    ),
    "journal_owner_queue_enqueue_lock_wait_max_us": max_key("journal_owner_queue_enqueue_lock_wait_max_us"),
    "journal_owner_queue_enqueue_lock_hold_avg_us": weighted_avg(
        "journal_owner_queue_enqueue_lock_hold_avg_us", "journal_owner_queue_enqueue_count"
    ),
    "journal_owner_queue_enqueue_lock_hold_max_us": max_key("journal_owner_queue_enqueue_lock_hold_max_us"),
    "journal_owner_queue_drain_count": int(sum_key("journal_owner_queue_drain_count")),
    "journal_owner_queue_drain_avg_records": weighted_avg(
        "journal_owner_queue_drain_avg_records", "journal_owner_queue_drain_count"
    ),
    "journal_owner_queue_drain_lock_wait_avg_us": weighted_avg(
        "journal_owner_queue_drain_lock_wait_avg_us", "journal_owner_queue_drain_count"
    ),
    "journal_owner_queue_drain_lock_wait_max_us": max_key("journal_owner_queue_drain_lock_wait_max_us"),
    "journal_owner_queue_drain_lock_hold_avg_us": weighted_avg(
        "journal_owner_queue_drain_lock_hold_avg_us", "journal_owner_queue_drain_count"
    ),
    "journal_owner_queue_drain_lock_hold_max_us": max_key("journal_owner_queue_drain_lock_hold_max_us"),
    "journal_owner_queue_depth_max": max_key("journal_owner_queue_depth_max"),
    "journal_group_sync_count": int(sum_key("journal_group_sync_count")),
    "journal_group_sync_records": int(sum_key("journal_group_sync_records")),
    "journal_group_sync_avg_us": weighted_avg("journal_group_sync_avg_us", "journal_group_sync_count"),
    "journal_group_sync_max_us": max_key("journal_group_sync_max_us"),
    "journal_group_sync_avg_records": weighted_avg("journal_group_sync_avg_records", "journal_group_sync_count"),
    "journal_group_sync_max_records": max_key("journal_group_sync_max_records"),
    "journal_group_sync_le_100us_count": int(sum_key("journal_group_sync_le_100us_count")),
    "journal_group_sync_le_250us_count": int(sum_key("journal_group_sync_le_250us_count")),
    "journal_group_sync_le_500us_count": int(sum_key("journal_group_sync_le_500us_count")),
    "journal_group_sync_le_1000us_count": int(sum_key("journal_group_sync_le_1000us_count")),
    "journal_group_sync_le_2500us_count": int(sum_key("journal_group_sync_le_2500us_count")),
    "journal_group_sync_le_5000us_count": int(sum_key("journal_group_sync_le_5000us_count")),
    "journal_group_sync_le_10000us_count": int(sum_key("journal_group_sync_le_10000us_count")),
    "journal_group_sync_gt_10000us_count": int(sum_key("journal_group_sync_gt_10000us_count")),
    "journal_group_sync_eq_1_count": int(sum_key("journal_group_sync_eq_1_count")),
    "journal_group_sync_le_2_count": int(sum_key("journal_group_sync_le_2_count")),
    "journal_group_sync_le_4_count": int(sum_key("journal_group_sync_le_4_count")),
    "journal_group_sync_le_8_count": int(sum_key("journal_group_sync_le_8_count")),
    "journal_group_sync_le_16_count": int(sum_key("journal_group_sync_le_16_count")),
    "journal_group_sync_le_32_count": int(sum_key("journal_group_sync_le_32_count")),
    "journal_group_sync_le_64_count": int(sum_key("journal_group_sync_le_64_count")),
    "journal_group_sync_gt_64_count": int(sum_key("journal_group_sync_gt_64_count")),
    "journal_durable_publish_count": int(sum_key("journal_durable_publish_count")),
    "journal_durable_publish_batch_count": int(sum_key("journal_durable_publish_batch_count")),
    "journal_durable_publish_batch_avg_records": weighted_avg(
        "journal_durable_publish_batch_avg_records", "journal_durable_publish_batch_count"
    ),
    "journal_durable_publish_batch_max_records": max_key("journal_durable_publish_batch_max_records"),
    "journal_durable_publish_batch_eq_1_count": int(sum_key("journal_durable_publish_batch_eq_1_count")),
    "journal_durable_publish_batch_le_2_count": int(sum_key("journal_durable_publish_batch_le_2_count")),
    "journal_durable_publish_batch_le_4_count": int(sum_key("journal_durable_publish_batch_le_4_count")),
    "journal_durable_publish_batch_le_8_count": int(sum_key("journal_durable_publish_batch_le_8_count")),
    "journal_durable_publish_batch_le_16_count": int(sum_key("journal_durable_publish_batch_le_16_count")),
    "journal_durable_publish_batch_le_32_count": int(sum_key("journal_durable_publish_batch_le_32_count")),
    "journal_durable_publish_batch_le_64_count": int(sum_key("journal_durable_publish_batch_le_64_count")),
    "journal_durable_publish_batch_gt_64_count": int(sum_key("journal_durable_publish_batch_gt_64_count")),
    "journal_tail_seal_count": int(sum_key("journal_tail_seal_count")),
    "journal_tail_seal_records": int(sum_key("journal_tail_seal_records")),
    "journal_tail_seal_avg_records": weighted_avg("journal_tail_seal_avg_records", "journal_tail_seal_count"),
    "journal_tail_seal_max_records": max_key("journal_tail_seal_max_records"),
    "journal_tail_sealed_page_max": max_key("journal_tail_sealed_page_max"),
    "journal_callback_dispatch_enqueue_count": int(sum_key("journal_callback_dispatch_enqueue_count")),
    "journal_callback_dispatch_enqueue_lock_wait_avg_us": weighted_avg(
        "journal_callback_dispatch_enqueue_lock_wait_avg_us", "journal_callback_dispatch_enqueue_count"
    ),
    "journal_callback_dispatch_enqueue_lock_wait_max_us": max_key(
        "journal_callback_dispatch_enqueue_lock_wait_max_us"
    ),
    "journal_callback_dispatch_queue_depth_max": max_key("journal_callback_dispatch_queue_depth_max"),
    "journal_callback_dispatch_count": int(sum_key("journal_callback_dispatch_count")),
    "journal_callback_dispatch_queue_wait_avg_us": weighted_avg(
        "journal_callback_dispatch_queue_wait_avg_us", "journal_callback_dispatch_count"
    ),
    "journal_callback_dispatch_queue_wait_max_us": max_key("journal_callback_dispatch_queue_wait_max_us"),
    "journal_callback_dispatch_service_avg_us": weighted_avg(
        "journal_callback_dispatch_service_avg_us", "journal_callback_dispatch_count"
    ),
    "journal_callback_dispatch_service_max_us": max_key("journal_callback_dispatch_service_max_us"),
    "journal_callback_dispatch_queue_wait_le_100us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_le_100us_count")
    ),
    "journal_callback_dispatch_queue_wait_le_250us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_le_250us_count")
    ),
    "journal_callback_dispatch_queue_wait_le_500us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_le_500us_count")
    ),
    "journal_callback_dispatch_queue_wait_le_1000us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_le_1000us_count")
    ),
    "journal_callback_dispatch_queue_wait_le_2500us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_le_2500us_count")
    ),
    "journal_callback_dispatch_queue_wait_le_5000us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_le_5000us_count")
    ),
    "journal_callback_dispatch_queue_wait_le_10000us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_le_10000us_count")
    ),
    "journal_callback_dispatch_queue_wait_gt_10000us_count": int(
        sum_key("journal_callback_dispatch_queue_wait_gt_10000us_count")
    ),
    "journal_tail_read_count": int(sum_key("journal_tail_read_count")),
    "journal_tail_matched_events": int(sum_key("journal_tail_matched_events")),
    "journal_tail_snapshot_avg_us": weighted_avg("journal_tail_snapshot_avg_us", "journal_tail_read_count"),
    "journal_tail_snapshot_max_us": max_key("journal_tail_snapshot_max_us"),
    "journal_tail_snapshot_lock_wait_avg_us": weighted_avg("journal_tail_snapshot_lock_wait_avg_us", "journal_tail_read_count"),
    "journal_tail_snapshot_lock_wait_max_us": max_key("journal_tail_snapshot_lock_wait_max_us"),
    "journal_tail_payload_read_avg_us": weighted_avg("journal_tail_payload_read_avg_us", "journal_tail_read_count"),
    "journal_tail_payload_read_max_us": max_key("journal_tail_payload_read_max_us"),
    "journal_tail_sort_avg_us": weighted_avg("journal_tail_sort_avg_us", "journal_tail_read_count"),
    "journal_tail_sort_max_us": max_key("journal_tail_sort_max_us"),
    "journal_tail_payload_alloc_avg_us": weighted_avg("journal_tail_payload_alloc_avg_us", "journal_tail_read_count"),
    "journal_tail_payload_alloc_max_us": max_key("journal_tail_payload_alloc_max_us"),
    "journal_tail_payload_syscall_avg_us": weighted_avg("journal_tail_payload_syscall_avg_us", "journal_tail_read_count"),
    "journal_tail_payload_syscall_max_us": max_key("journal_tail_payload_syscall_max_us"),
    "journal_tail_payload_memcpy_avg_us": weighted_avg("journal_tail_payload_memcpy_avg_us", "journal_tail_read_count"),
    "journal_tail_payload_memcpy_max_us": max_key("journal_tail_payload_memcpy_max_us"),
    "journal_tail_event_build_avg_us": weighted_avg("journal_tail_event_build_avg_us", "journal_tail_read_count"),
    "journal_tail_event_build_max_us": max_key("journal_tail_event_build_max_us"),
    "journal_tail_payload_read_le_100us_count": int(sum_key("journal_tail_payload_read_le_100us_count")),
    "journal_tail_payload_read_le_250us_count": int(sum_key("journal_tail_payload_read_le_250us_count")),
    "journal_tail_payload_read_le_500us_count": int(sum_key("journal_tail_payload_read_le_500us_count")),
    "journal_tail_payload_read_le_1000us_count": int(sum_key("journal_tail_payload_read_le_1000us_count")),
    "journal_tail_payload_read_le_2500us_count": int(sum_key("journal_tail_payload_read_le_2500us_count")),
    "journal_tail_payload_read_le_5000us_count": int(sum_key("journal_tail_payload_read_le_5000us_count")),
    "journal_tail_payload_read_le_10000us_count": int(sum_key("journal_tail_payload_read_le_10000us_count")),
    "journal_tail_payload_read_gt_10000us_count": int(sum_key("journal_tail_payload_read_gt_10000us_count")),
    "journal_tail_read_group_count": int(sum_key("journal_tail_read_group_count")),
    "journal_tail_read_group_avg": weighted_avg("journal_tail_read_group_avg", "journal_tail_read_count"),
    "journal_tail_read_group_max": max_key("journal_tail_read_group_max"),
    "journal_tail_physical_read_bytes": int(sum_key("journal_tail_physical_read_bytes")),
    "journal_tail_payload_copy_bytes": int(sum_key("journal_tail_payload_copy_bytes")),
    "journal_tail_overread_bytes": int(sum_key("journal_tail_overread_bytes")),
    "journal_tail_direct_read_count": int(sum_key("journal_tail_direct_read_count")),
    "journal_tail_coalesced_read_count": int(sum_key("journal_tail_coalesced_read_count")),
    "journal_tail_vectored_read_count": int(sum_key("journal_tail_vectored_read_count")),
    "post_ack_ingest_count": int(sum_key("post_ack_ingest_count")),
    "post_ack_ingest_avg_us": weighted_avg("post_ack_ingest_avg_us", "post_ack_ingest_count"),
    "post_ack_ingest_max_us": max_key("post_ack_ingest_max_us"),
    "async_drain_enqueue_count": int(sum_key("async_drain_enqueue_count")),
    "async_drain_complete_count": int(sum_key("async_drain_complete_count")),
    "async_drain_avg_us": weighted_avg("async_drain_avg_us", "async_drain_complete_count"),
    "async_drain_max_us": max_key("async_drain_max_us"),
    "async_drain_queue_depth_max": int(max_key("async_drain_queue_depth_max")),
    "wal_drain_read_count": int(sum_key("wal_drain_read_count")),
    "wal_drain_read_payload_bytes": int(sum_key("wal_drain_read_payload_bytes")),
    "wal_drain_read_avg_us": weighted_avg("wal_drain_read_avg_us", "wal_drain_read_count"),
    "wal_drain_read_max_us": max_key("wal_drain_read_max_us"),
    "wal_drain_read_batch_count": int(sum_key("wal_drain_read_batch_count")),
    "wal_drain_read_batch_avg_records": weighted_avg("wal_drain_read_batch_avg_records", "wal_drain_read_batch_count"),
    "wal_drain_read_batch_max_records": max_key("wal_drain_read_batch_max_records"),
    "wal_drain_read_physical_bytes": int(sum_key("wal_drain_read_physical_bytes")),
    "wal_drain_read_gap_bytes": int(sum_key("wal_drain_read_gap_bytes")),
    "wal_drain_read_syscall_count": int(sum_key("wal_drain_read_syscall_count")),
    "wal_drain_read_syscall_avg_records": weighted_avg("wal_drain_read_syscall_avg_records", "wal_drain_read_syscall_count"),
    "wal_drain_batch_wait_count": int(sum_key("wal_drain_batch_wait_count")),
    "wal_drain_batch_wait_avg_us": weighted_avg("wal_drain_batch_wait_avg_us", "wal_drain_batch_wait_count"),
    "wal_drain_batch_wait_max_us": max_key("wal_drain_batch_wait_max_us"),
    "timeline_merge_batch_count": int(sum_key("timeline_merge_batch_count")),
    "timeline_merge_event_count": int(sum_key("timeline_merge_event_count")),
    "timeline_merge_inserted_count": int(sum_key("timeline_merge_inserted_count")),
    "timeline_merge_batch_avg_events": weighted_avg("timeline_merge_batch_avg_events", "timeline_merge_batch_count"),
    "timeline_merge_batch_max_events": max_key("timeline_merge_batch_max_events"),
    "timeline_merge_lock_wait_avg_us": weighted_avg("timeline_merge_lock_wait_avg_us", "timeline_merge_batch_count"),
    "timeline_merge_lock_wait_max_us": max_key("timeline_merge_lock_wait_max_us"),
    "timeline_merge_lock_hold_avg_us": weighted_avg("timeline_merge_lock_hold_avg_us", "timeline_merge_batch_count"),
    "timeline_merge_lock_hold_max_us": max_key("timeline_merge_lock_hold_max_us"),
    "timeline_merge_insert_avg_us": weighted_avg("timeline_merge_insert_avg_us", "timeline_merge_event_count"),
    "timeline_merge_insert_max_us": max_key("timeline_merge_insert_max_us"),
}

summary["expected_record_event_count"] = expected_record_event_count
summary["keeper_count_expected"] = len(keeper_logs)
summary["record_event_count_matches_total"] = (
    expected_record_event_count == 0 or summary["record_event_count"] == expected_record_event_count
)
summary["append_stats_complete"] = (
    summary["record_event_count_matches_total"]
    and summary["keeper_count_with_append_stats"] == summary["keeper_count_expected"]
)
summary["orphan_warning_count"] = orphan_warning_count
metrics["keeper_append_stats_complete"] = summary["append_stats_complete"]
metrics["keeper_record_event_count"] = summary["record_event_count"]
metrics["keeper_journal_append_count"] = summary["journal_append_count"]
metrics["keeper_journal_durable_publish_count"] = summary["journal_durable_publish_count"]
metrics["keeper_journal_durable_publish_count_matches_total"] = (
    expected_record_event_count == 0 or summary["journal_durable_publish_count"] == expected_record_event_count
)
metrics["keeper_journal_tail_seal_count"] = summary["journal_tail_seal_count"]
metrics["keeper_journal_tail_seal_records"] = summary["journal_tail_seal_records"]
metrics["keeper_journal_tail_sealed_page_max"] = summary["journal_tail_sealed_page_max"]
for key in (
    "journal_tail_read_count",
    "journal_tail_matched_events",
    "journal_tail_snapshot_avg_us",
    "journal_tail_snapshot_max_us",
    "journal_tail_snapshot_lock_wait_avg_us",
    "journal_tail_snapshot_lock_wait_max_us",
    "journal_tail_payload_read_avg_us",
    "journal_tail_payload_read_max_us",
    "journal_tail_sort_avg_us",
    "journal_tail_sort_max_us",
    "journal_tail_payload_alloc_avg_us",
    "journal_tail_payload_alloc_max_us",
    "journal_tail_payload_syscall_avg_us",
    "journal_tail_payload_syscall_max_us",
    "journal_tail_payload_memcpy_avg_us",
    "journal_tail_payload_memcpy_max_us",
    "journal_tail_event_build_avg_us",
    "journal_tail_event_build_max_us",
    "journal_tail_payload_read_le_100us_count",
    "journal_tail_payload_read_le_250us_count",
    "journal_tail_payload_read_le_500us_count",
    "journal_tail_payload_read_le_1000us_count",
    "journal_tail_payload_read_le_2500us_count",
    "journal_tail_payload_read_le_5000us_count",
    "journal_tail_payload_read_le_10000us_count",
    "journal_tail_payload_read_gt_10000us_count",
    "journal_tail_read_group_count",
    "journal_tail_read_group_avg",
    "journal_tail_read_group_max",
    "journal_tail_physical_read_bytes",
    "journal_tail_payload_copy_bytes",
    "journal_tail_overread_bytes",
    "journal_tail_direct_read_count",
    "journal_tail_coalesced_read_count",
    "journal_tail_vectored_read_count",
):
    metrics[f"keeper_{key}"] = summary[key]
metrics["keeper_orphan_warning_count"] = orphan_warning_count
keeper_restart_recovery = metrics.get("workflow") == "keeper_restart_recovery"
if keeper_journal_mode != "disabled" and expected_record_event_count > 0 and not keeper_restart_recovery:
    metrics["success"] = bool(metrics.get("success")) and summary["append_stats_complete"]
if keeper_journal_mode != "disabled" and expected_record_event_count > 0 and keeper_restart_recovery:
    metrics["success"] = (
        bool(metrics.get("success"))
        and bool(metrics.get("keeper_journal_valid"))
        and bool(metrics.get("keeper_journal_record_count_matches_total"))
    )
if keeper_journal_mode == "fdatasync" and metrics.get("durability_boundary") in {
    "keeper_local_journal_fdatasync",
    "keeper_local_journal_group_commit_fdatasync",
}:
    metrics["success"] = bool(metrics.get("success")) and metrics["keeper_journal_durable_publish_count_matches_total"]
if metrics.get("deploy_stop_timed_out") is True:
    metrics["success"] = False
metrics["keeper_append_stats"] = {
    "summary": summary,
    "per_keeper": per_keeper,
}
refresh_grapher_archive_metrics(metrics)
metrics_path.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"keeper_append_stats": summary}, indent=2))
PY

cat > "${RESULT_DIR}/summary.md" <<EOF
# ChronoLog Distributed Benchmark Run

- system: ChronoLog
- workflow: ${WORKFLOW}
- completion_mode: ${COMPLETION_MODE}
- deployment_mode: bare_metal
- transport: ofi+sockets
- node_count: ${NODE_COUNT}
- nodes: ${NODE_COUNT}
- client_count: ${CLIENT_COUNT}
- parallel_clients: ${CLIENT_COUNT}
- record_groups: ${RECORD_GROUPS}
- chronolog_producer_outstanding: ${CHRONOLOG_PRODUCER_OUTSTANDING}
- chronolog_producer_batch_size: ${CHRONOLOG_PRODUCER_BATCH_SIZE}
- chronolog_producer_wait_mode: ${CHRONOLOG_PRODUCER_WAIT_MODE}
- chronolog_bench_owned_batch: ${CHRONOLOG_BENCH_OWNED_BATCH}
- chronolog_bench_owned_batch_min_message_size: ${CHRONOLOG_BENCH_OWNED_BATCH_MIN_MESSAGE_SIZE}
- chronolog_client_batch_keeper_selection: ${CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION}
- chronolog_client_keeper_time_bucket_ns: ${CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS}
- chronolog_grapher_inactive_story_delay_seconds: ${GRAPHER_INACTIVE_STORY_DELAY_SECONDS:-installed-config}
- chronolog_grapher_retire_on_stop: ${GRAPHER_RETIRE_ON_STOP}
- chronolog_grapher_stop_retire_grace_us: ${GRAPHER_STOP_RETIRE_GRACE_US}
- chronolog_grapher_stop_story_archive_drain: ${GRAPHER_STOP_STORY_ARCHIVE_DRAIN}
- chronolog_grapher_stop_story_archive_drain_timeout_ms: ${GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS}
- keeper_journal_placement: ${KEEPER_JOURNAL_PLACEMENT}
- operation_count: ${OPERATION_COUNT}
- operation_count_per_client: ${OPERATION_COUNT}
- message_count_per_client: ${OPERATION_COUNT}
- messages_per_client: ${OPERATION_COUNT}
- total_operation_count: $((OPERATION_COUNT * CLIENT_COUNT))
- total_message_count: $((OPERATION_COUNT * CLIENT_COUNT))
- total_messages: $((OPERATION_COUNT * CLIENT_COUNT))
- message_size_bytes: ${MESSAGE_SIZE_BYTES}
- total_payload_bytes: $((OPERATION_COUNT * CLIENT_COUNT * MESSAGE_SIZE_BYTES))
- parallel_client_count: ${CLIENT_COUNT}
- metrics: chronolog/metrics.json
EOF

python3 - "${CHRONOLOG_RESULT_DIR}/metrics.json" <<'PY'
import json
import sys
from pathlib import Path

metrics_path = Path(sys.argv[1])
if not metrics_path.exists():
    raise SystemExit(1)
metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
raise SystemExit(0 if metrics.get("success") is True else 1)
PY
