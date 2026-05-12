#!/usr/bin/env bash
set -euo pipefail

SESSION="${PROFILEFORGE_TMUX_SESSION:-profileforge-loop}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

if tmux has-session -t "${SESSION}" 2>/dev/null; then
  echo "tmux session already exists: ${SESSION}" >&2
  echo "attach with: tmux attach -t ${SESSION}" >&2
  exit 1
fi

CMD=(
  python3 profileforge/controller/run_loop.py
  --iterations "${PROFILEFORGE_ITERATIONS:-1}"
  --max-iterations "${PROFILEFORGE_MAX_ITERATIONS:-100}"
  --goal-system "${PROFILEFORGE_GOAL_SYSTEM:-mofka}"
  --goal-ratio "${PROFILEFORGE_GOAL_RATIO:-2.0}"
  --systems "${PROFILEFORGE_SYSTEMS:-chronolog}"
  --workflows "${PROFILEFORGE_WORKFLOWS:-append_throughput}"
  --node-counts "${PROFILEFORGE_NODE_COUNTS:-2}"
  --message-sizes "${PROFILEFORGE_MESSAGE_SIZES:-1024}"
  --operation-counts "${PROFILEFORGE_OPERATION_COUNTS:-100}"
  --trials "${PROFILEFORGE_TRIALS:-3}"
  --partition "${PROFILEFORGE_PARTITION:-debug}"
  --slurm-time "${PROFILEFORGE_SLURM_TIME:-00:10:00}"
  --chronolog-profile-mode "${PROFILEFORGE_PROFILE_MODE:-tau}"
)

if [[ "${PROFILEFORGE_RUN_UNTIL_GOAL:-1}" == "1" ]]; then
  CMD+=(--run-until-goal)
fi
if [[ -n "${PROFILEFORGE_NODELIST:-}" ]]; then
  CMD+=(--nodelist "${PROFILEFORGE_NODELIST}")
fi
if [[ -n "${PROFILEFORGE_PATCH_COMMAND:-}" ]]; then
  CMD+=(--patch-command "${PROFILEFORGE_PATCH_COMMAND}")
fi

printf -v RENDERED '%q ' "${CMD[@]}"
tmux new-session -d -s "${SESSION}" "cd '${REPO_ROOT}' && ${RENDERED}; exec bash"
echo "started tmux session: ${SESSION}"
echo "attach with: tmux attach -t ${SESSION}"
