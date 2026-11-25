#!/bin/bash
# Integration test for a full restart of a deployment: every component is
# stopped with deploy_local.sh -s and started again, and the visor must come
# back with the chronicles and stories it had, from its SQLite catalog
# (catalog_db_path, which deploy_local.sh sets to work_dir/visor/catalog.sqlite).
#
# Two keepers in one recording group, the template's timings. The scenario
# writes CHRONICLE_A with STORY_1 and STORY_2 and CHRONICLE_B with the same two
# story names, EVENTS events to each, waits for every event to be in the
# archive, stops and starts everything, and reads the catalog back through a
# new client. Checks, each a numbered PASS/FAIL line in this order:
#
#   1. deploy_local.sh --clean, then a start: ShowChronicles lists nothing, so
#      no earlier run leaks in.
#   2. The generated visor conf names the absolute catalog path
#      work_dir/visor/catalog.sqlite, and the file exists.
#   3. Plan step 1: both chronicles are created and every event of the four
#      stories is acknowledged. The probe calls ReleaseStory but does not check
#      its return, so the check does not cover the release.
#   4. Plan step 2: every acknowledged event is in the archive (h5dump), which
#      is after the grapher's acceptance window. A story with fewer than EVENTS
#      acknowledged events counts the shortfall as missing, so an empty write
#      cannot pass.
#   5. Plan step 3: deploy_local.sh -s succeeds and no daemon is left.
#   6. After -s the catalog file is still there, with no -wal and no -shm
#      file. The visor holds an exclusive lock, so SQLite keeps its WAL index
#      in memory and makes no -shm; an orderly stop closes the database, which
#      checkpoints the WAL into the catalog file and removes the -wal. A -wal
#      left behind means the visor did not close the catalog.
#   7. The visor log has no warning that the catalog is on NFS, and the work
#      directory is not on NFS.
#   8. Plan step 4: every component starts again.
#   9. Plan step 5: ShowChronicles lists the two chronicles and ShowStories the
#      two stories of each, and nothing else.
#  10. Plan step 6: a new client acquires each old story without creating its
#      chronicle and replays it: every written event comes back once, and
#      nothing else.
#  11. Plan step 7: DestroyStory of CHRONICLE_A/STORY_1 succeeds, its archive
#      files are gone after the grapher's destroy (the story directory stays,
#      see HDF5FileChunkExtractor::delete_story_files), the manifest holds a
#      delete record for it, and ShowStories no longer lists it.
#  12. Plan step 8: CreateChronicle of an existing name gets
#      CL_ERR_CHRONICLE_EXISTS.
#  13. After another stop and start the destroyed story is still gone:
#      ShowStories lists only STORY_2 in CHRONICLE_A and both stories in
#      CHRONICLE_B.
#  14. A --clean refused by a keeper WAL directory with no exit-summary leaves
#      the catalog file and the archive in place.
#  15. The catalog file exists before a --clean, and the --clean removes it,
#      its -wal and -shm, and the visor directory; the next start lists no
#      chronicles.
#  16. Every visor this run started had the deploy's directory as its working
#      directory, and no visor/ directory appears there (a relative
#      catalog_db_path would put one there).
#
# Expected result: 16/16 in about 3 minutes. It needs BUILD_DIR, and H5DUMP
# when the source tree has no .spack-env.
#
# The deploys run from a new empty directory, the one check 16 looks at. The
# writes, replays and destroy go through chronolog-test-exact-split-probe and
# the listings and the create through chrono-client-cli, both from the build
# tree. The test does not patch the conf template.
#
# This test runs deploy_local.sh --clean on WORK_DIR, at the start and in
# checks 14 and 15: that removes the generated confs, the logs in
# work_dir/monitor, the archive in work_dir/output and the visor catalog. It
# refuses to start when work_dir/wal exists, since check 14 puts a WAL
# directory there and a real one would make the first --clean refuse, and when
# daemons of WORK_DIR are already running, since it would kill them. A failed
# run keeps its files and the daemon logs in the directory it prints.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        ./visor_reboot_test.sh

