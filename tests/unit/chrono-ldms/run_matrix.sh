#!/bin/bash
# Run the Kafka vs ChronoLog workload matrix and emit results.csv.
# Workload dimensions: payload size x concurrency. Same event counts feed both
# backends. Metric of record: server-acknowledged throughput (Kafka flush-acked
# == acks=1; ChronoLog synchronous keeper-acked).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH=$HOME/chronolog-install/chronolog/lib:${LD_LIBRARY_PATH:-}
CONF=$HOME/chronolog-install/chronolog/conf/chrono-client-conf.json
BROKERS=localhost:9092
TOPIC=ldms-bench
ACKS=1
CHRONICLE=bench$(date +%s)

OUT=$HERE/results.csv
LOG=$HERE/results.log
echo "backend,acks,payload,events,threads,enqueue_eps,enqueue_mbps,acked_eps,acked_mbps,delivered,errors" > "$OUT"
: > "$LOG"

# payload_size -> event count (keeps per-cell volume/time bounded)
declare -A EVENTS=( [256]=150000 [1024]=80000 [4096]=40000 [16384]=15000 )
SIZES="256 1024 4096 16384"
THREADS="1 4 8"

for sz in $SIZES; do
  n=${EVENTS[$sz]}
  for thr in $THREADS; do
    echo ">>> kafka   size=$sz events=$n threads=$thr" | tee -a "$LOG"
    "$HERE/kafka_bench" $BROKERS $TOPIC $sz $n $thr $ACKS 2>>"$LOG" >> "$OUT"
    echo ">>> chrono  size=$sz events=$n threads=$thr" | tee -a "$LOG"
    "$HERE/chrono_bench" $CONF $CHRONICLE $sz $n $thr 2>>"$LOG" >> "$OUT"
  done
done

echo "[*] done -> $OUT"
