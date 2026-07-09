#!/bin/bash
# READ matrix: mirrors run_final.sh but for reads. Two patterns per backend:
#   tail    = most recent ~tail_k events (dashboard-style recent slice)
#   archive = full historical scan of the dataset
# Concurrency = independent reader processes (each reads its own partition/story).
# Aggregate throughput = Σ events_read / (max t_end - min t_start).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH=$HOME/chronolog-install/chronolog/lib:${LD_LIBRARY_PATH:-}
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
CONF=$HOME/chronolog-install/chronolog/conf/chrono-client-conf.json
KT=$HOME/kafka_2.13-4.1.2/bin/kafka-topics.sh
BROKERS=localhost:9092
CHRONICLE=rd$(date +%s)
SETTLE=35; TAIL_K=1000; PER=20000   # events per partition / per story
TIMEOUT=120

OUT=$HERE/read.csv
echo "backend,payload,concurrency,readtype,events_read,agg_eps,agg_mbps" > "$OUT"
SIZES="256 1024 4096 16384"
CONC="1 4 8"

agg() { # files... -> "events min_t0 max_t1"  (col layout: type ev plen t0 t1)
  cat "$@" 2>/dev/null | awk -v rt="$1x" 'BEGIN{} {} {ev+=$2; if(min==""||$4<min)min=$4; if($5>max)max=$5}
    END{printf "%d %.6f %.6f", ev+0, min+0, max+0}'
}
emit(){ # backend size c readtype  (reads $d/*.<readtype>)
  local b=$1 sz=$2 c=$3 rt=$4
  read ev t0 t1 < <(cat "$d"/*.$rt 2>/dev/null | awk '{ev+=$2; if(min==""||$4<min)min=$4; if($5>max)max=$5} END{printf "%d %.6f %.6f",ev+0,min+0,max+0}')
  local eps mbps
  eps=$(awk -v e=$ev -v a=$t0 -v b=$t1 'BEGIN{printf "%.0f",(b>a)?e/(b-a):0}')
  mbps=$(awk -v eps=$eps -v s=$sz 'BEGIN{printf "%.2f",eps*s/1048576}')
  echo "$b,$sz,$c,$rt,$ev,$eps,$mbps" | tee -a "$OUT"
}

echo "[*] pre-loading Kafka per-size topics (8 partitions x $PER events)..."
for sz in $SIZES; do
  T=read-$sz
  $KT --bootstrap-server $BROKERS --delete --topic $T >/dev/null 2>&1
  $KT --bootstrap-server $BROKERS --create --topic $T --partitions 8 --replication-factor 1 >/dev/null 2>&1
  "$HERE/kafka_bench" $BROKERS $T $sz $((PER*8)) 8 1 0 >/dev/null 2>&1
done

for sz in $SIZES; do
  for c in $CONC; do
    # ---------- Kafka ----------
    for rt in archive tail; do
      d=$(mktemp -d); pids=""
      for i in $(seq 0 $((c-1))); do
        "$HERE/kafka_read_bench" $BROKERS read-$sz $i $rt $TAIL_K > "$d/p$i.$rt" 2>/dev/null &
        pids="$pids $!"
      done
      wait $pids; emit kafka $sz $c $rt; rm -rf "$d"
    done
    # ---------- ChronoLog (each proc: write own story, hold, read) ----------
    d=$(mktemp -d); pids=""
    for i in $(seq 0 $((c-1))); do
      ( timeout $TIMEOUT "$HERE/chrono_read_bench" $CONF $CHRONICLE $sz $PER $SETTLE $TAIL_K 2>/dev/null \
          | awk -v f="$d/p$i" '{print > (f"."$1)}' ) &
      pids="$pids $!"
    done
    wait $pids
    emit chronolog $sz $c archive
    emit chronolog $sz $c tail
    rm -rf "$d"
  done
done
echo "[*] done -> $OUT"
