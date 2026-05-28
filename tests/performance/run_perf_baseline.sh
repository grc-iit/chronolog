#!/usr/bin/env bash
#
# run_perf_baseline.sh — Drive ares_test.sh with the fixed regression case
#                        and emit a baseline JSON for the current build.
#
# The regression case is frozen per test/performance/perf_regression_design.md:
#
#   COMPONENT_SCALES=4              (4 recording groups, 1 keeper/grapher/player each)
#   WRITE_CLIENT_CONFIGS=20x4       (20 clients × 4 nodes = 80 clients)
#   PROTOCOLS=ofi+sockets
#   EVENT_SIZES=1024 .. 1048576     (powers of two up to 1 MB)
#   UNIFORM_EVENT_COUNT=1           (same count for every event size)
#   UNIFORM_EVENT_COUNT_N=500       (500 events per client per run)
#   WRITE_TESTS=recording           (write metric tracked in the baseline)
#   READ_TESTS=replay               (read metric; activate by flipping RUN_READ=1 below)
#   RUN_WRITE=1 RUN_READ=0          (replay requires pre-written data so RUN_WRITE must stay 1
#                                    when RUN_READ=1; also set READ_CLIENT_CONFIGS as needed)
#   REPS=3
#
# Outputs:
#   --out PATH        — JSON baseline file (schema in perf_regression_design.md §3)
#   --logs-dir PATH   — passes through to ares_test.sh LOG_DIR; default is
#                       under the script's parent directory with the usual stamp.
#
# Other flags:
#   --skip-run        — don't re-run ares_test; just regenerate the JSON from
#                       the latest ares_test_logs_* / ares_test_logs_latest.
#   --dry-run         — propagate DRY_RUN=1 to ares_test.sh (smoke test only;
#                       no real cluster work).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
ARES_TEST="${REPO_ROOT}/tests/performance/ares_test.sh"
EXTRACT_PY="${REPO_ROOT}/tests/performance/extract_plot_results.py"
SUMMARIZE_PY="${SCRIPT_DIR}/summarize_to_baseline_json.py"

OUT_JSON=""
LOGS_DIR=""
SKIP_RUN=0
DRY_RUN_FLAG=0

usage() {
    sed -n '2,/^$/s/^# *//p' "$0"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)       OUT_JSON="$2";   shift 2 ;;
        --logs-dir)  LOGS_DIR="$2";   shift 2 ;;
        --skip-run)  SKIP_RUN=1;      shift   ;;
        --dry-run)   DRY_RUN_FLAG=1;  shift   ;;
        -h|--help)   usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
done

if [[ -z "$OUT_JSON" ]]; then
    echo "ERROR: --out PATH is required" >&2
    exit 1
fi

OUT_JSON="$(realpath -m "$OUT_JSON")"
mkdir -p "$(dirname "$OUT_JSON")"

# ─── Run the narrow case ──────────────────────────────────────────────────────
if (( ! SKIP_RUN )); then
    [[ -x "$ARES_TEST" ]] || { echo "ERROR: $ARES_TEST not executable" >&2; exit 1; }

    export MPIRUN="${REPO_ROOT}/.spack-env/view/bin/mpirun"
    export COMPONENT_SCALES=4
    export PROTOCOLS=ofi+sockets
    export WRITE_CLIENT_CONFIGS=20x4
    export READ_CLIENT_CONFIGS=""   # empty: read configs don't inflate node allocation
    export WRITE_TESTS=recording
    export READ_TESTS=replay
    export EVENT_SIZES="1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576"
    export UNIFORM_EVENT_COUNT=1
    export UNIFORM_EVENT_COUNT_N=500
    export RUN_WRITE=1
    export RUN_READ=0   # set to 1 (and export READ_CLIENT_CONFIGS) to include replay
    export REPS=3

    if (( DRY_RUN_FLAG )); then
        export DRY_RUN=1
    else
        export DRY_RUN=0
    fi

    # Default logs to SCRIPT_DIR so the ares_test_logs_latest symlink lands
    # next to this script, where the resolution block below will find it.
    if [[ -z "$LOGS_DIR" ]]; then
        LOGS_DIR="${SCRIPT_DIR}/ares_test_logs_$(date +%Y%m%d_%H%M%S)"
    fi
    export LOG_DIR="$LOGS_DIR"

    # Build flag list from exported RUN_* knobs so toggling RUN_READ=1 above
    # is the only change needed to add replay to the baseline.
    _ares_flags=()
    (( RUN_WRITE )) && _ares_flags+=(--write) || _ares_flags+=(--no-write)
    (( RUN_READ  )) && _ares_flags+=(--read)  || _ares_flags+=(--no-read)

    echo "[run_perf_baseline] invoking ares_test.sh"
    bash "$ARES_TEST" "${_ares_flags[@]}"
fi

# ─── Resolve which logs to summarize ──────────────────────────────────────────
if [[ -z "$LOGS_DIR" ]]; then
    # ares_test.sh writes ares_test_logs_latest symlink next to itself
    LATEST="${REPO_ROOT}/tests/performance/ares_test_logs_latest"
    [[ -e "$LATEST" ]] || { echo "ERROR: no ares_test_logs_latest symlink at $LATEST" >&2; exit 1; }
    LOGS_DIR="$(readlink -f "$LATEST")"
fi

[[ -d "$LOGS_DIR" ]] || { echo "ERROR: logs dir not found: $LOGS_DIR" >&2; exit 1; }

# ─── Run extract_plot_results.py to produce per-test CSVs ────────────────────
[[ -f "$EXTRACT_PY" ]] || { echo "ERROR: $EXTRACT_PY not found" >&2; exit 1; }

RESULTS_FILE="$(find "$LOGS_DIR" -maxdepth 1 -name 'ares_test_*.results' | head -1)"
[[ -n "$RESULTS_FILE" ]] || { echo "ERROR: no .results file in $LOGS_DIR" >&2; exit 1; }

echo "[run_perf_baseline] extracting per-test CSVs from $(basename "$RESULTS_FILE")"
python3 "$EXTRACT_PY" "$RESULTS_FILE" >/dev/null

# ─── Summarize the recording test into a baseline JSON ────────────────────────
[[ -f "$SUMMARIZE_PY" ]] || { echo "ERROR: $SUMMARIZE_PY not found" >&2; exit 1; }

echo "[run_perf_baseline] writing baseline JSON to $OUT_JSON"
python3 "$SUMMARIZE_PY" \
    --logs-dir "$LOGS_DIR" \
    --out "$OUT_JSON"

echo "[run_perf_baseline] done."
