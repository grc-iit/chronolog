#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:?repository root required}
script="$repo_root/tools/deploy/deploy_local.sh"
fixture=$(mktemp -d /tmp/chronolog-deploy-wal-test.XXXXXX)
trap 'rm -rf -- "$fixture"' EXIT
failures=0

check() {
    local message=$1
    shift
    if "$@"; then
        echo "PASS: $message"
    else
        echo "FAIL: $message" >&2
        failures=$((failures + 1))
    fi
}

# Load the production functions without starting daemons.
for fn in generate_config_files read_exit_summary report_wal_exit_summaries clean_wal_dirs restart_keeper parse_args; do
    source <(sed -n "/^${fn}() {/,/^}/p" "$script")
done
ERR='' INFO='' DEBUG='' NC=''
usage() { echo "usage called" >&2; return 1; }
quiet() { "$@" > /dev/null; }

reset_args() {
    start=false stop=false clean=false restart=false force=false
    EXEC_MODE_COUNT=0 RESTART_KEEPER_INDEX="" WAL_ENABLED=0
}

# Conf patch -----------------------------------------------------------------------------------------
template="$fixture/default-chrono-conf.json"
client_template="$fixture/default-chrono-client-conf.json"
cp "$repo_root/conf/default_conf.json.in" "$template"
cp "$repo_root/conf/default_client_conf.json.in" "$client_template"

on_conf="$fixture/conf-on"
mkdir -p "$on_conf"
generate_config_files 2 "$template" "$on_conf" "$fixture/output" 1 "$fixture/monitor" "$client_template" \
    "$fixture/work/wal" > /dev/null
for n in 1 2; do
    conf="$on_conf/chrono-keeper-conf-$n.json"
    check "WAL on: keeper $n has WAL.enabled true" \
        test "$(jq -r '.chrono_keeper.WAL.enabled' "$conf")" = true
    check "WAL on: keeper $n has its own absolute WAL.dir" \
        test "$(jq -r '.chrono_keeper.WAL.dir' "$conf")" = "$fixture/work/wal/keeper-$n"
    check "WAL on: keeper $n keeps the template's segment_bytes" \
        test "$(jq -r '.chrono_keeper.WAL.segment_bytes' "$conf")" = 67108864
done
check "WAL on: grapher conf has no WAL change" \
    test "$(jq -c '.chrono_keeper.WAL' "$on_conf/chrono-grapher-conf-1.json")" = "$(jq -c '.chrono_keeper.WAL' "$template")"

off_conf="$fixture/conf-off"
mkdir -p "$off_conf"
generate_config_files 1 "$template" "$off_conf" "$fixture/output" 1 "$fixture/monitor" "$client_template" "" > /dev/null
check "WAL off: keeper keeps the template's WAL block" \
    test "$(jq -c '.chrono_keeper.WAL' "$off_conf/chrono-keeper-conf-1.json")" = "$(jq -c '.chrono_keeper.WAL' "$template")"

# Argument parsing -----------------------------------------------------------------------------------
reset_args
parse_args --start --wal
check "--wal turns the WAL on" test "$WAL_ENABLED" = 1
reset_args
parse_args --restart-keeper 2
check "--restart-keeper selects the restart mode" test "$restart" = true
check "--restart-keeper keeps the keeper index" test "$RESTART_KEEPER_INDEX" = 2
reset_args
parse_args --clean --force
check "--force is accepted with --clean" test "$force" = true
reset_args
# usage() exits in the script; a subshell keeps that exit from ending the test.
refused() { ! ( usage() { exit 1; }; reset_args; parse_args "$@" ) > /dev/null 2>&1; }
check "--restart-keeper refuses a non-number" refused --restart-keeper x
check "--restart-keeper refuses keeper 0" refused --restart-keeper 0
check "--restart-keeper refuses another mode with it" refused --stop --restart-keeper 1

