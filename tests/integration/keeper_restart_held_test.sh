#!/bin/bash
# Integration test for a keeper restarted while clients hold stories: after the
# restart the keeper must get pipelines for the held stories from the visor,
# both accepted and durable writes to it must land, the player's hot fetch must
# reach it, and the client must reconnect to it at the same address.
#
# Two keepers in one recording group, WAL on (deploy_local.sh --wal). Writer A
# acquires story A and logs an event every INTERVAL_MS, odd calls accepted and
# even calls durable (durable calls with a 1 s deadline and one attempt). The
# client sends an event to keeper (time % 2) of the story's keeper list, so
# each keeper gets the events of one parity of the event time. The scenario:
#
#   - PRE_SECS of writes, with a replay of A after a few seconds so the player
#     has a client of both keepers before the kill;
#   - keeper 2 is killed with SIGKILL; DOWN_SECS later it is restarted with
#     deploy_local.sh --restart-keeper 2, which reuses its conf and WAL;
#     while it is down, writer B acquires story B and writes the same way;
#   - POST_SECS of writes once writer A's events reach keeper 2 again; then
#     both writers stop writing and keep holding their stories;
#   - HOT_WAIT_SECS after the last event, a replay of each story; then both
#     writers release their stories, and the archive is read with h5dump.
#
# Checks, per protocol:
#
#   1. While keeper 2 is down only its events fail: writer A's calls succeed
#      at keeper 1's time parity and fail at keeper 2's, no call takes over
#      5 s, every failure the clients log names keeper 2, and nothing fails
#      once keeper 2 is back. Story B can be acquired while keeper 2 is down.
#      Writer A's durable calls to keeper 2 in that time return
#      CL_ERR_DURABILITY_UNKNOWN (or succeed once it is restarted), since
#      keeper 2 had answered them before the kill, and its accepted calls to
#      keeper 2 fail.
#   2. The client reconnects: after the restart, accepted and durable events of
#      keeper 2's parity succeed, and nothing fails after that.
#   3. A replay of A while its post-restart events are on the keepers only
#      (sealed, but not yet written by the grapher) returns every acked event
#      of keeper 2 in that range: the player reached the restarted keeper. A
#      replay of B returns every sealed event of B.
#   4. The archive holds every acked post-restart event, accepted and durable,
#      of both stories, every durable event keeper 2 acked before the kill
#      (recovered from its WAL), and every event keeper 1 acked.
#   5. deploy_local.sh --stop finds both keepers' exit-summary with no pending
#      durable events.
#   6. The restarted keeper's log shows the visor's start_story_recording for
#      A and B, and the visor's log shows it started every held story on it.
#
# Story B is acquired while keeper 2 is down, and whether its keeper list then
# holds keeper 2 is not settled yet. The script prints the keepers the visor
# gave writer B and B's post-restart successes at keeper 2's parity, without
# judging them.
#
# The scenario runs once with the template's protocol and once with ofi+tcp if
# the template uses another one (PROTOCOLS="..." overrides the list). The
# keeper and grapher chunk and acceptance times are pinned, since check 3
# depends on them: an event is sealed on its keeper at most
# K_CHUNK_SECS + K_ACCEPT_SECS after its time, and the grapher writes it no
# sooner than G_ACCEPT_SECS after its time.
#
# Requires an installed tree (deploy_local.sh layout) and the build tree for
# the probe. The script patches the *installed* conf template (never the source
# tree), deploys 2 keepers / 1 recording group per protocol, stops the
# deployment and restores the template on exit. A failed run keeps its files
# in the directory it prints.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./keeper_restart_held_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../")"
H5DUMP="${H5DUMP:-$REPO_ROOT/.spack-env/view/bin/h5dump}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_DIR="$WORK_DIR/conf"
CONF_TEMPLATE="$CONF_DIR/default-chrono-conf.json"
CLIENT_CONF="$CONF_DIR/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/keeper-wal/chronolog-test-durable-probe"

CHRONICLE=KeeperRestartChronicle
STORY_A=HeldStoryA
STORY_B=HeldStoryB

INTERVAL_MS=100
PRE_SECS=15
DOWN_SECS=20
POST_SECS=30
RECONNECT_DEADLINE=90
ARCHIVE_DEADLINE=240

K_CHUNK_SECS=10
K_ACCEPT_SECS=15
G_CHUNK_SECS=30
G_ACCEPT_SECS=60
REPORT_SECS=1
# an event is sealed on its keeper by SEALED_AFTER_SECS after its time, and in
# the archive no sooner than G_ACCEPT_SECS after it; the replay of check 3
# comes HOT_WAIT_SECS after the last event and looks at the events between
# HOT_FROM_SECS and SEALED_AFTER_SECS before the replay started
SEALED_AFTER_SECS=$((K_CHUNK_SECS + K_ACCEPT_SECS + 5))
HOT_FROM_SECS=$((G_ACCEPT_SECS - 10))
HOT_WAIT_SECS=$((SEALED_AFTER_SECS + 2))