set -u
# the keeper WAL stays off: deploy_local.sh turns it on with WAL_ENABLED=1
unset WAL_ENABLED

# absolute, since deploy() runs deploy_local.sh from another directory
WORK_DIR="$(realpath -ms "${WORK_DIR:-$HOME/chronolog-install/chronolog}")"
BUILD_DIR="$(realpath -ms "${BUILD_DIR:-$HOME/chronolog-build/Debug}")"
REPO_ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../")"
H5DUMP="${H5DUMP:-$REPO_ROOT/.spack-env/view/bin/h5dump}"

MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_DIR="$WORK_DIR/conf"
CLIENT_CONF="$CONF_DIR/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
PROBE="$BUILD_DIR/tests/integration/watermark-replay/chronolog-test-exact-split-probe"
CLI="$BUILD_DIR/tools/cli/chrono-client-cli"
CATALOG="$WORK_DIR/visor/catalog.sqlite"

CHRONICLE_A=VisorRebootChronicleA
CHRONICLE_B=VisorRebootChronicleB
STORY_1=RebootStory1
STORY_2=RebootStory2
EVENTS=50

ARCHIVE_DEADLINE=360
DESTROY_DEADLINE=120
READY_DEADLINE=60

PASS=0
FAIL=0
CHECK=0

say() { echo -e "[visor_reboot_test $(date +%H:%M:%S)] $*"; }
ok() { CHECK=$((CHECK + 1)); say "$CHECK. PASS: $*"; PASS=$((PASS + 1)); }
bad() { CHECK=$((CHECK + 1)); say "$CHECK. FAIL: $*"; FAIL=$((FAIL + 1)); }

daemon_count() { pgrep -c -f "$WORK_DIR/bin/chrono-(visor|keeper|grapher|player) --config"; }

kill_daemons() {
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin --config" 2> /dev/null
    done
}

# runs deploy_local.sh from the scratch cwd; $1 is the label of its output file
deploy() {
    local label=$1
    shift
    (cd "$CWD_DIR" && "$DEPLOY" "$@" -w "$WORK_DIR") > "$RUN_DIR/deploy_$label.out" 2>&1
}

DAEMON_CONFS="chrono-visor-conf.json chrono-grapher-conf-1.json chrono-player-conf-1.json chrono-keeper-conf-1.json chrono-keeper-conf-2.json"

# the confs in DAEMON_CONFS whose daemon is not ready, with the reason: a daemon
# is ready when its <conf>.lifecycle.json says Running with status 0 and names a
# live process started with that conf. Each daemon writes Running once its
# services are up, a keeper only after the visor accepted its registration; a
# file left by an earlier run names a process that is gone or says Stopped.
not_ready() {
    local name conf pid state
    for name in $DAEMON_CONFS; do
        conf="$CONF_DIR/$name"
        state=$(jq -r '"\(.state) \(.status)"' "$conf.lifecycle.json" 2> /dev/null)
        pid=$(jq -r '.pid // empty' "$conf.lifecycle.json" 2> /dev/null)
        if [ "$state" != "Running 0" ]; then
            echo "$name: ${state:-no lifecycle file}"
        elif ! tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null | grep -qF -- "--config $conf"; then
            echo "$name: pid ${pid:-?} is not running with this conf"
        fi
    done
}