# Stop summary ---------------------------------------------------------------------------------------
stop_case() {
    local name=$1
    shift
    local conf_dir="$fixture/stop-$name/conf"
    local wal_root="$fixture/stop-$name/wal"
    mkdir -p "$conf_dir"
    generate_config_files "$#" "$template" "$conf_dir" "$fixture/output" 1 "$fixture/monitor" "$client_template" \
        "$wal_root" > /dev/null
    local n=1
    for summary in "$@"; do
        mkdir -p "$wal_root/keeper-$n"
        if [[ "$summary" != missing ]]; then
            printf '%s\n' "$summary" > "$wal_root/keeper-$n/exit-summary"
        fi
        n=$((n + 1))
    done
    echo "$conf_dir"
}

conf_dir=$(stop_case zero 'pending_events=0 pending_bytes=0' 'pending_events=0 pending_bytes=0')
if report_wal_exit_summaries "$conf_dir" > "$fixture/zero.out"; then rc=0; else rc=$?; fi
check "stop: every summary zero exits 0" test "$rc" -eq 0
check "stop: the table names each keeper" test "$(grep -c 'keeper-[12]' "$fixture/zero.out")" -eq 2

conf_dir=$(stop_case pending 'pending_events=0 pending_bytes=0' 'pending_events=3 pending_bytes=4096')
if report_wal_exit_summaries "$conf_dir" > "$fixture/pending.out"; then rc=0; else rc=$?; fi
check "stop: a summary with pending events exits nonzero" test "$rc" -ne 0
check "stop: the table shows the pending count" grep -q 'keeper-2.*3.*4096' "$fixture/pending.out"

conf_dir=$(stop_case missing 'pending_events=0 pending_bytes=0' missing)
if report_wal_exit_summaries "$conf_dir" > "$fixture/missing.out"; then rc=0; else rc=$?; fi
check "stop: a missing summary exits nonzero" test "$rc" -ne 0
check "stop: the table marks the missing summary" grep -q 'keeper-2.*missing' "$fixture/missing.out"

# 2^64 wraps to 0 in bash arithmetic; it is still a pending count.
conf_dir=$(stop_case huge 'pending_events=0 pending_bytes=0' 'pending_events=18446744073709551616 pending_bytes=4096')
if report_wal_exit_summaries "$conf_dir" > "$fixture/huge.out"; then rc=0; else rc=$?; fi
check "stop: a 2^64 pending count exits nonzero" test "$rc" -ne 0
check "stop: the table marks a 2^64 count pending" grep -q 'keeper-2 .*18446744073709551616 .*pending' "$fixture/huge.out"

conf_dir=$(stop_case zeros 'pending_events=000 pending_bytes=0')
check "stop: an all-zero count is clean" quiet report_wal_exit_summaries "$conf_dir"

# A deploy with fewer keepers than the one before it in the same conf dir: the earlier keeper-2 conf
# is removed, so stop does not report a keeper that never ran as missing.
conf_dir="$fixture/stop-shrink/conf"
mkdir -p "$conf_dir" "$fixture/stop-shrink/wal/keeper-1"
generate_config_files 2 "$template" "$conf_dir" "$fixture/output" 2 "$fixture/monitor" "$client_template" \
    "$fixture/stop-shrink/wal" > /dev/null
check "first deploy: two keeper and two grapher confs" \
    test -f "$conf_dir/chrono-keeper-conf-2.json" -a -f "$conf_dir/chrono-grapher-conf-2.json"
generate_config_files 1 "$template" "$conf_dir" "$fixture/output" 1 "$fixture/monitor" "$client_template" \
    "$fixture/stop-shrink/wal" > /dev/null
printf 'pending_events=0 pending_bytes=0\n' > "$fixture/stop-shrink/wal/keeper-1/exit-summary"
check "redeploy with fewer keepers: the earlier keeper-2 conf is gone" \
    test ! -e "$conf_dir/chrono-keeper-conf-2.json"
check "redeploy with fewer keepers: keeper 1's conf is written" test -f "$conf_dir/chrono-keeper-conf-1.json"
check "redeploy with fewer keepers: stop finds only keeper 1 and exits 0" quiet report_wal_exit_summaries "$conf_dir"
check "redeploy with fewer groups: the earlier grapher-2 conf is gone" \
    test ! -e "$conf_dir/chrono-grapher-conf-2.json"

