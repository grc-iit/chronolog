#!/usr/bin/env bash
# Interleaved before and after runs of one append cell, so disk and load history hit both variants alike:
#   ab.sh BEFORE_KEEPER AFTER_KEEPER "append --payload 1024 ... --batch 1 --writers 1" [ROUNDS]
# Prints one line per run with the one minute load average at its start.
set -uo pipefail
before=$1 after=$2 cell=$3 rounds=${4:-4}
here=$(cd "$(dirname "$0")" && pwd)
for ((i = 1; i <= rounds; ++i)); do
    for variant in before after; do
        keeper=$before
        [ "$variant" = after ] && keeper=$after
        sleep 5
        load=$(cut -d' ' -f1 /proc/loadavg)
        # shellcheck disable=SC2086
        BENCH_KEEPER=$keeper bash "$here/run.sh" custom --out "$here/../../build/bench-ab-$variant" -- $cell --tag "$variant-$i" 2>&1 | LOAD=$load python3 -c '
import json, os, sys
for line in sys.stdin:
    if not line.startswith("{\"layer\""):
        continue
    r = json.loads(line)
    p, x = r["params"], r["result"]
    print("%-9s load %5s ev/s %8.0f p50 %8.0f p99 %9.0f p99.9 %9.0f max %9.0f us calls %6d errors %d" % (
        p.get("tag", ""), os.environ["LOAD"], x["events_per_s"], x["p50_us"], x["p99_us"], x["p999_us"], x["max_us"], x["calls"], x["errors"]))
'
    done
done