# start <label>: deploys 2 keepers / 1 recording group and waits until the five
# daemons are ready (not_ready prints nothing), at most READY_DEADLINE seconds
start_all() {
    deploy "$1" -d -k 2 -r 1 || { say "deploy failed: $(tail -3 "$RUN_DIR/deploy_$1.out")"; return 1; }
    local deadline=$((SECONDS + READY_DEADLINE)) waiting
    while waiting=$(not_ready) && [ -n "$waiting" ]; do
        if [ $SECONDS -ge $deadline ]; then
            say "start $1: not ready after ${READY_DEADLINE}s: $(paste -sd ';' <<< "$waiting"); see $RUN_DIR/deploy_$1.out and $MONITOR_DIR/chrono-*.log (copied to $RUN_DIR at exit)"
            return 1
        fi
        sleep 1
    done
    say "start $1: all five daemons Running after $((SECONDS - deadline + READY_DEADLINE))s"
    # the visor's working directory, where a relative catalog_db_path would go;
    # one per line, so a path with spaces stays whole
    VISOR_CWD=$(readlink "/proc/$(pgrep -f "$WORK_DIR/bin/chrono-visor --config" | head -1)/cwd" 2> /dev/null)
    VISOR_CWDS="$VISOR_CWDS${VISOR_CWD:-?}"$'\n'
}

stop_all() { deploy "stop_$1" -s; }

# cli <label> <command>...: runs the commands in chrono-client-cli, one per
# argument, then -disconnect; the output goes to $RUN_DIR/cli_<label>.out
cli() {
    local label=$1
    shift
    local started=$SECONDS rc
    printf '%s\n' "$@" -disconnect | timeout 120 "$CLI" -c "$RUN_DIR/client_conf.json" > "$RUN_DIR/cli_$label.out" 2>&1
    rc=$?
    # the CLI prints no status when its Connect fails, so the exit and the time
    # it took go at the end of its output
    echo "[visor_reboot_test] chrono-client-cli exit $rc after $((SECONDS - started))s, started $(date -d "-$((SECONDS - started)) sec" +%T)" >> "$RUN_DIR/cli_$label.out"
}

# the names a "-l -c" or "-l -s" listing in cli output $1 printed, sorted, one per line
listed() { awk '/^(Chronicles|Stories in Chronicle .*) \([0-9]+\):$/ { on = 1; next } on && /^  / { print $1; next } { on = 0 }' "$1" | sort; }

probe() { "$PROBE" --config "$RUN_DIR/client_conf.json" "$@"; }