conf_dir="$fixture/stop-off/conf"
mkdir -p "$conf_dir"
generate_config_files 1 "$template" "$conf_dir" "$fixture/output" 1 "$fixture/monitor" "$client_template" "" > /dev/null
check "stop: WAL off has nothing to check and exits 0" quiet report_wal_exit_summaries "$conf_dir"

# Clean ----------------------------------------------------------------------------------------------
wal_root="$fixture/clean/wal"
mkdir -p "$wal_root/keeper-1" "$wal_root/keeper-2"
printf 'pending_events=0 pending_bytes=0\n' > "$wal_root/keeper-1/exit-summary"
touch "$wal_root/keeper-1/00000001.wal"
printf 'pending_events=5 pending_bytes=100\n' > "$wal_root/keeper-2/exit-summary"
touch "$wal_root/keeper-2/00000001.wal"
if clean_wal_dirs "$wal_root" false > /dev/null 2>&1; then rc=0; else rc=$?; fi
check "clean: pending events refuse the clean" test "$rc" -ne 0
check "clean: a refused clean removes no WAL directory" test -f "$wal_root/keeper-1/00000001.wal"
check "clean: the pending WAL is kept" test -f "$wal_root/keeper-2/00000001.wal"

printf 'pending_events=18446744073709551616 pending_bytes=100\n' > "$wal_root/keeper-2/exit-summary"
if clean_wal_dirs "$wal_root" false > /dev/null 2>&1; then rc=0; else rc=$?; fi
check "clean: a 2^64 pending count refuses the clean" test "$rc" -ne 0
check "clean: the WAL with a 2^64 pending count is kept" test -f "$wal_root/keeper-2/00000001.wal"

rm -f "$wal_root/keeper-2/exit-summary"
if clean_wal_dirs "$wal_root" false > /dev/null 2>&1; then rc=0; else rc=$?; fi
check "clean: a missing summary refuses the clean" test "$rc" -ne 0
check "clean: the WAL without a summary is kept" test -f "$wal_root/keeper-2/00000001.wal"

check "clean: --force removes it" quiet clean_wal_dirs "$wal_root" true
check "clean: --force removed every WAL directory" test ! -e "$wal_root"

mkdir -p "$wal_root/keeper-1"
printf 'pending_events=0 pending_bytes=0\n' > "$wal_root/keeper-1/exit-summary"
touch "$wal_root/keeper-1/00000001.wal"
check "clean: a zero summary is removed" quiet clean_wal_dirs "$wal_root" false
check "clean: the zero-summary WAL is gone" test ! -e "$wal_root/keeper-1"
check "clean: no WAL root is fine" quiet clean_wal_dirs "$fixture/clean/none" false

# Restart one keeper ---------------------------------------------------------------------------------
# The deployment lifecycle (deployment_lifecycle.py) relaunches the keeper and
# refuses while the recorded one still runs; test_deployment_lifecycle.py checks
# that. Here it is a stand-in that logs its arguments.
restart_conf="$fixture/restart/conf"
MONITOR_DIR="$fixture/restart/monitor"
mkdir -p "$restart_conf" "$MONITOR_DIR"
generate_config_files 2 "$template" "$restart_conf" "$fixture/output" 1 "$MONITOR_DIR" "$client_template" \
    "$fixture/restart/wal" > /dev/null
