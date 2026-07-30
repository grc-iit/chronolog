#!/bin/bash
# 1 Hz sampler for a chrono-keeper process. Runs ON a keeper node (started over
# ssh by run_bench.sh) and appends CSV rows until <stop_file> appears.
#
# Usage: keeper_sampler.sh <out_csv> <stop_file>
#
# Columns: epoch_s,utime_ticks,stime_ticks,rss_kb
# utime/stime are cumulative clock ticks from /proc/<pid>/stat fields 14/15;
# analyze.py differentiates them against epoch_s to get %CPU of one core.

set -u

OUT_CSV=$1
STOP_FILE=$2
TICKS_PER_SEC=$(getconf CLK_TCK)

echo "# ticks_per_sec=${TICKS_PER_SEC}" >"${OUT_CSV}"
echo "epoch_s,utime_ticks,stime_ticks,rss_kb" >>"${OUT_CSV}"

while [[ ! -f "${STOP_FILE}" ]]; do
    pid=$(pgrep -f 'chrono-keeper --config' | head -1)
    if [[ -n "${pid}" && -r "/proc/${pid}/stat" ]]; then
        # Field 2 (comm) may contain spaces/parens, so split on the last ')'.
        stat_tail=$(sed 's/.*) //' "/proc/${pid}/stat" 2>/dev/null)
        if [[ -n "${stat_tail}" ]]; then
            # After stripping "pid (comm) ", field 1 is state, so utime/stime
            # (14/15 in the original numbering) are fields 12/13 here.
            utime=$(echo "${stat_tail}" | awk '{print $12}')
            stime=$(echo "${stat_tail}" | awk '{print $13}')
            rss=$(awk '/^VmRSS:/{print $2}' "/proc/${pid}/status" 2>/dev/null)
            echo "$(date +%s.%N),${utime:-0},${stime:-0},${rss:-0}" >>"${OUT_CSV}"
        fi
    fi
    sleep 1
done