# the event times in the archive of story $2 of chronicle $1, one per copy
archive_times() {
    local f
    for f in "$OUTPUT_DIR/$1/$2"/*.h5; do
        [ -f "$f" ] || continue
        "$H5DUMP" -d /story_chunks/data.vlen_bytes "$f" 2> /dev/null
    done | awk '/^ *\([0-9]+\): \{/ { n = 1; next }
                n >= 1 && n <= 2 { gsub(/[ ,]/, ""); v[n] = $0; n++
                                   if(n == 3) { print v[2]; n = 0 } }'
}

archive_files() { find "$OUTPUT_DIR/$1/$2" -maxdepth 1 -type f -name '*.h5' 2> /dev/null | wc -l; }

missing() { comm -23 "$1" "$2" | wc -l; } # lines of sorted $1 not in sorted $2

# not_archived <written> <archived>: the written events not in the archive; a
# written file with fewer than EVENTS lines adds its shortfall, so an empty
# one counts EVENTS missing instead of 0
not_archived() {
    local w
    w=$(wc -l < "$1")
    echo $(($(missing "$1" "$2") + (w < EVENTS ? EVENTS - w : 0)))
}

# the stories cli output $1 listed under "Stories in Chronicle $2 (n):", sorted
stories_listed() {
    awk -v h="Stories in Chronicle $2 (" 'index($0, h) == 1 && /\([0-9]+\):$/ { on = 1; next }
         on && /^  / { print $1; next } { on = 0 }' "$1" | sort
}

catalog_files() {
    local f s=""
    for f in "$CATALOG" "$CATALOG-wal" "$CATALOG-shm"; do
        [ -e "$f" ] && s="$s $(basename "$f")"
    done
    echo "${s# }"
}

STORIES="$CHRONICLE_A/$STORY_1 $CHRONICLE_A/$STORY_2 $CHRONICLE_B/$STORY_1 $CHRONICLE_B/$STORY_2"

# ---------------------------------------------------------------- setup ----
command -v jq > /dev/null || { say "jq not found"; exit 2; }
[ -x "$PROBE" ] || { say "probe not found at $PROBE"; exit 2; }
[ -x "$CLI" ] || { say "client CLI not found at $CLI"; exit 2; }
[ -x "$H5DUMP" ] || { say "h5dump not found at $H5DUMP (set H5DUMP)"; exit 2; }
[ -x "$DEPLOY" ] || { say "deploy script not found at $DEPLOY"; exit 2; }
if [ -e "$WORK_DIR/wal" ]; then
    say "$WORK_DIR/wal exists; this test runs --clean, which refuses or removes keeper WALs. Move it away and run again"
    exit 2
fi
# the test kills every daemon of WORK_DIR and cleans its files, so it does not
# start over a deployment that is running
running=$(daemon_count)
if [ "$running" -gt 0 ]; then
    say "$running daemon(s) of $WORK_DIR are running; this test would kill them and --clean their files. Stop them and run again"
    exit 2
fi
# every endpoint of the template is on loopback; the probe and the CLI bind
# their query service over ofi+tcp too
export FI_TCP_IFACE="${FI_TCP_IFACE:-lo}"

RUN_DIR=$(mktemp -d /tmp/visor_reboot.XXXXXX)
CWD_DIR="$RUN_DIR/deploy_cwd"
FAKE_WAL=""
VISOR_CWDS=""
mkdir -p "$CWD_DIR"
jq --arg log "$RUN_DIR/client.log" '.chrono_client.Monitoring.monitor.file = $log' "$CLIENT_CONF" \
    > "$RUN_DIR/client_conf.json" || { say "client conf patch failed"; exit 2; }

finish() {
    deploy final_stop -s
    kill_daemons
    if [ -n "$FAKE_WAL" ]; then
        rm -rf "$FAKE_WAL"
        rmdir -- "$WORK_DIR/wal" 2> /dev/null || true
    fi
    if [ $FAIL -eq 0 ]; then
        rm -rf "$RUN_DIR"
    else
        command cp -f "$MONITOR_DIR"/chrono-*.log "$RUN_DIR/" 2> /dev/null
        say "files of the failed run kept in $RUN_DIR"
    fi
}
trap finish EXIT

say "files in $RUN_DIR, deploys run from $CWD_DIR"
kill_daemons
sleep 1

# ------------------------------------------------ 1. clean, empty start ----
if deploy clean_first -c && start_all first; then
    cli empty_first "-l -c"
    if grep -q '^Chronicles (0):$' "$RUN_DIR/cli_empty_first.out"; then
        ok "--clean, then a start: ShowChronicles lists no chronicle"
    else
        bad "the start after --clean is not empty: $(grep -A5 '^Chronicles' "$RUN_DIR/cli_empty_first.out" | paste -sd ' ')"
    fi
else
    bad "--clean or the first start failed: $(tail -3 "$RUN_DIR"/deploy_clean_first.out "$RUN_DIR"/deploy_first.out 2> /dev/null)"
    exit 1
fi

# ----------------------------------------------- 2. the catalog path ----
# a -wal file next to the catalog while the visor runs shows SQLite granted
# journal_mode=WAL; the visor's exclusive lock keeps the WAL index in memory,
# so there is no -shm
conf_path=$(jq -r '.chrono_visor.catalog_db_path // empty' "$CONF_DIR/chrono-visor-conf.json")
files=$(catalog_files)
if [ "$conf_path" = "$(realpath -m "$CATALOG")" ] && [ -f "$CATALOG" ] && [ -e "$CATALOG-wal" ] && [ ! -e "$CATALOG-shm" ]; then
    ok "the visor conf names the absolute catalog path $conf_path; while the visor runs the files there are: $files"
else
    bad "the visor conf names catalog_db_path '${conf_path}' (expected $(realpath -m "$CATALOG")); files there while the visor runs: ${files:-none} (expected catalog.sqlite catalog.sqlite-wal)"
fi

# ---------------------------------------------- 3. create and write ----
cli create "-c $CHRONICLE_A" "-c $CHRONICLE_B"
created=$(grep -c '^Chronicle created successfully: ' "$RUN_DIR/cli_create.out")
write_bad=""
for cs in $STORIES; do
    out="$RUN_DIR/write_${cs//\//_}.out"
    probe write "${cs%/*}" "${cs#*/}" "$EVENTS" > "$out" 2>&1
    awk '$1 == "WROTE" { print $2 }' "$out" | sort -u > "$RUN_DIR/written_${cs//\//_}.times"
    n=$(wc -l < "$RUN_DIR/written_${cs//\//_}.times")
    if ! grep -q '^WRITE_STATUS CL_SUCCESS' "$out" || [ "$n" -ne "$EVENTS" ]; then
        write_bad="$write_bad $cs: $n acked, $(grep -c '^WRITE_FAILED' "$out") failed, $(grep STATUS "$out" | tail -1);"
    fi
