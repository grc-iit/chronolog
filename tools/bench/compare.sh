#!/usr/bin/env bash
# Repeats one append cell several times on fresh stacks so a fix can be compared before and after on the same
# parameters:  compare.sh LABEL "append --payload 1024 --durability durable --seconds 10 --warmup 1 --batch 64 --writers 16" [RUNS]
# Prints one summary line per run; the JSON lines stay under build/bench-compare-LABEL.
set -uo pipefail
label=$1 cell=$2 runs=${3:-3}
here=$(cd "$(dirname "$0")" && pwd)
out=$here/../../build/bench-compare-$label
rm -rf "$out"
for ((i = 1; i <= runs; ++i)); do
    # shellcheck disable=SC2086
    bash "$here/run.sh" custom --out "$out" -- $cell --tag "$label-$i" 2>&1 | python3 -c '
import json, sys
for line in sys.stdin:
    if not line.startswith("{\"layer\""):
        continue
    r = json.loads(line)
    p, x = r["params"], r["result"]
    print("%-18s b%-3s w%-3s ev/s %8.0f p50 %8.0f p99 %9.0f p99.9 %9.0f max %9.0f us calls %6d errors %d" % (
        p.get("tag", ""), p["batch"], p["writers"], x["events_per_s"], x["p50_us"], x["p99_us"], x["p999_us"], x["max_us"], x["calls"], x["errors"]))
'
done
