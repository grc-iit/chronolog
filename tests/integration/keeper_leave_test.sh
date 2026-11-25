#!/bin/bash
# Integration test: stories still start, stop and go away after a keeper of
# the recording group has left (#707, #743).
#
# The visor holds one DataStoreAdminClient per keeper, grapher and player, all
# sending the same RPC names from one engine. When a keeper unregisters, the
# visor deletes its admin client a few seconds later (delayed_data_admin_exit_in_secs),
# from the next getActiveKeepers() call, which an acquire makes. Before #740
# its destructor deregistered the shared RPC IDs, so from that acquire on the
# visor could not start a story on the keeper that stayed nor on the grapher,
# and every acquire returned CL_ERR_NO_KEEPERS. Since #750 the admin clients
# send through the visor's admin runtime, which owns the procedures.
#
# Two keepers in one recording group; keeper 2 is stopped with SIGTERM, so it
# unregisters. Then, through chrono-client-cli, against keeper 1:
#
#   before   : with both keepers up, a story is acquired
#   left     : keeper 2 exited
#   acquire  : a new story is acquired after keeper 2 left (its admin client is deleted here)
#   write    : an event is written to it
#   release  : it is released
#   destroy  : it and its chronicle are destroyed, and the chronicle is no longer listed
#   again    : a second new story is acquired and destroyed
#   visor    : the visor logged no refused start, no unconfirmed stop and no
#              failed admin client
#
# Requires an installed tree (deploy_local.sh layout). Ports are the template's,
# so it refuses to start while any ChronoLog daemon runs on this host.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog ./keeper_leave_test.sh

set -u

WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"

MONITOR_DIR="$WORK_DIR/monitor"
CLIENT_CONF="$WORK_DIR/conf/default-chrono-client-conf.json"
DEPLOY="$WORK_DIR/tools/deploy/deploy_local.sh"
CLI="$WORK_DIR/tools/cli/chrono-client-cli"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/keeper_leave_test.XXXXXX")"

# deploy_local.sh returns before the keepers register with the visor (#742)
REGISTER_WAIT_SECS=10
# how long keeper 2 may take to exit after SIGTERM
EXIT_WAIT_SECS=60
# past delayed_data_admin_exit_in_secs (3 s in the template)
DELAYED_EXIT_WAIT_SECS=10

PASS=0
FAIL=0

say() { echo -e "[keeper_leave_test] $*"; }
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
    if [ "$FAIL" -eq 0 ]; then
        rm -rf -- "$OUT"
    else
        say "files of the failed run kept in $OUT"
    fi
}

# the CLI's answers to the commands, without its help text
answers() { grep -E "successfully|Failed|return code|Error" "$1" | grep -v "^\[ClientCLI\]" | tr '\n' ' '; }

# runs chrono-client-cli with one command per argument, prints its output
cli() {
    local commands=("$@" "-disconnect")
    printf '%s\n' "${commands[@]}" | (cd "$WORK_DIR" && timeout 120 "$CLI" -c "$CLIENT_CONF") 2>&1
}

keeper2_pids() { pgrep -f "$WORK_DIR/bin/chrono-keeper --config .*chrono-keeper-conf-2\.json"; }

# ---------------------------------------------------------------- setup ----
[ -x "$CLI" ] || { say "chrono-client-cli not found at $CLI"; exit 2; }
if pgrep -f "chrono-(visor|keeper|grapher|player) --config" > /dev/null; then
    say "ChronoLog daemons are running on this host; this test needs the template's ports to itself"
    pgrep -af "chrono-(visor|keeper|grapher|player) --config" | sed 's/^/    /'
    exit 2
fi
trap cleanup EXIT

rm -f "$MONITOR_DIR"/chrono-*.log
say "deploying 2 keepers / 1 recording group"
"$DEPLOY" -d -w "$WORK_DIR" -k 2 -r 1 > "$OUT/deploy.log" 2>&1 || { say "deploy failed"; exit 2; }
sleep "$REGISTER_WAIT_SECS"
if [ "$(pgrep -c -f "$WORK_DIR/bin/chrono-keeper --config")" -ne 2 ]; then
    say "expected 2 keepers up"
    exit 2