done
if [ "$created" -eq 2 ] && [ -z "$write_bad" ]; then
    ok "plan step 1: created $CHRONICLE_A and $CHRONICLE_B, and all $EVENTS events of each of the four stories were acknowledged"
else
    bad "plan step 1: $created of 2 chronicles created ($(grep -E '^(Chronicle|Failed)' "$RUN_DIR/cli_create.out" | paste -sd ' '));$write_bad"
fi

# ------------------------------------------------------- 4. archive ----
say "waiting up to ${ARCHIVE_DEADLINE}s for the events in the archive"
deadline=$((SECONDS + ARCHIVE_DEADLINE))
while :; do
    archive_missing=0
    archive_summary=""
    for cs in $STORIES; do
        archive_times "${cs%/*}" "${cs#*/}" | sort -u > "$RUN_DIR/archive_${cs//\//_}.times"
        c=$(not_archived "$RUN_DIR/written_${cs//\//_}.times" "$RUN_DIR/archive_${cs//\//_}.times")
        archive_missing=$((archive_missing + c))
        archive_summary="$archive_summary $cs $((EVENTS - c))/$EVENTS"
    done
    [ $archive_missing -eq 0 ] && break
    [ $SECONDS -ge $deadline ] && break
    sleep 10
done
if [ $archive_missing -eq 0 ]; then
    ok "plan step 2: every written event is in the archive after $((SECONDS - deadline + ARCHIVE_DEADLINE))s (archived/written:$archive_summary)"
else
    bad "plan step 2: $archive_missing written events not in the archive after ${ARCHIVE_DEADLINE}s (archived/written:$archive_summary)"
fi

# ---------------------------------------------------------- 5. stop ----
if stop_all first && [ "$(daemon_count)" -eq 0 ]; then
    ok "plan step 3: deploy_local.sh -s stopped every component, and no daemon is left"
else
    bad "plan step 3: deploy_local.sh -s: $(tail -2 "$RUN_DIR/deploy_stop_first.out" | paste -sd ' '); $(daemon_count) daemon(s) left"
    kill_daemons
fi

# --------------------------------------------- 6. catalog after -s ----
# the visor closes the database at exit, and SQLite checkpoints the WAL into
# the catalog file and removes it; a -wal left behind means it was not closed
files=$(catalog_files)
if [ -s "$CATALOG" ] && [ ! -e "$CATALOG-wal" ] && [ ! -e "$CATALOG-shm" ]; then
    ok "after -s the catalog file remains ($(stat -c %s "$CATALOG") bytes), with no -wal (checkpointed when the visor closed it) and no -shm"
else
    bad "after -s: catalog files present: ${files:-none} (expected catalog.sqlite alone)"
fi

