#!/bin/bash
# Final, robust measurement.
#  - ChronoLog: independent writer processes, each timeout-guarded (the client
#    can deadlock under concurrency); per-event synchronous keeper-acked rate.
#  - Kafka: high event counts so the batched/async pipeline + flush is timed
#    credibly (acks=1, broker-acknowledged).
# Aggregate throughput per cell = sum(events_completed) / (max t_end - min t_start).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH=$HOME/chronolog-install/chronolog/lib:${LD_LIBRARY_PATH:-}
CONF=$HOME/chronolog-install/chronolog/conf/chrono-client-conf.json
BROKERS=localhost:9092; TOPIC=ldms-bench; ACKS=1
CHRONICLE=fin$(date +%s)
TIMEOUT=40

OUT=$HERE/final.csv
echo "backend,payload,concurrency,events_done,procs_done,procs_req,agg_acked_eps,agg_acked_mbps,errors" > "$OUT"

SIZES="256 1024 4096 16384"
CONC="1 4 8"
declare -A CN=( [256]=60000 [1024]=60000 [4096]=30000 [16384]=15000 )   # ChronoLog N
declare -A KN=( [256]=4000000 [1024]=2000000 [4096]=800000 [16384]=300000 ) # Kafka N

agg() { # files... -> "events min_t0 max_t1 errs procs_done"
  cat "$@" 2>/dev/null | awk -F, 'NF>=13{ ev+=$4; err+=$11; pc++;
      if(min==""||$12<min)min=$12; if($13>max)max=$13 }
      END{ printf "%d %.6f %.6f %d %d", ev+0, min+0, max+0, err+0, pc+0 }'
}
emit() { # backend size c  (reads d/*.csv)
  local backend=$1 sz=$2 c=$3
  read ev t0 t1 err pc < <(agg "$d"/p*.csv)
  local eps mbps
  eps=$(awk -v e=$ev -v a=$t0 -v b=$t1 'BEGIN{printf "%.0f",(b>a)?e/(b-a):0}')
  mbps=$(awk -v eps=$eps -v s=$sz 'BEGIN{printf "%.2f",eps*s/1048576}')
  echo "$backend,$sz,$c,$ev,$pc,$c,$eps,$mbps,$err" | tee -a "$OUT"
}

for sz in $SIZES; do
  for c in $CONC; do
    # ---- Kafka ----
    n=${KN[$sz]}; per=$(( n / c )); d=$(mktemp -d); pids=""
    for i in $(seq 0 $((c-1))); do
      "$HERE/kafka_bench" $BROKERS $TOPIC $sz $per 1 $ACKS $i > "$d/p$i.csv" 2>/dev/null &
      pids="$pids $!"
    done
    wait $pids; emit kafka $sz $c; rm -rf "$d"
    # ---- ChronoLog (timeout-guarded) ----
    n=${CN[$sz]}; per=$(( n / c )); d=$(mktemp -d); pids=""
    for i in $(seq 0 $((c-1))); do
      timeout $TIMEOUT "$HERE/chrono_bench" $CONF $CHRONICLE $sz $per 1 > "$d/p$i.csv" 2>/dev/null &
      pids="$pids $!"
    done
    wait $pids; emit chronolog $sz $c; rm -rf "$d"
  done
done
echo "[*] done -> $OUT"
