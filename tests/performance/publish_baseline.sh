#!/usr/bin/env bash
#
# publish_baseline.sh — Place CURRENT.json into the perf-baselines branch and
#                       push.
#
# Assumes the perf-baselines branch is already checked out at --baselines-root
# (e.g. via `git worktree add .perf-baselines perf-baselines`).
#
# Usage:
#   publish_baseline.sh CURRENT.json --baselines-root DIR
#                       [--version X.Y.Z] [--remote origin]
#                       [--dry-run]
#
# Version is parsed from CURRENT.json's "version" field unless --version is
# given. The destination path inside the branch is
#   <version>/ares/ofi+sockets/4groups_4x20clients/record_event.json

set -euo pipefail

CURRENT=""
BASELINES_ROOT=""
VERSION=""
REMOTE="origin"
DRY_RUN=0

usage() {
    sed -n '2,/^$/s/^# *//p' "$0"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baselines-root) BASELINES_ROOT="$2"; shift 2 ;;
        --version)        VERSION="$2";        shift 2 ;;
        --remote)         REMOTE="$2";         shift 2 ;;
        --dry-run)        DRY_RUN=1;           shift   ;;
        -h|--help)        usage ;;
        *)
            if [[ -z "$CURRENT" ]]; then
                CURRENT="$1"; shift
            else
                echo "Unknown arg: $1" >&2; usage
            fi ;;
    esac
done

[[ -n "$CURRENT" ]]          || { echo "ERROR: CURRENT.json required" >&2;       usage; }
[[ -n "$BASELINES_ROOT" ]]   || { echo "ERROR: --baselines-root required" >&2;   usage; }
[[ -f "$CURRENT" ]]          || { echo "ERROR: $CURRENT does not exist" >&2;     exit 1; }
[[ -d "$BASELINES_ROOT" ]]   || { echo "ERROR: $BASELINES_ROOT does not exist" >&2; exit 1; }

if [[ -z "$VERSION" ]]; then
    VERSION="$(python3 -c "import json,sys; print(json.load(open('$CURRENT'))['version'])")"
fi

DEST_REL="${VERSION}/ares/ofi+sockets/4groups_4x20clients/record_event.json"
DEST_ABS="${BASELINES_ROOT%/}/${DEST_REL}"

mkdir -p "$(dirname "$DEST_ABS")"
cp "$CURRENT" "$DEST_ABS"

cd "$BASELINES_ROOT"

git add "$DEST_REL"
if git diff --cached --quiet; then
    echo "[publish_baseline] $DEST_REL is unchanged; nothing to commit."
    exit 0
fi

MSG="perf: baseline for ${VERSION}"
if (( DRY_RUN )); then
    echo "[publish_baseline] DRY-RUN — would commit and push:"
    echo "  $MSG"
    git diff --cached --stat
    exit 0
fi

git commit -m "$MSG"
git push "$REMOTE" HEAD:perf-baselines

echo "[publish_baseline] published $DEST_REL"