# ------------------------------------------------- 7. no NFS warning ----
fs_type=$(stat -f -c %T "$(dirname "$CATALOG")" 2> /dev/null)
nfs_lines=$(cat "$MONITOR_DIR"/chrono-visor*.log 2> /dev/null | grep -c 'is on nfs')
opened=$(cat "$MONITOR_DIR"/chrono-visor*.log 2> /dev/null | grep -c "Opened the catalog database $(realpath -m "$CATALOG") ")
if [ "$fs_type" != "nfs" ] && [ "$nfs_lines" -eq 0 ] && [ "$opened" -ge 1 ]; then
    ok "the catalog directory is on $fs_type; the visor log shows it opened the catalog and has no NFS warning"
else
    bad "the catalog directory is on ${fs_type:-?}; the visor log has $nfs_lines NFS warning line(s) and $opened line(s) saying it opened $CATALOG"
fi

# --------------------------------------------------------- 8. start ----
if start_all second; then
    ok "plan step 4: every component started again ($(daemon_count) daemons)"
else
    bad "plan step 4: the restart failed"
    exit 1
fi

# ---------------------------------------------------- 9. listings ----
cli list "-l -c" "-l -s $CHRONICLE_A" "-l -s $CHRONICLE_B"
listed "$RUN_DIR/cli_list.out" > "$RUN_DIR/listed.names"
printf '%s\n' "$CHRONICLE_A" "$CHRONICLE_B" "$STORY_1" "$STORY_1" "$STORY_2" "$STORY_2" | sort > "$RUN_DIR/expected.names"
if diff -q "$RUN_DIR/expected.names" "$RUN_DIR/listed.names" > /dev/null &&
   grep -q '^Chronicles (2):$' "$RUN_DIR/cli_list.out" &&
   [ "$(grep -c '^Stories in Chronicle .* (2):$' "$RUN_DIR/cli_list.out")" -eq 2 ]; then
    ok "plan step 5: ShowChronicles lists $CHRONICLE_A and $CHRONICLE_B, and ShowStories lists $STORY_1 and $STORY_2 in each"
else
    bad "plan step 5: listed: $(grep -E '^(Chronicles|Stories|  |Chronicle does|Failed)' "$RUN_DIR/cli_list.out" | paste -sd ' ')"
fi

# ------------------------------------------------------- 10. replay ----
replay_bad=""
replay_summary=""
for cs in $STORIES; do
    out="$RUN_DIR/replay_${cs//\//_}.out"
    probe replay "${cs%/*}" "${cs#*/}" > "$out" 2>&1
    awk '$1 == "EVENT" { print $2 }' "$out" | sort > "$RUN_DIR/replayed_${cs//\//_}.times"
    status=$(sed -n 's/^REPLAY_STATUS //p' "$out")
    got=$(wc -l < "$RUN_DIR/replayed_${cs//\//_}.times")
    m=$(missing "$RUN_DIR/written_${cs//\//_}.times" "$RUN_DIR/replayed_${cs//\//_}.times")
    extra=$(sort -u "$RUN_DIR/replayed_${cs//\//_}.times" | comm -13 "$RUN_DIR/written_${cs//\//_}.times" - | wc -l)
    replay_summary="$replay_summary $cs $got/$EVENTS"
    if [ "$status" != "CL_SUCCESS" ] || [ "$got" -ne "$EVENTS" ] || [ "$m" -ne 0 ] || [ "$extra" -ne 0 ]; then
        replay_bad="$replay_bad $cs: status ${status:-none}, $got returned, $m missing, $extra not written;"
    fi
done
if [ -z "$replay_bad" ]; then
    ok "plan step 6: a new client acquired each old story without creating its chronicle and replayed every written event once (replayed/written:$replay_summary)"
else
    bad "plan step 6:$replay_bad"
fi

