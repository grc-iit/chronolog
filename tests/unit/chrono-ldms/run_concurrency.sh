#!/bin/bash
# Concurrency comparison via INDEPENDENT WRITER PROCESSES (ChronoLog's intended
# many-writers model; also realistic for Kafka). For each payload size and
# concurrency C, launch C single-threaded writer processes (each handles n/C
# events to its own ChronoLog story / Kafka partition). Aggregate throughput is
# total_events / (max(t_end) - min(t_start)) over the overlapping timed regions.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH=$HOME/chronolog-install/chronolog/lib:${LD_LIBRARY_PATH:-}
CONF=$HOME/chronolog-install/chronolog/conf/chrono-client-conf.json
BROKERS=localhost:9092; TOPIC=ldms-bench; ACKS=1
CHRONICLE=conc$(date +%s)

OUT=$HERE/concurrency.csv
echo "backend,payload,concurrency,total_events,agg_acked_eps,agg_acked_mbps,errors" > "$OUT"

declare -A EVENTS=( [256]=200000 [1024]=120000 [4096]=60000 [16384]=24000 )
SIZES="256 1024 4096 16384"
CONC="1 4 8"

aggregate() { # <glob of per-proc csv files> -> echoes "sum_events min_t0 max_t1 sum_err"
  cat "$@" | awk -F, '
    { ev+=$4; err+=$11; if(min==""||$12<min)min=$12; if($13>max)max=$13 }
    END { printf "%d %.6f %.6f %d", ev, min, max, err }'
}

for sz in $SIZES; do
  n=${EVENTS[$sz]}
  for c in $CONC; do
    per=$(( n / c ))
    # --- Kafka ---
    d=$(mktemp -d); pids=""
    for i in $(seq 0 $((c-1))); do
      "$HERE/kafka_bench" $BROKERS $TOPIC $sz $per 1 $ACKS $i > "$d/p$i.csv" 2>/dev/null &
      pids="$pids $!"
    done
    wait $pids
    read ev t0 t1 err < <(aggregate "$d"/p*.csv)
    eps=$(awk -v e=$ev -v a=$t0 -v b=$t1 'BEGIN{printf "%.0f", (b>a)?e/(b-a):0}')
    mbps=$(awk -v eps=$eps -v s=$sz 'BEGIN{printf "%.2f", eps*s/1048576}')
    echo "kafka,$sz,$c,$ev,$eps,$mbps,$err" | tee -a "$OUT"
    rm -rf "$d"
    # --- ChronoLog ---
    d=$(mktemp -d); pids=""
    for i in $(seq 0 $((c-1))); do
      "$HERE/chrono_bench" $CONF $CHRONICLE $sz $per 1 > "$d/p$i.csv" 2>/dev/null &
      pids="$pids $!"
    done
    wait $pids
    read ev t0 t1 err < <(aggregate "$d"/p*.csv)
    eps=$(awk -v e=$ev -v a=$t0 -v b=$t1 'BEGIN{printf "%.0f", (b>a)?e/(b-a):0}')
    mbps=$(awk -v eps=$eps -v s=$sz 'BEGIN{printf "%.2f", eps*s/1048576}')
    echo "chronolog,$sz,$c,$ev,$eps,$mbps,$err" | tee -a "$OUT"
    rm -rf "$d"
  done
done
echo "[*] done -> $OUT"
