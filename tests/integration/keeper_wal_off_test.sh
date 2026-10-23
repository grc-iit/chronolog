#!/bin/bash
# Integration test for durable writes to a keeper that cannot make them
# durable: a keeper with the WAL off, and a v3.3.0 keeper, which has no
# record_event_durable RPC. Both must answer a durable write
# CL_ERR_DURABILITY_UNAVAILABLE at once, store nothing, and leave accepted
# writes working.
#
# Phase 1, this tree's keeper with the WAL off (deploy_local.sh without --wal):
#   1. The generated keeper conf has the WAL off and the keeper made no WAL
#      directory.
#   2. Every one of DURABLE_EVENTS durable writes returns
#      CL_ERR_DURABILITY_UNAVAILABLE, and the client log shows no retry.
#   3. Every one of ACCEPTED_EVENTS accepted writes to the same story, from a
#      second client, returns CL_SUCCESS.
#   4. The archive of the story ends up with exactly ACCEPTED_EVENTS events,
#      and a replay returns exactly the acknowledged accepted identities: none
#      of the durable writes was stored.
#
# Phase 2, the v3.3.0 keeper (built from the v3.3.0 tag) with this tree's
# visor, grapher and player:
#   5. The running keeper is the v3.3.0 binary.
#   6. A durable write from this tree's client returns
#      CL_ERR_DURABILITY_UNAVAILABLE after exactly one attempt: the client logs
#      "does not define record_event_durable" once per call and no attempt
#      that went unanswered, and the writes take less than one timeout_ms.
#   7. Accepted writes from the same client to that keeper return CL_SUCCESS.
#
# deploy_local.sh -p <keeper> is passed in phase 2, but deploy_local.sh resets
# KEEPER_BIN to work_dir/bin/chrono-keeper after parsing its options
# (refresh_paths_for_work_dir), so it starts this tree's keeper anyway. The
# script then stops that keeper and launches the v3.3.0 keeper itself with the
# conf deploy_local.sh generated for keeper 1, and stops it itself at the end.
#
# The v3.3.0 keeper is V330_KEEPER. When that file does not exist, the script
# extracts the v3.3.0 tag into V330_SRC with git archive, links the repo's
# .spack-env (and spack.lock, when there is one) into it, and builds and
# installs it with its own local_single_user_deploy.sh into V330_BUILD and
# V330_INSTALL, which takes several minutes.
#
# Requires an installed tree (deploy_local.sh layout) and the build tree for
# the probe. The script patches the *installed* conf template (never the source
# tree) to shorten the keeper chunks and grapher windows, deploys 1 keeper /
# 1 recording group, runs both phases, stops the deployment and restores the
# template.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        [V330_KEEPER=<v3.3.0 chrono-keeper>] ./keeper_wal_off_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../")"
H5DUMP="${H5DUMP:-$REPO_ROOT/.spack-env/view/bin/h5dump}"

V330_TAG=v3.3.0
V330_SRC="${V330_SRC:-$HOME/chronolog-build/v3.3.0-src}"
V330_BUILD="${V330_BUILD:-$HOME/chronolog-build/v3.3.0}"
V330_INSTALL="${V330_INSTALL:-$HOME/chronolog-install-v3.3.0}"
V330_KEEPER="${V330_KEEPER:-$V330_INSTALL/chronolog/bin/chrono-keeper}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
KEEPER_CONF="$WORK_DIR/conf/chrono-keeper-conf-1.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/keeper-wal/chronolog-test-durable-probe"
WAL_ROOT="$(realpath -m "$WORK_DIR/wal")"

CHRONICLE=KeeperWalOffChronicle
STORY=MixedStory
OLD_KEEPER_STORY=OldKeeperStory
DURABLE_EVENTS=20
ACCEPTED_EVENTS=50
OLD_KEEPER_EVENTS=5
# LogEventOptions defaults: a retried durable write takes up to attempts x this
TIMEOUT_MS=10000

# keeper chunks and grapher windows patched small so the archive is written soon
K_CHUNK_SECS=10
K_ACCEPT_SECS=15
G_CHUNK_SECS=10
G_ACCEPT_SECS=15
REPORT_SECS=1
VISIBILITY_SECS=3
ARCHIVE_DEADLINE=240
# after the archive holds every accepted event, how long to watch for more
ARCHIVE_SETTLE_SECS=30