# ------------------------------------------------------ 11. destroy ----
files_before=$(archive_files "$CHRONICLE_A" "$STORY_1")
probe destroy "$CHRONICLE_A" "$STORY_1" > "$RUN_DIR/destroy.out" 2>&1
d_status=$(sed -n 's/^DESTROY_STATUS //p' "$RUN_DIR/destroy.out")
deadline=$((SECONDS + DESTROY_DEADLINE))
while :; do
    files_after=$(archive_files "$CHRONICLE_A" "$STORY_1")
    manifest_delete=no
    jq -s --arg c "$CHRONICLE_A" --arg s "$STORY_1" -e \
        'any(.[]; .op == "delete" and .chronicle == $c and .story == $s)' \
        "$OUTPUT_DIR"/%manifest/*.log > /dev/null 2>&1 && manifest_delete=yes
    [ "$files_after" -eq 0 ] && [ "$manifest_delete" = yes ] && break
    [ $SECONDS -ge $deadline ] && break
    sleep 2
done
cli list_after_destroy "-l -s $CHRONICLE_A"
listed "$RUN_DIR/cli_list_after_destroy.out" > "$RUN_DIR/listed_after_destroy.names"
if [ "$d_status" = "CL_SUCCESS" ] && [ "$files_before" -gt 0 ] && [ "$files_after" -eq 0 ] &&
   [ "$manifest_delete" = yes ] && [ "$(cat "$RUN_DIR/listed_after_destroy.names")" = "$STORY_2" ]; then
    ok "plan step 7: DestroyStory $CHRONICLE_A/$STORY_1 returned CL_SUCCESS, its $files_before archive file(s) are gone, the manifest holds its delete record, and ShowStories lists only $STORY_2"
else
    bad "plan step 7: destroy status ${d_status:-none}; archive files $files_before before, $files_after after; manifest delete record: $manifest_delete; ShowStories $CHRONICLE_A: $(paste -sd ' ' "$RUN_DIR/listed_after_destroy.names")"
fi

# ----------------------------------------------- 12. create existing ----
cli create_existing "-c $CHRONICLE_B"
if grep -q "^Chronicle already exists: $CHRONICLE_B\$" "$RUN_DIR/cli_create_existing.out"; then
    ok "plan step 8: CreateChronicle $CHRONICLE_B got CL_ERR_CHRONICLE_EXISTS"
else
    bad "plan step 8: CreateChronicle $CHRONICLE_B: $(grep -E '^(Chronicle|Failed)' "$RUN_DIR/cli_create_existing.out" | paste -sd ' ') $(tail -1 "$RUN_DIR/cli_create_existing.out")"
fi

# ---------------------------------- 13. destroy survives a restart ----
if stop_all second && start_all third; then
    cli list_third "-l -c" "-l -s $CHRONICLE_A" "-l -s $CHRONICLE_B"
    third_a=$(stories_listed "$RUN_DIR/cli_list_third.out" "$CHRONICLE_A" | paste -sd ' ')
    third_b=$(stories_listed "$RUN_DIR/cli_list_third.out" "$CHRONICLE_B" | paste -sd ' ')
    if grep -q '^Chronicles (2):$' "$RUN_DIR/cli_list_third.out" && [ "$third_a" = "$STORY_2" ] &&
       [ "$third_b" = "$(printf '%s\n' "$STORY_1" "$STORY_2" | sort | paste -sd ' ')" ]; then
        ok "after another stop and start $CHRONICLE_A lists only $STORY_2 and $CHRONICLE_B both stories"
    else
        bad "after another stop and start: $(grep '^Chronicles' "$RUN_DIR/cli_list_third.out"); $CHRONICLE_A lists '${third_a}', $CHRONICLE_B lists '${third_b}'"
    fi
else
    bad "the second stop or the third start failed: $(tail -2 "$RUN_DIR"/deploy_stop_second.out "$RUN_DIR"/deploy_third.out 2> /dev/null | paste -sd ' ')"
fi
stop_all third || say "the third stop failed: $(tail -2 "$RUN_DIR/deploy_stop_third.out")"
kill_daemons

# --------------------------------------------- 14. a refused --clean ----
# deploy_local.sh --clean refuses a keeper WAL directory that has files and no
# exit-summary (the keeper crashed with events the grapher may not have
# confirmed) before it removes anything
FAKE_WAL="$WORK_DIR/wal/keeper-1"
mkdir -p "$FAKE_WAL" && echo "visor_reboot_test" > "$FAKE_WAL/0.wal"
archive_before=$(find "$OUTPUT_DIR" -type f -name '*.h5' | wc -l)
deploy clean_refused -c
refused_rc=$?
archive_after=$(find "$OUTPUT_DIR" -type f -name '*.h5' | wc -l)
files=$(catalog_files)
if [ $refused_rc -ne 0 ] && grep -q 'Refusing to remove keeper WAL directories' "$RUN_DIR/deploy_clean_refused.out" &&
   [ -s "$CATALOG" ] && [ "$archive_after" -eq "$archive_before" ] && [ "$archive_before" -gt 0 ]; then
    ok "--clean with a keeper WAL that has no exit-summary was refused (exit $refused_rc) and left the catalog ($files) and the $archive_after archive file(s)"
else
    bad "--clean with a keeper WAL that has no exit-summary: exit $refused_rc, catalog files ${files:-none}, archive files $archive_before before and $archive_after after"
fi
rm -rf "$FAKE_WAL"
rmdir -- "$WORK_DIR/wal" 2> /dev/null || true
FAKE_WAL=""

# ---------------------------------------------- 15. --clean, then empty ----
catalog_before=no
[ -s "$CATALOG" ] && catalog_before=yes
if deploy clean_last -c; then
    files=$(catalog_files)
    if [ "$catalog_before" = yes ] && [ -z "$files" ] && [ ! -e "$(dirname "$CATALOG")" ] && start_all last; then
        cli empty_last "-l -c"
        if grep -q '^Chronicles (0):$' "$RUN_DIR/cli_empty_last.out"; then
            ok "--clean removed the catalog files and the visor directory, and the next start lists no chronicle"
        else
            bad "after --clean the start lists: $(grep -A5 '^Chronicles' "$RUN_DIR/cli_empty_last.out" | paste -sd ' ')"
        fi
    else
        bad "catalog file before --clean: $catalog_before; after --clean: catalog files left: ${files:-none}; visor directory $([ -e "$(dirname "$CATALOG")" ] && echo kept || echo removed); or the start failed"
    fi
else
    bad "--clean failed: $(tail -3 "$RUN_DIR/deploy_clean_last.out" | paste -sd ' ')"
fi
stop_all last || say "the last stop failed: $(tail -2 "$RUN_DIR/deploy_stop_last.out")"
kill_daemons

# -------------------------------------------------- 16. the deploy cwd ----
# the lifecycle launches the visor in the deploy's cwd; an absent visor/ there
# shows nothing only when every visor this run started had that cwd
other_cwds=$(printf '%s' "$VISOR_CWDS" | grep -v '^$' | grep -vxF -- "$CWD_DIR" | paste -sd ' ')
if [ ! -e "$CWD_DIR/visor" ] && [ -n "$VISOR_CWDS" ] && [ -z "$other_cwds" ]; then
    ok "no visor/ directory appeared in $CWD_DIR, the working directory of every deploy and of every visor it started ($(find "$CWD_DIR" -mindepth 1 | wc -l) entries there)"
elif [ ! -e "$CWD_DIR/visor" ]; then
    bad "not every visor ran in $CWD_DIR, so the absence of visor/ there shows nothing; the visors ran in: $(printf '%s' "$VISOR_CWDS" | paste -sd ' ')"
else
    bad "a visor/ directory appeared in $CWD_DIR: $(find "$CWD_DIR/visor" | paste -sd ' ')"
fi

# ------------------------------------------------------------------ report ----
say "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
