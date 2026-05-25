#!/usr/bin/env bash
#
# setup_perf_baselines_branch.sh — One-time bootstrap of the perf-baselines
#                                   orphan branch.
#
# Creates a perf-baselines branch with no shared history with main, seeds it
# with a README and schema, and optionally pushes it to origin. After this
# runs once, baselines are appended manually via save_baseline.sh.
#
# Usage (from the repo root):
#   tools/.../setup_perf_baselines_branch.sh [--remote origin] [--push]
#
# Without --push the branch is created locally only; pass --push to publish.

set -euo pipefail

REMOTE="origin"
DO_PUSH=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --remote) REMOTE="$2"; shift 2 ;;
        --push)   DO_PUSH=1;   shift   ;;
        -h|--help)
            sed -n '2,/^$/s/^# *//p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# If the local branch already exists and --push was requested, just push it.
if git rev-parse --verify --quiet refs/heads/perf-baselines >/dev/null; then
    if (( DO_PUSH )); then
        if git ls-remote --exit-code --heads "$REMOTE" perf-baselines >/dev/null 2>&1; then
            echo "ERROR: remote $REMOTE already has a perf-baselines branch." >&2
            exit 1
        fi
        git push "$REMOTE" perf-baselines
        echo "[setup] pushed perf-baselines to $REMOTE."
        exit 0
    else
        echo "ERROR: local branch perf-baselines already exists." >&2
        exit 1
    fi
fi

if git ls-remote --exit-code --heads "$REMOTE" perf-baselines >/dev/null 2>&1; then
    echo "ERROR: remote $REMOTE already has a perf-baselines branch." >&2
    exit 1
fi

# Build the seed content in a temp dir, then commit on an orphan branch in a
# disposable worktree so we don't touch the user's current checkout.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git worktree add --detach "$WORK"
pushd "$WORK" >/dev/null

# Wipe the worktree (the orphan branch starts from scratch).
git rm -rf --quiet . >/dev/null 2>&1 || true
find . -mindepth 1 -maxdepth 1 -not -name '.git' -exec rm -rf {} +

git checkout --orphan perf-baselines

cat >README.md <<'MD'
# ChronoLog Performance Baselines

This branch holds per-release performance baselines for the manual regression
workflow defined in `tests/performance/perf_regression_design.md` (on `main`).

Layout:

    <version>/ares/ofi+sockets/4groups_4x20clients/record_event.json

Each baseline JSON conforms to `schema.json` at the branch root.

Baselines are committed manually after each release run on Ares using
`tests/performance/save_baseline.sh`. If you must amend an existing baseline,
prefer adding a corrected file alongside the original rather than rewriting
it so the history reflects what each release actually measured.
MD

cat >schema.json <<'JSON'
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "ChronoLog perf-baseline (record_event bandwidth + throughput)",
  "type": "object",
  "required": ["schema_version", "version", "git_sha", "test_date",
               "platform", "config", "metrics"],
  "properties": {
    "schema_version": {"type": "integer", "minimum": 2},
    "version":        {"type": "string", "pattern": "^[0-9]+\\.[0-9]+\\.[0-9]+$"},
    "git_sha":        {"type": "string"},
    "test_date":      {"type": "string", "format": "date-time"},
    "platform":       {"type": "string"},
    "config": {
      "type": "object",
      "required": ["recording_groups", "client_nodes", "clients_per_node", "protocol"]
    },
    "reps":        {"type": "integer", "minimum": 1},
    "aggregation": {"type": "string", "enum": ["mean", "median"]},
    "metrics": {
      "type": "object",
      "required": ["record_event_bw_mbps", "record_event_ops"],
      "properties": {
        "record_event_bw_mbps": {
          "type": "object",
          "additionalProperties": {"type": "number"},
          "description": "Per-event-size mean (reps<=5) or median (reps>5) record_event bandwidth in MB/s"
        },
        "record_event_ops": {
          "type": "object",
          "additionalProperties": {"type": "number"},
          "description": "Per-event-size mean (reps<=5) or median (reps>5) record_event throughput in ops/s"
        }
      }
    }
  }
}
JSON

git add README.md schema.json
git commit -m "perf-baselines: seed branch with README + schema"

popd >/dev/null
git worktree remove --force "$WORK"

if (( DO_PUSH )); then
    git push "$REMOTE" perf-baselines
    echo "[setup] pushed perf-baselines to $REMOTE."
else
    echo "[setup] perf-baselines branch created locally; rerun with --push to publish."
fi