PASS=0
FAIL=0

say() { echo -e "[keeper_restart_held_test $(date +%H:%M:%S)] $*"; }
ok() { say "PASS: $*"; PASS=$((PASS + 1)); RUN_PASS=$((RUN_PASS + 1)); }
bad() { say "FAIL: $*"; FAIL=$((FAIL + 1)); RUN_FAIL=$((RUN_FAIL + 1)); }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin" 2> /dev/null
    done
}

stop_writers() {
    local pid
    for pid in ${WRITER_PIDS:-}; do kill -9 "$pid" 2> /dev/null; done
    WRITER_PIDS=""
}

# true once every writer process has exited, false after $1 seconds
writers_exited() {
    local deadline=$((SECONDS + $1)) pid
    for pid in ${WRITER_PIDS:-}; do
        while kill -0 "$pid" 2> /dev/null; do
            [ $SECONDS -ge $deadline ] && return 1
            sleep 0.5
        done
    done
}

cleanup() {
    stop_writers
    "$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
    kill_daemons
}

now_ns() { date +%s%N; }

keeper_pid() { pgrep -f "$WORK_DIR/bin/chrono-keeper --config .*chrono-keeper-conf-$1.json" | head -1; }

# "<time> <client id> <index>" of the archived events of story $1, one per copy
archive_ids() {
    local f
    for f in "$OUTPUT_DIR/$CHRONICLE/$1"/*.h5; do
        [ -f "$f" ] || continue
        "$H5DUMP" -d /story_chunks/data.vlen_bytes "$f" 2> /dev/null
    done | awk '/^ *\([0-9]+\): \{/ { n = 1; next }
                n >= 1 && n <= 4 { gsub(/[ ,]/, ""); v[n] = $0; n++
                                   if(n == 5) { print v[2], v[3], v[4]; n = 0 } }'
}

# EVENT lines of writer output $1 as "<mode> <code> <time> <client id> <index> <wall ns> <time parity>"
events() { sed -n 's/^EVENT //p' "$1" | awk '{ print $0, substr($3, length($3), 1) % 2 }'; }

# identities of the acked events of writer output $1 for which the awk condition $2 holds;
# fields: mode code time client index wall parity
acked_ids() {
    events "$1" | awk -v kill="$KILL_NS" -v restart="$RESTART_NS" -v p="${K2_PARITY:--1}" \
        "\$2 == \"CL_SUCCESS\" && ($2) { print \$3, \$4, \$5 }" | sort -u
}

# writer <label> <story> <query port>: starts a looping writer in the background
start_writer() {
    local conf="$RUN_DIR/$1_client_conf.json"
    jq --arg proto "$PROTO" --arg log "$RUN_DIR/$1.client.log" --argjson port "$3" \
        '(.. | objects | select(has("protocol_conf")) | .protocol_conf) |= $proto |
         .chrono_client.ClientQueryService.rpc.service_base_port = $port |
         .chrono_client.Monitoring.monitor.file = $log |
         .chrono_client.Monitoring.monitor.filesize = 104857600' "$CLIENT_CONF" > "$conf" || return 1
    "$PROBE" --config "$conf" --chronicle "$CHRONICLE" --story "$2" --loop --interval-ms "$INTERVAL_MS" \
        --timeout-ms 1000 --attempts 1 --stop-file "$RUN_DIR/stop" --release-file "$RUN_DIR/release" \
        > "$RUN_DIR/$1.out" 2>&1 &
    WRITER_PIDS="$WRITER_PIDS $!"
}

wait_for_line() { # <file> <pattern> <secs>
    local deadline=$((SECONDS + $3))
    until grep -q "$2" "$1" 2> /dev/null; do
        [ $SECONDS -ge $deadline ] && return 1
        sleep 0.5
    done
}

# replay <story> <label>: sets R_STATUS; the returned identities go to $RUN_DIR/replay_<label>.ids
replay_story() {
    local out="$RUN_DIR/replay_$2.out"
    "$PROBE" --config "$RUN_DIR/replay_client_conf.json" --chronicle "$CHRONICLE" --story "$1" --replay > "$out" 2>&1
    R_STATUS=$(sed -n 's/^REPLAY_STATUS \(.*\)$/\1/p' "$out")
    grep -E '^[0-9]+ [0-9]+ [0-9]+$' "$out" | sort -u > "$RUN_DIR/replay_$2.ids"
    say "replay of $1: status ${R_STATUS:-?}, $(wc -l < "$RUN_DIR/replay_$2.ids") event(s)"
}

missing() { comm -23 "$1" "$2" | wc -l; } # lines of sorted $1 not in sorted $2

# ------------------------------------------------------------ scenario ----
run_scenario() {
    PROTO="$1"
    RUN_PASS=0
    RUN_FAIL=0
    RUN_DIR="$RUN_ROOT/${PROTO//[^a-z0-9]/_}"
    mkdir -p "$RUN_DIR"
    WRITER_PIDS=""
    KILL_NS=0
    RESTART_NS=0
    K2_PARITY=""
    say "=== protocol $PROTO (files in $RUN_DIR) ==="

    # the extraction modules name their engine's protocol in extraction_protocol
    jq --arg proto "$PROTO" "(.. | objects | select(has(\"protocol_conf\")) | .protocol_conf) |= \$proto |
        (.. | objects | select(has(\"extraction_protocol\")) | .extraction_protocol) |= \$proto |
        .chrono_keeper.DataStoreInternals.story_chunk_duration_secs = $K_CHUNK_SECS |
        .chrono_keeper.DataStoreInternals.acceptance_window_secs = $K_ACCEPT_SECS |
        .chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs = 60 |
        .chrono_keeper.Monitoring.monitor.filesize = 104857600 |
        .chrono_visor.Monitoring.monitor.filesize = 104857600 |
        .chrono_grapher.DataStoreInternals.story_chunk_duration_secs = $G_CHUNK_SECS |
        .chrono_grapher.DataStoreInternals.acceptance_window_secs = $G_ACCEPT_SECS |
        .chrono_grapher.DataStoreInternals.watermark_report_interval_secs = $REPORT_SECS" \
        "$CONF_TEMPLATE.restart_held_backup" > "$CONF_TEMPLATE.restart_held_patched" ||
        { bad "$PROTO: conf patch failed"; return 1; }
    mv -f "$CONF_TEMPLATE.restart_held_patched" "$CONF_TEMPLATE"
    jq --arg proto "$PROTO" --arg log "$RUN_DIR/replay.client.log" \
        '(.. | objects | select(has("protocol_conf")) | .protocol_conf) |= $proto |
        .chrono_client.ClientQueryService.rpc.service_base_port = 5563 |
        .chrono_client.Monitoring.monitor.file = $log' \
        "$CLIENT_CONF" > "$RUN_DIR/replay_client_conf.json" || { bad "$PROTO: client conf patch failed"; return 1; }

    kill_daemons
    sleep 2
    rm -f "$MONITOR_DIR"/chrono-*.log
    rm -rf "${OUTPUT_DIR:?}/$CHRONICLE"
    # a WAL left by an earlier run would be recovered into this one
    rm -rf "$WORK_DIR/wal/keeper-1" "$WORK_DIR/wal/keeper-2"

    say "deploying 2 keepers / 1 recording group over $PROTO with the WAL on"
    "$DEPLOY" -d -w "$WORK_DIR" -k 2 -r 1 --wal > "$RUN_DIR/deploy.out" 2>&1 ||
        { bad "$PROTO: deploy failed"; cat "$RUN_DIR/deploy.out"; return 1; }
    sleep 8
    if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-(visor|keeper|grapher|player) --config")" -lt 5 ]; then
        bad "$PROTO: expected 5 daemons up"; return 1
    fi
    local k1_port k2_port
    k1_port=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.service_base_port' "$CONF_DIR/chrono-keeper-conf-1.json")
    k2_port=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.service_base_port' "$CONF_DIR/chrono-keeper-conf-2.json")
    # A story's keeper list comes from the visor in the order of its keeper
    # map, keyed by (ip, port), and the client sends an event to
    # list[time % 2]; both keepers listen on one ip, so keeper 2, on the higher
    # port, gets the events of odd times. Check 1 confirms it while keeper 2 is
    # down, when only keeper 1's parity can succeed.
    [ "$k1_port" -lt "$k2_port" ] || { bad "$PROTO: keeper 1's port $k1_port is not below keeper 2's $k2_port"; return 1; }
    K2_PARITY=1

    # --------------------------------------------- writes, then the kill ----
    start_writer writerA "$STORY_A" 5564 || { bad "$PROTO: writer A conf patch failed"; return 1; }
    wait_for_line "$RUN_DIR/writerA.out" '^ACQUIRED' 30 && grep -q '^ACQUIRED CL_SUCCESS' "$RUN_DIR/writerA.out" ||
        { bad "$PROTO: writer A could not acquire $STORY_A: $(head -3 "$RUN_DIR/writerA.out")"; return 1; }
    sleep 5
    # gives the player a client of both keepers, so the replay after the restart
    # goes through the one it made for the keeper that died
    replay_story "$STORY_A" before_kill
    [ "$R_STATUS" = "CL_SUCCESS" ] || bad "$PROTO: replay of $STORY_A before the kill: status ${R_STATUS:-none}"
    sleep $((PRE_SECS - 5))

    local pid
    pid=$(keeper_pid 2)
    [ -n "$pid" ] || { bad "$PROTO: keeper 2 not running before the kill"; return 1; }
    kill -9 "$pid"
    KILL_NS=$(now_ns)
    say "killed keeper 2 (pid $pid)"

    sleep 3
    start_writer writerB "$STORY_B" 5566 || { bad "$PROTO: writer B conf patch failed"; return 1; }
    local acquire_secs=$((DOWN_SECS - 5))
    if wait_for_line "$RUN_DIR/writerB.out" '^ACQUIRED' $acquire_secs && grep -q '^ACQUIRED CL_SUCCESS' "$RUN_DIR/writerB.out"; then
        ok "$PROTO: $STORY_B acquired while keeper 2 was down"
    else
        bad "$PROTO: acquire of $STORY_B while keeper 2 was down did not succeed within ${acquire_secs}s: $(head -1 "$RUN_DIR/writerB.out")"
    fi
    while [ $((($(now_ns) - KILL_NS) / 1000000000)) -lt $DOWN_SECS ]; do sleep 0.5; done

    # the restarted keeper writes a new log; keep the old one apart
    mkdir -p "$RUN_DIR/keeper2-before-restart"
    find "$MONITOR_DIR" -maxdepth 1 -name 'chrono-keeper-2*.log' ! -name '*.launch.log' \
        -exec mv -f {} "$RUN_DIR/keeper2-before-restart/" \;
    RESTART_NS=$(now_ns)
    "$DEPLOY" --restart-keeper 2 -w "$WORK_DIR" > "$RUN_DIR/restart.out" 2>&1 ||
        { bad "$PROTO: deploy_local.sh --restart-keeper 2 failed"; cat "$RUN_DIR/restart.out"; return 1; }
    say "restarted keeper 2"
    wait_for_line "$RUN_DIR/writerB.out" '^ACQUIRED' 60 && grep -q '^ACQUIRED CL_SUCCESS' "$RUN_DIR/writerB.out" ||
        { bad "$PROTO: writer B could not acquire $STORY_B after the restart: $(head -1 "$RUN_DIR/writerB.out")"; return 1; }

    # ------------------------------------------------------- reconnect ----
    local reconnect_ns=""
    local deadline=$((SECONDS + RECONNECT_DEADLINE))
    while [ $SECONDS -lt $deadline ]; do
        [ -n "$(keeper_pid 2)" ] || break
        if [ -n "$(acked_ids "$RUN_DIR/writerA.out" '$6 > restart && $7 == p && $1 == "accepted"')" ] &&
           [ -n "$(acked_ids "$RUN_DIR/writerA.out" '$6 > restart && $7 == p && $1 == "durable"')" ]; then
            reconnect_ns=$(now_ns)
            break
        fi
        sleep 0.5
    done
    if [ -z "$(keeper_pid 2)" ]; then
        bad "$PROTO: the restarted keeper 2 exited; see $MONITOR_DIR/chrono-keeper-2.launch.log"; return 1
    elif [ -n "$reconnect_ns" ]; then
        ok "$PROTO: reconnect: accepted and durable events reach keeper 2 again $(((reconnect_ns - RESTART_NS) / 1000000)) ms after the restart"
    else
        bad "$PROTO: reconnect: no accepted and durable success on keeper 2 within ${RECONNECT_DEADLINE}s of the restart"
    fi
    [ -n "$reconnect_ns" ] || reconnect_ns=$(now_ns)

    sleep "$POST_SECS"
    touch "$RUN_DIR/stop"
    wait_for_line "$RUN_DIR/writerA.out" '^LOOP_STOPPED' 30 && wait_for_line "$RUN_DIR/writerB.out" '^LOOP_STOPPED' 30 ||
        bad "$PROTO: a writer did not stop writing"
    say "writer A: $(grep -c '^EVENT' "$RUN_DIR/writerA.out") calls, writer B: $(grep -c '^EVENT' "$RUN_DIR/writerB.out") calls; codes A: $(events "$RUN_DIR/writerA.out" | awk '{ c[$1 " " $2]++ } END { for(k in c) printf "%s=%d ", k, c[k] }')"

    # -------------------------------------------- failures while down ----
    local k1_up_while_down k2_up_while_down fails_outside longest_call
    k1_up_while_down=$(events "$RUN_DIR/writerA.out" | awk -v kill="$KILL_NS" -v restart="$RESTART_NS" -v p="$K2_PARITY" \
        '$2 == "CL_SUCCESS" && $6 > kill + 1e9 && $6 < restart && $7 != p' | wc -l)
    k2_up_while_down=$(events "$RUN_DIR/writerA.out" | awk -v kill="$KILL_NS" -v restart="$RESTART_NS" -v p="$K2_PARITY" \
        '$2 == "CL_SUCCESS" && $6 > kill + 1e9 && $6 < restart && $7 == p' | wc -l)
    # the longest any call of writer A took from the kill on, in ms: a call
    # that waits on the dead keeper holds up every later event of the writer
    longest_call=$(events "$RUN_DIR/writerA.out" | awk -v kill="$KILL_NS" -v interval="$INTERVAL_MS" \
        'prev != "" && prev > kill - 2e9 { d = ($6 - prev) / 1e6 - interval; if(d > m) m = d } { prev = $6 }
         END { printf "%d\n", m }')
    fails_outside=$(events "$RUN_DIR/writerA.out" | awk -v kill="$KILL_NS" -v rec="$reconnect_ns" \
        '$2 != "CL_SUCCESS" && ($6 < kill - 1e9 || $6 > rec)' | wc -l)
    local a_fails b_fails log_k2 log_ports log_other
    a_fails=$(events "$RUN_DIR/writerA.out" | awk '$2 != "CL_SUCCESS"' | wc -l)
    b_fails=$(events "$RUN_DIR/writerB.out" | awk '$2 != "CL_SUCCESS"' | wc -l)
    # the client logs one of these per failed call, naming the keeper
    log_ports=$(cat "$RUN_DIR"/writer[AB].client.log* 2> /dev/null |
        grep -E '\[KeeperRecordingClient\] (Failed to send|Durable event to|Keeper ServiceId.*does not define)' |
        sed -n 's/.*ServiceId{[^:]*:[^:]*:\([0-9]*\):.*/\1/p' | sort | uniq -c)
    log_k2=$(echo "$log_ports" | awk -v k2="$k2_port" 'NF == 2 && $2 == k2 { n += $1 } END { print n + 0 }')
    log_other=$(echo "$log_ports" | awk -v k2="$k2_port" 'NF == 2 && $2 != k2 { n += $1 } END { print n + 0 }')
    if [ "$a_fails" -gt 0 ] && [ "$k1_up_while_down" -gt 0 ] && [ "$k2_up_while_down" -eq 0 ] &&
       [ "$fails_outside" -eq 0 ] && [ "$longest_call" -lt 5000 ] &&
       [ "$log_k2" -ge $((a_fails + b_fails)) ] && [ "$log_other" -eq 0 ]; then
        ok "$PROTO: down: only keeper 2's events failed: $a_fails failed call(s) of writer A and $b_fails of writer B, all between the kill and the reconnect, while $k1_up_while_down of A's events reached keeper 1; no call took over ${longest_call} ms; the clients logged $log_k2 failure(s), all for keeper 2 (port $k2_port)"
    else
        bad "$PROTO: down: $a_fails failed call(s) of A ($fails_outside outside the down time) and $b_fails of B; while keeper 2 was down $k1_up_while_down of A's events reached keeper 1 and $k2_up_while_down succeeded at keeper 2's parity; the longest call of A took ${longest_call} ms; client failure log lines per keeper port: $(echo $log_ports)"
    fi
    say "$PROTO: codes of the failed calls: $(cat "$RUN_DIR"/writer[AB].out | sed -n 's/^EVENT //p' |
        awk '$2 != "CL_SUCCESS" { c[$1 " " $2]++ } END { for(k in c) printf "%s=%d ", k, c[k] }')"
    # Writer A's calls at keeper 2's parity while it is down. Keeper 2 answered
    # writer A's durable calls before the kill, so a durable call to it now got
    # no answer: it may have been stored, and the client returns
    # CL_ERR_DURABILITY_UNKNOWN, never CL_ERR_DURABILITY_UNAVAILABLE ("nothing
    # was stored"). After the restart a durable call may also succeed. An
    # accepted call sent before the restart fails with some error code. A failed
    # call prints event time 0, so its parity says nothing; the calls of keeper 2
    # are all but the successes at keeper 1's parity (the check above ties every
    # failure to keeper 2 through the client log).
    local down_calls durable_down durable_bad accepted_down accepted_bad down_summary
    down_calls=$(events "$RUN_DIR/writerA.out" | awk -v kill="$KILL_NS" -v rec="$reconnect_ns" -v p="$K2_PARITY" \
        '$6 > kill + 1e9 && $6 < rec && !($2 == "CL_SUCCESS" && $7 != p)')
    durable_down=$(echo "$down_calls" | awk '$1 == "durable"' | wc -l)
    durable_bad=$(echo "$down_calls" | awk -v restart="$RESTART_NS" \
        '$1 == "durable" && $2 != "CL_ERR_DURABILITY_UNKNOWN" && !($2 == "CL_SUCCESS" && $6 > restart)' | wc -l)
    accepted_down=$(echo "$down_calls" | awk -v restart="$RESTART_NS" '$1 == "accepted" && $6 < restart' | wc -l)
    accepted_bad=$(echo "$down_calls" | awk -v restart="$RESTART_NS" \
        '$1 == "accepted" && $6 < restart && $2 == "CL_SUCCESS"' | wc -l)
    down_summary=$(echo "$down_calls" | awk -v restart="$RESTART_NS" \
        'NF { c[$1 " " $2 " " (($6 > restart) ? "after" : "before")]++ } END { for(k in c) printf "%s=%d, ", k, c[k] }')
    if [ "$durable_down" -gt 0 ] && [ "$durable_bad" -eq 0 ] && [ "$accepted_down" -gt 0 ] && [ "$accepted_bad" -eq 0 ]; then
        ok "$PROTO: codes: writer A's $durable_down durable call(s) to keeper 2 between the kill and the reconnect returned CL_ERR_DURABILITY_UNKNOWN or, after the restart, CL_SUCCESS, and its $accepted_down accepted call(s) to keeper 2 before the restart failed (mode code before/after the restart: ${down_summary%, })"
    else
        bad "$PROTO: codes: of writer A's $durable_down durable call(s) to keeper 2 between the kill and the reconnect, $durable_bad returned neither CL_ERR_DURABILITY_UNKNOWN nor a post-restart CL_SUCCESS; $accepted_bad of its $accepted_down accepted call(s) to keeper 2 before the restart succeeded (mode code before/after the restart: ${down_summary%, })"
    fi

    # ------------------------------------------------------ hot replay ----
    local last_ns
    last_ns=$(events "$RUN_DIR/writerA.out" | awk '$2 == "CL_SUCCESS" { print $3 }' | sort -n | tail -1)
    while [ $((($(now_ns) - last_ns) / 1000000000)) -lt $HOT_WAIT_SECS ]; do sleep 0.5; done
    local hot_ns
    hot_ns=$(now_ns)
    replay_story "$STORY_A" hot_a
    local a_status="$R_STATUS"
    archive_ids "$STORY_A" | sort -u > "$RUN_DIR/archive_at_hot_a.ids"
    replay_story "$STORY_B" hot_b
    local b_status="$R_STATUS"
    events "$RUN_DIR/writerA.out" | awk -v restart="$RESTART_NS" -v p="${K2_PARITY:--1}" -v t="$hot_ns" \
        -v from="$HOT_FROM_SECS" -v to="$SEALED_AFTER_SECS" \
        '$2 == "CL_SUCCESS" && $6 > restart && $7 == p && $3 > t - from * 1e9 && $3 < t - to * 1e9 { print $3, $4, $5 }' |
        sort -u > "$RUN_DIR/hot_k2.ids"
    local hot_count hot_missing hot_archived
    hot_count=$(wc -l < "$RUN_DIR/hot_k2.ids")
    hot_missing=$(missing "$RUN_DIR/hot_k2.ids" "$RUN_DIR/replay_hot_a.ids")
    hot_archived=$(comm -12 "$RUN_DIR/hot_k2.ids" "$RUN_DIR/archive_at_hot_a.ids" | wc -l)
    if [ "$a_status" != "CL_SUCCESS" ]; then
        bad "$PROTO: hot replay of $STORY_A: status ${a_status:-none}"
    elif [ "$hot_count" -eq 0 ]; then
        bad "$PROTO: hot replay of $STORY_A: no acked event of keeper 2 between ${HOT_FROM_SECS}s and ${SEALED_AFTER_SECS}s before the replay"
    elif [ "$hot_archived" -ne 0 ]; then
        bad "$PROTO: hot replay of $STORY_A: $hot_archived of the $hot_count events of keeper 2 were already in the archive; the replay did not test the hot fetch"
    elif [ "$hot_missing" -eq 0 ]; then
        ok "$PROTO: hot replay of $STORY_A: all $hot_count acked post-restart events of keeper 2 that were sealed but not archived came back"
    else
        bad "$PROTO: hot replay of $STORY_A: $hot_missing of $hot_count acked post-restart events of keeper 2 missing; the player did not reach the restarted keeper"
        grep -h 'KeeperHotFetchClient' "$MONITOR_DIR"/chrono-player-1*.log 2> /dev/null | tail -3 | sed 's/^/    /'
    fi
    events "$RUN_DIR/writerB.out" | awk -v t="$hot_ns" -v to="$SEALED_AFTER_SECS" \
        '$2 == "CL_SUCCESS" && $3 < t - to * 1e9 { print $3, $4, $5 }' | sort -u > "$RUN_DIR/hot_b.ids"
    local b_count b_missing
    b_count=$(wc -l < "$RUN_DIR/hot_b.ids")
    b_missing=$(missing "$RUN_DIR/hot_b.ids" "$RUN_DIR/replay_hot_b.ids")
    if [ "$b_status" = "CL_SUCCESS" ] && [ "$b_count" -gt 0 ] && [ "$b_missing" -eq 0 ]; then
        ok "$PROTO: replay of $STORY_B: all $b_count acked events sealed by the replay came back"
    else
        bad "$PROTO: replay of $STORY_B: status ${b_status:-none}, $b_missing of $b_count acked sealed events missing"
    fi

    # --------------------------------------------------------- archive ----
    touch "$RUN_DIR/release"
    # a writer stuck in ReleaseStory or Disconnect would keep its query port
    # into the next protocol's run, so it is killed here
    if wait_for_line "$RUN_DIR/writerA.out" '^RELEASED' 30 && wait_for_line "$RUN_DIR/writerB.out" '^RELEASED' 30 &&
       writers_exited 30; then
        WRITER_PIDS=""
    else
        bad "$PROTO: a writer did not release its story and exit"
        stop_writers
    fi

    # post-restart events of each story and mode; keeper 2's durable events from
    # before the kill; and the rest except keeper 2's accepted events from
    # before the kill, which the kill may have lost
    local s m
    for s in A B; do
        for m in accepted durable; do
            acked_ids "$RUN_DIR/writer$s.out" "\$6 > restart && \$1 == \"$m\"" > "$RUN_DIR/expect_post_${s}_$m.ids"
        done
    done
    acked_ids "$RUN_DIR/writerA.out" '$6 < kill && $7 == p && $1 == "durable"' > "$RUN_DIR/expect_recovered.ids"
    {
        acked_ids "$RUN_DIR/writerA.out" '$6 <= restart && !($6 < kill + 1e9 && $7 == p && $1 == "accepted")'
        acked_ids "$RUN_DIR/writerB.out" '$6 <= restart'
    } | sort -u > "$RUN_DIR/expect_rest.ids"
    cat "$RUN_DIR"/expect_*.ids | sort -u > "$RUN_DIR/expect_all.ids"
    say "waiting up to ${ARCHIVE_DEADLINE}s for the $(wc -l < "$RUN_DIR/expect_all.ids") expected events in the archive"
    deadline=$((SECONDS + ARCHIVE_DEADLINE))
    while :; do
        { archive_ids "$STORY_A"; archive_ids "$STORY_B"; } | sort > "$RUN_DIR/archive_all.copies"
        sort -u "$RUN_DIR/archive_all.copies" > "$RUN_DIR/archive_all.ids"
        [ "$(missing "$RUN_DIR/expect_all.ids" "$RUN_DIR/archive_all.ids")" -eq 0 ] && break
        [ $SECONDS -ge $deadline ] && break
        sleep 10
    done
    local post_summary="" post_missing=0 post_empty=0 n c
    for s in A B; do
        for m in accepted durable; do
            n=$(wc -l < "$RUN_DIR/expect_post_${s}_$m.ids")
            c=$(missing "$RUN_DIR/expect_post_${s}_$m.ids" "$RUN_DIR/archive_all.ids")
            post_summary="$post_summary $s/$m $((n - c))/$n"
            post_missing=$((post_missing + c))
            [ "$n" -gt 0 ] || post_empty=1
        done
    done
    local k2_post
    k2_post=$(acked_ids "$RUN_DIR/writerA.out" '$6 > restart && $7 == p' | comm -12 - "$RUN_DIR/archive_all.ids" | wc -l)
    if [ $post_missing -eq 0 ] && [ $post_empty -eq 0 ] && [ "$k2_post" -gt 0 ]; then
        ok "$PROTO: archive: every acked post-restart event is archived (story/mode archived/acked:$post_summary; $k2_post on keeper 2)"
    else
        bad "$PROTO: archive: post-restart events missing (story/mode archived/acked:$post_summary; $k2_post on keeper 2)"
    fi
    # Informational: the keepers the visor gave writer B, from the client's
    # "Response from AcquireStory" line (keepers, then the player), and B's
    # post-restart events at keeper 2's parity, which reach keeper 2 only if
    # B's keeper list holds it. The writers have exited, so the log is flushed.
    local b_keepers b_k2_acked b_k2_archived
    b_keepers=$(cat "$RUN_DIR"/writerB.client.log* 2> /dev/null | grep 'Response from AcquireStory' | tail -1 |
        sed 's/.*{story_id:[0-9]*}//' | grep -o 'ServiceId{[^}]*}' | sed '$d' |
        sed -n 's/ServiceId{[^:]*:[^:]*:\([0-9]*\):.*/\1/p' | paste -sd ' ')
    b_k2_acked=$(acked_ids "$RUN_DIR/writerB.out" '$6 > restart && $7 == p' | tee "$RUN_DIR/b_post_k2.ids" | wc -l)
    b_k2_archived=$(comm -12 "$RUN_DIR/b_post_k2.ids" "$RUN_DIR/archive_all.ids" | wc -l)
    say "$PROTO: info: $STORY_B got $(echo $b_keepers | wc -w) keeper(s) at acquire (ports: ${b_keepers:-none}; keeper 2 is port $k2_port); $b_k2_acked of B's post-restart events at keeper 2's parity succeeded, $b_k2_archived of them archived"
    n=$(wc -l < "$RUN_DIR/expect_recovered.ids")
    c=$(missing "$RUN_DIR/expect_recovered.ids" "$RUN_DIR/archive_all.ids")
    if [ "$n" -gt 0 ] && [ "$c" -eq 0 ]; then
        ok "$PROTO: archive: all $n durable events keeper 2 acked before the kill are archived"
    else
        bad "$PROTO: archive: $c of $n durable events keeper 2 acked before the kill missing"
    fi
    n=$(wc -l < "$RUN_DIR/expect_rest.ids")
    c=$(missing "$RUN_DIR/expect_rest.ids" "$RUN_DIR/archive_all.ids")
    if [ "$c" -eq 0 ]; then
        ok "$PROTO: archive: all $n other acked events up to the restart are archived (keeper 2's accepted events from before the kill not counted)"
    else
        bad "$PROTO: archive: $c of $n other acked events up to the restart missing"
    fi
    acked_ids "$RUN_DIR/writerA.out" '$6 < kill && $7 == p && $1 == "accepted"' > "$RUN_DIR/k2_accepted_before_kill.ids"
    say "archive: $(wc -l < "$RUN_DIR/archive_all.copies") stored copies of $(wc -l < "$RUN_DIR/archive_all.ids") events; $(missing "$RUN_DIR/k2_accepted_before_kill.ids" "$RUN_DIR/archive_all.ids") of the $(wc -l < "$RUN_DIR/k2_accepted_before_kill.ids") accepted events keeper 2 acked before the kill were lost"

    # ------------------------------------------------------------ stop ----
    say "stopping the deployment"
    if "$DEPLOY" -s -w "$WORK_DIR" > "$RUN_DIR/stop.out" 2>&1; then
        ok "$PROTO: stop: both keepers left an exit-summary with no unconfirmed durable events"
    else
        bad "$PROTO: stop: deploy_local.sh --stop reported a missing exit-summary or unconfirmed durable events"
        grep -A3 'PENDING_EVENTS' "$RUN_DIR/stop.out"
    fi
    kill_daemons

    # ------------------------------------------------------------ logs ----
    # read after the stop: the keeper and visor flush INFO lines at exit
    local started_a started_b held
    started_a=$(cat "$MONITOR_DIR"/chrono-keeper-2*.log 2> /dev/null |
        grep -c "\[DataStoreAdminService\] Starting Story Recording: StoryName=$STORY_A,")
    started_b=$(cat "$MONITOR_DIR"/chrono-keeper-2*.log 2> /dev/null |
        grep -c "\[DataStoreAdminService\] Starting Story Recording: StoryName=$STORY_B,")
    held=$(grep -h 'Started [0-9]* of [0-9]* held stories on keeper' "$MONITOR_DIR"/chrono-visor*.log 2> /dev/null |
        sed -n 's/.*Started \([0-9]*\) of \([0-9]*\) held stories.*/\1 \2/p' | tail -1)
    if [ "$started_a" -ge 1 ] && [ "$started_b" -ge 1 ] && [ "${held:-}" = "2 2" ]; then
        ok "$PROTO: logs: the visor started both held stories on the restarted keeper, and its log shows start_story_recording for $STORY_A and $STORY_B"
    else
        bad "$PROTO: logs: restarted keeper 2 got start_story_recording $started_a time(s) for $STORY_A and $started_b for $STORY_B; visor started/held: ${held:-none}"
    fi
    grep -h 'Durable write starts a pipeline' "$MONITOR_DIR"/chrono-keeper-2*.log 2> /dev/null | sed 's/^/    /'

    # keep the logs of a failed run next to its files
    if [ $RUN_FAIL -gt 0 ]; then
        command cp -f "$MONITOR_DIR"/chrono-*.log "$RUN_DIR/" 2> /dev/null
    fi
    say "$PROTO: $RUN_PASS passed, $RUN_FAIL failed"
}

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -x "$H5DUMP" ] || { say "h5dump not found at $H5DUMP"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }
# a backup left by an interrupted run holds the real template, and the template
# may still be that run's patched copy
if [ -e "$CONF_TEMPLATE.restart_held_backup" ]; then
    say "$CONF_TEMPLATE.restart_held_backup exists, left by an interrupted run; restore the template from it" \
        "(mv -f $CONF_TEMPLATE.restart_held_backup $CONF_TEMPLATE) or remove it, then run again"
    exit 2
fi

TEMPLATE_PROTO=$(jq -r '.chrono_keeper.KeeperRecordingService.rpc.protocol_conf' "$CONF_TEMPLATE")
if [ -z "${PROTOCOLS:-}" ]; then
    PROTOCOLS="$TEMPLATE_PROTO"
    [ "$TEMPLATE_PROTO" = "ofi+tcp" ] || PROTOCOLS="$PROTOCOLS ofi+tcp"
fi

command cp -f "$CONF_TEMPLATE" "$CONF_TEMPLATE.restart_held_backup"
RUN_ROOT=$(mktemp -d /tmp/keeper_restart_held.XXXXXX)
restore_conf() { mv -f "$CONF_TEMPLATE.restart_held_backup" "$CONF_TEMPLATE"; }
finish() {
    cleanup
    restore_conf
    if [ $FAIL -eq 0 ]; then rm -rf "$RUN_ROOT"; else say "files of the failed run kept in $RUN_ROOT"; fi
}
trap finish EXIT

for proto in $PROTOCOLS; do
    run_scenario "$proto"
    cleanup
    sleep 2
done

# ------------------------------------------------------------------ report ----
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