NO_ANSWER_LINE='got no answer'
NOT_DEFINED_LINE='does not define record_event_durable'

PASS=0
FAIL=0

say() { echo -e "[keeper_wal_off_test $(date +%H:%M:%S)] $*"; }
ok() { say "PASS: $*"; PASS=$((PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
    pkill -9 -f "$V330_KEEPER --config" 2> /dev/null
}

# stop_old_keeper: SIGTERM, then SIGKILL after 60 s; deploy_local.sh --stop
# looks only for work_dir/bin/chrono-keeper
stop_old_keeper() {
    pkill -TERM -f "$V330_KEEPER --config" 2> /dev/null || return 0
    for _ in $(seq 1 60); do
        pgrep -f "$V330_KEEPER --config" > /dev/null || return 0
        sleep 1
    done
    pkill -9 -f "$V330_KEEPER --config" 2> /dev/null
}

cleanup() {
    stop_old_keeper
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

now_ms() { echo $(($(date +%s%N) / 1000000)); }

keeper_pid() { pgrep -f "chrono-keeper --config $KEEPER_CONF" | head -1; }

h5_story_events() { # $1 = story; total events across its archive files
    local total=0 f n
    for f in "$OUTPUT_DIR/$CHRONICLE/$1"/*.h5; do
        [ -f "$f" ] || continue
        n=$("$H5DUMP" -H -d /story_chunks/data.vlen_bytes "$f" 2> /dev/null |
            sed -n 's/.*DATASPACE *SIMPLE *{ *( *\([0-9]*\) *).*/\1/p' | head -1)
        total=$((total + ${n:-0}))
    done
    echo "$total"
}

identities() { grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$1" | sort -u; } # "<time> <client id> <index>" lines

# client_conf <label> <query port>: a client conf of its own for each probe, with
# its own query port and a log file in RUN_DIR flushed on every line
client_conf() {
    jq ".chrono_client.ClientQueryService.rpc.service_base_port = $2 |
        .chrono_client.Monitoring.monitor.file = \"$RUN_DIR/$1_client.log\" |
        .chrono_client.Monitoring.monitor.level = \"debug\" |
        .chrono_client.Monitoring.monitor.flushlevel = \"debug\"" \
        "$CLIENT_CONF" > "$RUN_DIR/$1_client_conf.json" || { say "$1 client conf patch failed"; exit 2; }
}

# write <label> <story> <n> <mode>: sets W_ACKED, W_CODES, W_MS, W_NO_ANSWER and
# W_NOT_DEFINED; the acknowledged identities go to $RUN_DIR/acked_<label>
write_story() {
    local out="$RUN_DIR/write_$1.out" start_ms
    start_ms=$(now_ms)
    "$PROBE" --config "$RUN_DIR/$1_client_conf.json" --chronicle "$CHRONICLE" --story "$2" --write "$3" --mode "$4" \
        > "$out" 2>&1
    W_MS=$(($(now_ms) - start_ms))
    identities "$out" > "$RUN_DIR/acked_$1"
    W_ACKED=$(wc -l < "$RUN_DIR/acked_$1")
    W_CODES=$(sed -n 's/^WRITE_CODE //p' "$out" | paste -sd ',' -)
    # the logger rotates at filesize; count across every file
    W_NO_ANSWER=$(cat "$RUN_DIR/$1_client"*.log 2> /dev/null | grep -c "$NO_ANSWER_LINE")
    W_NOT_DEFINED=$(cat "$RUN_DIR/$1_client"*.log 2> /dev/null | grep -c "$NOT_DEFINED_LINE")
    say "$4 writes ($1) to $2: $W_ACKED acked of $3 in ${W_MS} ms; codes: ${W_CODES:-none} ($(sed -n 's/^WRITE_STATUS //p' "$out"))"
}

# count of WRITE_CODE <name> in the last write's codes
code_count() { tr ',' '\n' <<< "$W_CODES" | sed -n "s/^$1 //p"; }

# replay <story> <label>: sets R_STATUS; the returned identities go to $RUN_DIR/replay_<label>.ids
replay_story() {
    local out="$RUN_DIR/replay_$2.out"
    "$PROBE" --config "$RUN_DIR/$2_client_conf.json" --chronicle "$CHRONICLE" --story "$1" --replay > "$out" 2>&1
    R_STATUS=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$out")
    grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$out" > "$RUN_DIR/replay_$2.ids"
    say "replay of $1: status ${R_STATUS:-?}, $(wc -l < "$RUN_DIR/replay_$2.ids") event(s)"
}

# Extracts the v3.3.0 tag and builds its keeper with that tree's own deploy
# script, which installs the whole v3.3.0 tree into V330_INSTALL.
build_v330_keeper() {
    say "building the $V330_TAG keeper: source $V330_SRC, build $V330_BUILD, install $V330_INSTALL"
    if [ ! -f "$V330_SRC/CMakeLists.txt" ]; then
        mkdir -p "$V330_SRC" || return 1
        git -C "$REPO_ROOT" archive "$V330_TAG" | tar -x -C "$V330_SRC" || return 1
    fi
    # the spack environment is the repo's; without its lock the build would
    # concretize the environment again
    [ -e "$V330_SRC/.spack-env" ] || ln -s "$(realpath "$REPO_ROOT/.spack-env")" "$V330_SRC/.spack-env" || return 1
    [ -e "$V330_SRC/spack.lock" ] || [ ! -f "$REPO_ROOT/spack.lock" ] || cp "$REPO_ROOT/spack.lock" "$V330_SRC/spack.lock"
    "$V330_SRC/tools/deploy/local_single_user_deploy.sh" -b -t Debug -B "$V330_BUILD" -I "$V330_INSTALL" \
        > "$RUN_DIR/v330_build.out" 2>&1 &&
        "$V330_SRC/tools/deploy/local_single_user_deploy.sh" -i -t Debug -B "$V330_BUILD" -I "$V330_INSTALL" \
            >> "$RUN_DIR/v330_build.out" 2>&1
}

deploy() { # extra deploy_local.sh options; WAL_ENABLED=0 so the environment cannot turn the WAL on
    WAL_ENABLED=0 "$DEPLOY" -d -w "$WORK_DIR" -k 1 -r 1 "$@" > "$RUN_DIR/deploy.out" 2>&1 ||
        { say "deploy failed (see output below)"; cat "$RUN_DIR/deploy.out"; exit 2; }
    sleep 3
    if [ "$(pgrep -c -f "chrono-(visor|keeper|grapher|player) --config $WORK_DIR/conf/")" -lt 4 ]; then
        say "expected 4 daemons up"; exit 2
    fi
}

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -x "$H5DUMP" ] || { say "h5dump not found at $H5DUMP"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }

RUN_DIR=$(mktemp -d /tmp/keeper_wal_off.XXXXXX)
if [ ! -x "$V330_KEEPER" ]; then
    build_v330_keeper && [ -x "$V330_KEEPER" ] ||
        { say "$V330_TAG keeper build failed; last lines of $RUN_DIR/v330_build.out:"; tail -20 "$RUN_DIR/v330_build.out"; exit 2; }
fi
V330_KEEPER="$(realpath "$V330_KEEPER")"
say "$V330_TAG keeper: $V330_KEEPER"

say "patching installed conf template: keeper ${K_CHUNK_SECS}s/${K_ACCEPT_SECS}s chunks, grapher ${G_CHUNK_SECS}s/${G_ACCEPT_SECS}s windows, report ${REPORT_SECS}s, keeper visibility ${VISIBILITY_SECS}s"
# jq writes to a side file that replaces the template only on success, so a failed
# patch leaves the template intact (the restore trap below is not set up yet)
cp "$CONF_TEMPLATE" "$CONF_TEMPLATE.wal_off_backup"
jq ".chrono_keeper.DataStoreInternals.story_chunk_duration_secs = $K_CHUNK_SECS |
    .chrono_keeper.DataStoreInternals.acceptance_window_secs = $K_ACCEPT_SECS |
    .chrono_keeper.DataStoreInternals.archive_visibility_delay_secs = $VISIBILITY_SECS |
    .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = 30 |
    .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
    .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
    .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
    "$CONF_TEMPLATE.wal_off_backup" > "$CONF_TEMPLATE.wal_off_patched" ||
    { say "conf patch failed; template left as it was"; rm -f "$CONF_TEMPLATE.wal_off_backup" "$CONF_TEMPLATE.wal_off_patched"; exit 2; }
mv -f "$CONF_TEMPLATE.wal_off_patched" "$CONF_TEMPLATE"

restore_conf() { mv -f "$CONF_TEMPLATE.wal_off_backup" "$CONF_TEMPLATE"; }
trap 'cleanup; restore_conf; rm -rf "$RUN_DIR"' EXIT

kill_daemons
sleep 2
rm -f "$MONITOR_DIR"/chrono-*.log
rm -rf "${OUTPUT_DIR:?}/$CHRONICLE" "$OUTPUT_DIR"/"$CHRONICLE".*.h5
# a WAL directory left by an earlier run would hide whether this keeper made one
rm -rf "$WAL_ROOT/keeper-1"

# the writers and the replay probe bind their query services on ports of their
# own, so a probe never waits for the previous one's port to be released
client_conf durable 5564
client_conf accepted 5565
client_conf replay 5563
client_conf old_durable 5566
client_conf old_accepted 5567

# ------------------------------------------------ phase 1: WAL off ----
say "phase 1: deploying 1 keeper / 1 recording group with the WAL off"
deploy
wal_conf=$(jq -c '.chrono_keeper.WAL.enabled' "$KEEPER_CONF")
if [ "$wal_conf" = "false" ]; then
    ok "WAL off: $KEEPER_CONF has WAL.enabled false"
else
    bad "WAL off: $KEEPER_CONF has WAL.enabled $wal_conf"
fi

write_story durable "$STORY" "$DURABLE_EVENTS" durable
unavailable=$(code_count CL_ERR_DURABILITY_UNAVAILABLE)
if [ "${unavailable:-0}" -eq "$DURABLE_EVENTS" ] && [ "$W_NO_ANSWER" -eq 0 ] && [ "$W_NOT_DEFINED" -eq 0 ]; then
    ok "WAL off: all $DURABLE_EVENTS durable writes got CL_ERR_DURABILITY_UNAVAILABLE in ${W_MS} ms, no retry logged"
else
    bad "WAL off: durable writes got ${W_CODES:-no codes}; $W_NO_ANSWER unanswered attempt(s) and $W_NOT_DEFINED missing-RPC line(s) logged"
fi

write_story accepted "$STORY" "$ACCEPTED_EVENTS" accepted
if [ "$W_ACKED" -eq "$ACCEPTED_EVENTS" ]; then
    ok "WAL off: all $ACCEPTED_EVENTS accepted writes acknowledged"
else
    bad "WAL off: $W_ACKED of $ACCEPTED_EVENTS accepted writes acknowledged ($W_CODES)"
fi

if [ ! -e "$WAL_ROOT/keeper-1" ]; then
    ok "WAL off: the keeper made no WAL directory $WAL_ROOT/keeper-1"
else
    bad "WAL off: $WAL_ROOT/keeper-1 exists: $(ls -A "$WAL_ROOT/keeper-1" | paste -sd ' ' -)"
fi

say "waiting up to ${ARCHIVE_DEADLINE}s for $ACCEPTED_EVENTS events of $STORY in the archive, then ${ARCHIVE_SETTLE_SECS}s for any more"
deadline=$((SECONDS + ARCHIVE_DEADLINE))
archived=$(h5_story_events "$STORY")
while [ "$archived" -lt "$ACCEPTED_EVENTS" ] && [ $SECONDS -lt $deadline ]; do
    [ -n "$(keeper_pid)" ] || break
    sleep 5
    archived=$(h5_story_events "$STORY")
done
if [ "$archived" -ge "$ACCEPTED_EVENTS" ]; then
    sleep "$ARCHIVE_SETTLE_SECS"
    archived=$(h5_story_events "$STORY")
fi
if [ -z "$(keeper_pid)" ]; then
    bad "archive: the keeper exited; see $MONITOR_DIR/chrono-keeper-1.launch.log"
elif [ "$archived" -eq "$ACCEPTED_EVENTS" ]; then
    ok "archive: $STORY holds exactly the $ACCEPTED_EVENTS accepted events"
else
    bad "archive: $STORY holds $archived events, expected the $ACCEPTED_EVENTS accepted ones"
fi

replay_story "$STORY" replay
missing=$(comm -23 "$RUN_DIR/acked_accepted" <(sort -u "$RUN_DIR/replay_replay.ids") | wc -l)
unexpected=$(comm -13 "$RUN_DIR/acked_accepted" <(sort -u "$RUN_DIR/replay_replay.ids") | wc -l)
returned=$(wc -l < "$RUN_DIR/replay_replay.ids")
if [ "$R_STATUS" != "CL_SUCCESS" ]; then
    bad "replay: status ${R_STATUS:-none}"
    grep -Ev '^[0-9]+ [0-9]+ [0-9]+$' "$RUN_DIR/replay_replay.out" | tail -5 | sed 's/^/    /'
elif [ "$missing" -eq 0 ] && [ "$unexpected" -eq 0 ] && [ "$returned" -eq "$ACCEPTED_EVENTS" ]; then
    ok "replay: returns exactly the $ACCEPTED_EVENTS acknowledged accepted events and none of the durable writes"
else
    bad "replay: $returned event(s), $missing acknowledged accepted event(s) missing, $unexpected not acknowledged"
fi

say "stopping phase 1"
"$DEPLOY" -s -w "$WORK_DIR" > "$RUN_DIR/stop1.out" 2>&1 || { bad "phase 1 stop failed"; tail -5 "$RUN_DIR/stop1.out"; }
kill_daemons
sleep 2

# --------------------------------------------- phase 2: v3.3.0 keeper ----
say "phase 2: deploying with -p $V330_KEEPER"
deploy -p "$V330_KEEPER"
pid=$(keeper_pid)
if [ -n "$pid" ] && [ "$(readlink -f "/proc/$pid/exe")" != "$V330_KEEPER" ]; then
    # see the header: deploy_local.sh ignores -p
    say "deploy_local.sh started $(readlink -f "/proc/$pid/exe"), not the $V330_TAG keeper; relaunching keeper 1 with $V330_KEEPER"
    kill -TERM "$pid"
    for _ in $(seq 1 60); do kill -0 "$pid" 2> /dev/null || break; sleep 1; done
    kill -9 "$pid" 2> /dev/null
    nohup "$V330_KEEPER" --config "$KEEPER_CONF" >> "$MONITOR_DIR/chrono-keeper-1.launch.log" 2>&1 &
    sleep 3
fi
pid=$(keeper_pid)
running=$([ -n "$pid" ] && readlink -f "/proc/$pid/exe")
if [ "$running" = "$V330_KEEPER" ]; then
    ok "$V330_TAG keeper: keeper 1 (pid $pid) runs $V330_KEEPER"
else
    bad "$V330_TAG keeper: keeper 1 runs '${running:-nothing}'"; exit 1
fi

# the keeper may not have registered with the visor yet: the probe then fails
# to acquire the story and logs no write
for _ in $(seq 1 10); do
    write_story old_accepted "$OLD_KEEPER_STORY" "$OLD_KEEPER_EVENTS" accepted
    grep -q '^WRITE_ACKED' "$RUN_DIR/write_old_accepted.out" && break
    sleep 3
done
if [ "$W_ACKED" -eq "$OLD_KEEPER_EVENTS" ]; then
    ok "$V330_TAG keeper: all $OLD_KEEPER_EVENTS accepted writes acknowledged"
else
    bad "$V330_TAG keeper: $W_ACKED of $OLD_KEEPER_EVENTS accepted writes acknowledged (${W_CODES:-no codes})"
fi

write_story old_durable "$OLD_KEEPER_STORY" "$OLD_KEEPER_EVENTS" durable
unavailable=$(code_count CL_ERR_DURABILITY_UNAVAILABLE)
if [ "${unavailable:-0}" -eq "$OLD_KEEPER_EVENTS" ] && [ "$W_NOT_DEFINED" -eq "$OLD_KEEPER_EVENTS" ] &&
    [ "$W_NO_ANSWER" -eq 0 ] && [ "$W_MS" -lt "$TIMEOUT_MS" ]; then
    ok "$V330_TAG keeper: all $OLD_KEEPER_EVENTS durable writes got CL_ERR_DURABILITY_UNAVAILABLE after one attempt each ($W_NOT_DEFINED missing-RPC lines, no unanswered attempt, ${W_MS} ms)"
else
    bad "$V330_TAG keeper: durable writes got ${W_CODES:-no codes} in ${W_MS} ms; $W_NOT_DEFINED missing-RPC line(s) and $W_NO_ANSWER unanswered attempt(s) logged, expected $OLD_KEEPER_EVENTS and 0"
fi

say "stopping phase 2"
stop_old_keeper
"$DEPLOY" -s -w "$WORK_DIR" > "$RUN_DIR/stop2.out" 2>&1 || { bad "phase 2 stop failed"; tail -5 "$RUN_DIR/stop2.out"; }

# ------------------------------------------------------------------ report ----
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