fi

# ---------------------------------------------------------------- tests ----
cli "-c leaveA" "-a -s leaveA s1" "-q -s leaveA s1" "-d -s leaveA s1" "-d -c leaveA" > "$OUT/before.out"
if grep -q "Story acquired successfully: s1 in Chronicle leaveA" "$OUT/before.out"; then
    ok "before: with both keepers up, a story is acquired"
else
    bad "before: the acquire with both keepers up failed: $(answers "$OUT/before.out")"
fi

say "stopping keeper 2 with SIGTERM"
pkill -TERM -f "$WORK_DIR/bin/chrono-keeper --config .*chrono-keeper-conf-2\.json"
for _ in $(seq "$EXIT_WAIT_SECS"); do
    [ -z "$(keeper2_pids)" ] && break
    sleep 1
done
if [ -z "$(keeper2_pids)" ]; then
    ok "left: keeper 2 exited"
else
    bad "left: keeper 2 still runs ${EXIT_WAIT_SECS}s after SIGTERM"
fi
sleep "$DELAYED_EXIT_WAIT_SECS"

cli "-c leaveB" "-a -s leaveB s1" "-w after-keeper-2-left" "-q -s leaveB s1" "-d -s leaveB s1" "-d -c leaveB" \
    "-l -c" > "$OUT/after.out"
if grep -q "Story acquired successfully: s1 in Chronicle leaveB" "$OUT/after.out"; then
    ok "acquire: a new story is acquired after keeper 2 left"
else
    bad "acquire: $(answers "$OUT/after.out")"
fi
if grep -q "Event written successfully" "$OUT/after.out"; then
    ok "write: an event is written to it"
else
    bad "write: no event was written"
fi
if grep -q "Story released successfully: s1 in Chronicle leaveB" "$OUT/after.out"; then
    ok "release: it is released"
else
    bad "release: $(answers "$OUT/after.out")"
fi
if grep -q "Story destroyed successfully: s1 in Chronicle leaveB" "$OUT/after.out" &&
    grep -q "Chronicle destroyed successfully: leaveB" "$OUT/after.out" &&
    ! sed -n '/^Chronicles (/,$p' "$OUT/after.out" | grep -q "^  leaveB$"; then
    ok "destroy: the story and its chronicle are destroyed and no longer listed"
else
    bad "destroy: $(answers "$OUT/after.out")"
fi

cli "-c leaveC" "-a -s leaveC s1" "-q -s leaveC s1" "-d -s leaveC s1" "-d -c leaveC" > "$OUT/again.out"
if grep -q "Story acquired successfully: s1 in Chronicle leaveC" "$OUT/again.out" &&
    grep -q "Chronicle destroyed successfully: leaveC" "$OUT/again.out"; then
    ok "again: a second new story is acquired and destroyed"
else
    bad "again: $(answers "$OUT/again.out")"
fi

# the visor flushes at warning, so its warning and error lines are on disk
"$DEPLOY" -s -w "$WORK_DIR" > /dev/null 2>&1
cp "$MONITOR_DIR"/chrono-*.log "$OUT"/ 2> /dev/null
# what the visor logs when a start or stop to a keeper or grapher fails
FAILED_ADMIN='Refused [0-9]+ story start|not confirmed by every component|failed to create DataStoreAdminClient'
failed_notices=$(grep -h -c -E "$FAILED_ADMIN" "$MONITOR_DIR"/chrono-visor*.log 2> /dev/null |
    awk '{ s += $1 } END { print s + 0 }')
if [ "$failed_notices" -eq 0 ]; then
    ok "visor: no refused start, unconfirmed stop or failed admin client"
else
    bad "visor: $failed_notices such line(s), e.g. $(grep -h -m1 -E "$FAILED_ADMIN" "$MONITOR_DIR"/chrono-visor*.log)"
fi

say "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