before=$(md5sum "$restart_conf"/*.json)
KEEPER_BIN="$fixture/restart/chrono-keeper"
CONF_DIR="$restart_conf"
lifecycle() { echo "$*" > "$fixture/restart/args"; }
restart_keeper 2 > /dev/null
check "restart: the lifecycle relaunches keeper 2 with its generated conf and launch log" \
    test "$(cat "$fixture/restart/args" 2>/dev/null)" = "restart-keeper --executable $KEEPER_BIN --config $restart_conf/chrono-keeper-conf-2.json --output $MONITOR_DIR/chrono-keeper-2.launch.log"
check "restart: no conf regenerated" test "$(md5sum "$restart_conf"/*.json)" = "$before"
rm -f "$fixture/restart/args"
if restart_keeper 3 > /dev/null 2>&1; then rc=0; else rc=$?; fi
check "restart: a keeper without a generated conf is refused" test "$rc" -ne 0
check "restart: nothing relaunched for a keeper without a conf" test ! -e "$fixture/restart/args"
lifecycle() { return 27; }
if restart_keeper 2 > /dev/null 2>&1; then rc=0; else rc=$?; fi
check "restart: a refused relaunch fails" test "$rc" -ne 0
unset -f lifecycle

# Given binaries -------------------------------------------------------------------------------------
# Runs the whole script on a work dir whose bin/ holds the "installed" binaries,
# with -v/-g/-p/-a naming others: start, stop and --restart-keeper must hand the
# given ones to the lifecycle. A python3 first in PATH stands in for
# deployment_lifecycle.py: it appends its arguments to calls.log, runs the
# command a fenced start or clean wraps, and succeeds.
given_work="$fixture/given/work"
given_bin="$fixture/given/bin"
calls="$fixture/given/calls.log"
mkdir -p "$given_work/bin" "$given_work/lib" "$given_work/conf" "$given_bin" "$fixture/given/path"
cp "$template" "$given_work/conf/default-chrono-conf.json"
cp "$client_template" "$given_work/conf/default-chrono-client-conf.json"
for name in chrono-visor chrono-grapher chrono-player chrono-keeper; do
    for dir in "$given_work/bin" "$given_bin"; do
        printf '#!/bin/sh\nexit 0\n' > "$dir/$name"
        chmod +x "$dir/$name"
    done
done
cat > "$fixture/given/path/python3" << FAKE
#!/usr/bin/env bash
shift
echo "\$*" >> "$calls"
args=("\$@")
for i in "\${!args[@]}"; do
    if [[ "\${args[\$i]}" == run-start || "\${args[\$i]}" == run-clean ]]; then
        CHRONOLOG_DEPLOYMENT_GUARD=fake exec "\${args[@]:\$((i + 2))}"
    fi
done
exit 0
FAKE
chmod +x "$fixture/given/path/python3"
run_script() { PATH="$fixture/given/path:$PATH" bash "$script" "$@"; }
given_args=(-w "$given_work" -v "$given_bin/chrono-visor" -g "$given_bin/chrono-grapher"
    -p "$given_bin/chrono-keeper" -a "$given_bin/chrono-player")
called() { grep -qF -- "$1" "$calls" 2> /dev/null; }
launched() { # BIN CONF COMPONENT: start asked the lifecycle to launch BIN with CONF
    called "launch --executable $1 --config $2 --component $3 "
}

run_script -d "${given_args[@]}" > "$fixture/given/start.out" 2>&1 || true
check "given binaries: start launches the given keeper" \
    launched "$given_bin/chrono-keeper" "$given_work/conf/chrono-keeper-conf-1.json" keeper
check "given binaries: start launches the given visor" \
    launched "$given_bin/chrono-visor" "$given_work/conf/chrono-visor-conf.json" visor
check "given binaries: start launches the given grapher" \
    launched "$given_bin/chrono-grapher" "$given_work/conf/chrono-grapher-conf-1.json" grapher
check "given binaries: start launches the given player" \
    launched "$given_bin/chrono-player" "$given_work/conf/chrono-player-conf-1.json" player
check "given binaries: start fences on the given binaries" called "--occupied-executable $given_bin/chrono-keeper "
run_script -s "${given_args[@]}" > "$fixture/given/stop.out" 2>&1 || true
check "given binaries: stop runs the managed stop" called "--work-dir $given_work stop"
run_script --restart-keeper 1 "${given_args[@]}" > "$fixture/given/restart.out" 2>&1 || true
check "given binaries: --restart-keeper relaunches the given keeper" \
    called "restart-keeper --executable $given_bin/chrono-keeper --config $given_work/conf/chrono-keeper-conf-1.json "
check "given binaries: the installed binaries were never named" test "$(grep -c "$given_work/bin/" "$calls")" -eq 0

# Given confs with -w --------------------------------------------------------------------------------
# -f and -n name templates outside work_dir/conf, which holds none: start must
# generate the confs next to the -f template from both, and stop, restart and
# clean must read them there.
alt_work="$fixture/alt/work"
alt_conf="$fixture/alt/conf"
mkdir -p "$alt_work/bin" "$alt_work/lib" "$alt_work/conf" "$alt_conf" "$fixture/alt/client"
cp "$template" "$alt_conf/my-conf.json"
jq '.chrono_client.marker = "alt"' "$client_template" > "$fixture/alt/client/my-client.json"
alt_args=(-w "$alt_work" -f "$alt_conf/my-conf.json" -n "$fixture/alt/client/my-client.json"
    -v "$given_bin/chrono-visor" -g "$given_bin/chrono-grapher" -p "$given_bin/chrono-keeper" -a "$given_bin/chrono-player")
: > "$calls"
run_script -d --wal "${alt_args[@]}" > "$fixture/alt/start.out" 2>&1 || true
check "-f with -w: start launches the keeper with a conf generated next to the -f template" \
    launched "$given_bin/chrono-keeper" "$alt_conf/chrono-keeper-conf-1.json" keeper
check "-f with -w: start launches the visor with a conf generated next to the -f template" \
    launched "$given_bin/chrono-visor" "$alt_conf/chrono-visor-conf.json" visor
check "-n with -w: the client conf is generated from the -n template" \
    test "$(jq -r '.chrono_client.marker' "$alt_conf/chrono-client-conf.json" 2> /dev/null)" = alt
check "-f with -w: nothing generated in work_dir/conf" test -z "$(ls -A "$alt_work/conf")"
# The fake keeper writes no exit-summary, so stop reports keeper 1 as missing:
# only a stop that reads the -f conf directory finds that keeper's WAL.
run_script -s "${alt_args[@]}" > "$fixture/alt/stop.out" 2>&1 || true
check "-f with -w: stop runs the managed stop" called "--work-dir $alt_work stop"
check "-f with -w: stop reads the keeper WAL from the -f conf directory" grep -q 'keeper-1 .*missing' "$fixture/alt/stop.out"
run_script --restart-keeper 1 "${alt_args[@]}" > "$fixture/alt/restart.out" 2>&1 || true
check "-f with -w: --restart-keeper relaunches the keeper with the -f conf directory's conf" \
    called "restart-keeper --executable $given_bin/chrono-keeper --config $alt_conf/chrono-keeper-conf-1.json "
# clean is fenced by the lifecycle, so run its function with the fence left
# out. The confs start generated are there whether or not start honored -f.
for name in chrono-visor-conf.json chrono-keeper-conf-1.json chrono-client-conf.json; do
    [[ -f "$alt_conf/$name" ]] || echo '{}' > "$alt_conf/$name"
done
(
    for fn in clean check_work_dir refresh_paths_for_work_dir; do
        source <(sed -n "/^${fn}() {/,/^}/p" "$script")
    done
    fence_mutation() { :; }
    clean_output_dir() { :; }
    WORK_DIR_USER_SET=false OUTPUT_DIR_USER_SET=false MONITOR_DIR_USER_SET=false
    VISOR_BIN_USER_SET=false KEEPER_BIN_USER_SET=false GRAPHER_BIN_USER_SET=false PLAYER_BIN_USER_SET=false
    CONF_FILE_USER_SET=false CLIENT_CONF_FILE_USER_SET=false
    reset_args
    parse_args -c --force "${alt_args[@]}"
    clean
) > "$fixture/alt/clean.out" 2>&1 || true
check "-f with -w: clean removes the confs generated next to the -f template" test "$(ls -A "$alt_conf")" = my-conf.json
check "-f with -w: clean keeps the -n template" test -f "$fixture/alt/client/my-client.json"

echo "$failures deploy_local WAL checks failed"
test "$failures" -eq 0
