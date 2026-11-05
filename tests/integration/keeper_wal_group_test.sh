#!/bin/bash
# Integration test: a keeper WAL group grows larger than the keeper's RPC pool.
#
# The keeper answers record_event_durable on its RPC pool, IngestionThreadCount
# xstreams (4 in the template). A durable write waits until its group is
# synced. If the handler waited on a std::condition_variable it would hold one
# of those OS threads for the whole wait, so at most IngestionThreadCount
# writes could be waiting at once and no group could hold more events than
# that. The handlers wait with Argobots primitives instead, which yield the
# xstream, so many writes wait on one group and one fdatasync covers them all.
#
# 64 MPI ranks of chrono-bench --durable write against one keeper with the WAL
# on and IngestionThreadCount 4. The keeper logs a "[KeeperWal] stats" line
# every 10 s, whose max_events_per_group is the most E records in one synced
# group so far.
#
#   bench : every durable write is acknowledged (CL_SUCCESS for each one sent)
#   group : some stats line shows max_events_per_group above 4
#
# Requires an installed tree (deploy_local.sh layout). Patches only the
# *installed* conf template, and restores it on exit. The WAL lives in
# work_dir/wal, where deploy_local.sh --wal puts it; the test refuses to start
# when that directory already holds a keeper WAL, and removes the one it made.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog ./keeper_wal_group_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../")"
MPIEXEC="${MPIEXEC:-$REPO_ROOT/.spack-env/view/bin/mpiexec}"

MONITOR_DIR="$WORK_DIR/monitor"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
BENCH="$WORK_DIR/tools/benchmark/chrono-bench"
WAL_ROOT="$WORK_DIR/wal"

RANKS=64
INGESTION_THREADS=4
EVENTS_PER_RANK=50
EVENT_BYTES=256
# the keeper logs WAL stats every 10 s
STATS_WAIT_SECS=30

PASS=0
FAIL=0

say() { echo -e "[keeper_wal_group_test] $*"; }
ok()  { say "PASS: $*"; PASS=$((PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

cleanup() {
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
    # the bench's stories are destroyed; nothing in this WAL is worth keeping
    rm -rf -- "$WAL_ROOT"/keeper-*
    rmdir -- "$WAL_ROOT" 2> /dev/null || true
}

# largest max_events_per_group over every keeper stats line, rotated logs too
max_group() {
    cat "$MONITOR_DIR"/chrono-keeper-1*.log 2> /dev/null |
        sed -n 's/.*\[KeeperWal\] stats .*max_events_per_group=\([0-9]*\).*/\1/p' |
        sort -n | tail -1
}

# the last stats line whose max_events_per_group is above the pool size
group_line() {
    cat "$MONITOR_DIR"/chrono-keeper-1*.log 2> /dev/null | grep "\[KeeperWal\] stats" |
        awk -v pool="$INGESTION_THREADS" '{
             match($0, /max_events_per_group=[0-9]+/)
             if(substr($0, RSTART + 21, RLENGTH - 21) + 0 > pool) line = $0
         } END { print line }'
}

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$BENCH" ] || { say "chrono-bench not found at $BENCH"; exit 2; }
[ -x "$MPIEXEC" ] || { say "mpiexec not found at $MPIEXEC"; exit 2; }
if [ -n "$(ls -A "$WAL_ROOT" 2> /dev/null)" ]; then
    say "$WAL_ROOT already holds a keeper WAL; clean it (deploy_local.sh -c) before running this test"
    exit 2
fi

say "patching installed conf template (keeper IngestionThreadCount=$INGESTION_THREADS, keeper log at info)"
# The keeper flushes its log at warning by default, so the INFO stats line would
# sit in spdlog's buffer; info also keeps the per-event debug lines out.
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wal_group_test_backup"
jq ".chrono_keeper.KeeperRecordingService.IngestionThreadCount = $INGESTION_THREADS |
    .chrono_keeper.Monitoring.monitor.level = \"info\" |
    .chrono_keeper.Monitoring.monitor.flushlevel = \"info\"" \
    "$CONF_TEMPLATE.wal_group_test_backup" > "$CONF_TEMPLATE.wal_group_test_patched" ||
    {
        say "conf patch failed; template left as it was"
        rm -f "$CONF_TEMPLATE.wal_group_test_backup" "$CONF_TEMPLATE.wal_group_test_patched"
        exit 2
    }
mv -f "$CONF_TEMPLATE.wal_group_test_patched" "$CONF_TEMPLATE"

restore_conf() { mv -f "$CONF_TEMPLATE.wal_group_test_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-keeper-*.log

say "deploying 1 keeper / 1 recording group with the WAL on"
"$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 --wal > /dev/null 2>&1 || { say "deploy failed"; exit 2; }
sleep 5
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-")" -lt 4 ]; then
    say "expected 4 daemons up"
    exit 2
fi

# ------------------------------------------------------- durable writes ----
say "running $RANKS ranks of chrono-bench --durable, $EVENTS_PER_RANK events of $EVENT_BYTES bytes each"
BENCH_OUT="$MONITOR_DIR/keeper_wal_group_test.bench.log"
"$MPIEXEC" -n "$RANKS" "$BENCH" -c "$CLIENT_CONF" --durable -w -n "$EVENTS_PER_RANK" -t 1 -h 1 \
    -a "$EVENT_BYTES" -s "$EVENT_BYTES" -b "$EVENT_BYTES" > "$BENCH_OUT" 2>&1
bench_status=$?
grep "^Durable" "$BENCH_OUT" | while read -r line; do say "  $line"; done

expected=$((RANKS * EVENTS_PER_RANK))
summary=$(grep "^Durable writes" "$BENCH_OUT")
sent=$(sed -n 's/.*: \([0-9]*\) sent, .*/\1/p' <<< "$summary")
acked=$(sed -n 's/.* sent, \([0-9]*\) acknowledged.*/\1/p' <<< "$summary")
if [ $bench_status -eq 0 ] && [ "${sent:-0}" -eq $expected ] && [ "${acked:-0}" -eq $expected ]; then
    ok "bench: $acked of $expected durable writes acknowledged"
else
    bad "bench: exit $bench_status, sent=${sent:-?} acknowledged=${acked:-?} of $expected (output in $BENCH_OUT)"
fi

# ----------------------------------------------------------- group size ----
deadline=$((SECONDS + STATS_WAIT_SECS))
while [ "$(max_group)" = "" ] || [ "$(max_group)" -le $INGESTION_THREADS ]; do
    [ $SECONDS -ge $deadline ] && break
    sleep 2
done
largest=$(max_group)
if [ -n "$largest" ] && [ "$largest" -gt $INGESTION_THREADS ]; then
    say "  $(group_line)"
    ok "group: max_events_per_group=$largest is above IngestionThreadCount=$INGESTION_THREADS"
else
    bad "group: max_events_per_group=${largest:-none} is not above IngestionThreadCount=$INGESTION_THREADS"
fi

# ------------------------------------------------------------------ report ----
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
