#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:?repository root required}
script="$repo_root/tools/deploy/deploy_local.sh"
fixture=$(mktemp -d /tmp/chronolog-deploy-wal-test.XXXXXX)
trap 'pkill -f "$fixture/" || true; rm -rf -- "$fixture"' EXIT
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
restart_conf="$fixture/restart/conf"
MONITOR_DIR="$fixture/restart/monitor"
mkdir -p "$restart_conf" "$MONITOR_DIR"
generate_config_files 2 "$template" "$restart_conf" "$fixture/output" 1 "$MONITOR_DIR" "$client_template" \
    "$fixture/restart/wal" > /dev/null
before=$(md5sum "$restart_conf"/*.json)
KEEPER_BIN="$fixture/restart/fake-keeper"
CONF_DIR="$restart_conf"
printf '#!/bin/sh\necho "$@" > "%s/args"\n' "$fixture/restart" > "$KEEPER_BIN"
chmod +x "$KEEPER_BIN"
restart_keeper 2 > /dev/null
for _ in $(seq 50); do [[ -s "$fixture/restart/args" ]] && break; sleep 0.1; done
check "restart: keeper 2 runs with its generated conf" \
    test "$(cat "$fixture/restart/args" 2>/dev/null)" = "--config $restart_conf/chrono-keeper-conf-2.json"
check "restart: no conf regenerated" test "$(md5sum "$restart_conf"/*.json)" = "$before"
if restart_keeper 3 > /dev/null 2>&1; then rc=0; else rc=$?; fi
check "restart: a keeper without a generated conf is refused" test "$rc" -ne 0

# Given binaries -------------------------------------------------------------------------------------
# Runs the whole script on a work dir whose bin/ holds the "installed" binaries,
# with -v/-g/-p/-a naming others: start, stop and --restart-keeper must use the
# given ones. Every fake appends "<its path> <args>" to runs.log; the given
# keeper stays up until SIGTERM so stop has something to stop.
given_work="$fixture/given/work"
given_bin="$fixture/given/bin"
runs="$fixture/given/runs.log"
mkdir -p "$given_work/bin" "$given_work/lib" "$given_work/conf" "$given_bin"
cp "$template" "$given_work/conf/default-chrono-conf.json"
cp "$client_template" "$given_work/conf/default-chrono-client-conf.json"
for name in chrono-visor chrono-grapher chrono-player chrono-keeper; do
    for dir in "$given_work/bin" "$given_bin"; do
        printf '#!/bin/sh\necho "$0 $*" >> "%s"\n' "$runs" > "$dir/$name"
        chmod +x "$dir/$name"
    done
done
cat > "$given_bin/chrono-keeper" << EOF
#!/bin/sh
echo "\$0 \$*" >> "$runs"
trap 'echo "\$0 stopped" >> "$runs"; exit 0' TERM
while :; do sleep 0.1; done
EOF
given_args=(-w "$given_work" -v "$given_bin/chrono-visor" -g "$given_bin/chrono-grapher"
    -p "$given_bin/chrono-keeper" -a "$given_bin/chrono-player")
keeper_run="$given_bin/chrono-keeper --config $given_work/conf/chrono-keeper-conf-1.json"
runs_of() { grep -cxF "$1" "$runs" 2> /dev/null || true; }
ran() { test "$(runs_of "$1")" -ge 1; }
wait_runs_of() { for _ in $(seq 50); do [[ $(runs_of "$1") -ge $2 ]] && break; sleep 0.1; done; }

bash "$script" -d "${given_args[@]}" > "$fixture/given/start.out" 2>&1 || true
wait_runs_of "$keeper_run" 1
check "given binaries: start runs the given keeper" ran "$keeper_run"
check "given binaries: start runs the given visor" \
    ran "$given_bin/chrono-visor --config $given_work/conf/chrono-visor-conf.json"
check "given binaries: start runs the given grapher" \
    ran "$given_bin/chrono-grapher --config $given_work/conf/chrono-grapher-conf-1.json"
check "given binaries: start runs the given player" \
    ran "$given_bin/chrono-player --config $given_work/conf/chrono-player-conf-1.json"
bash "$script" -s "${given_args[@]}" > "$fixture/given/stop.out" 2>&1 || true
check "given binaries: stop stops the given keeper" ran "$given_bin/chrono-keeper stopped"
bash "$script" --restart-keeper 1 "${given_args[@]}" > "$fixture/given/restart.out" 2>&1 || true
wait_runs_of "$keeper_run" 2
check "given binaries: --restart-keeper runs the given keeper" test "$(runs_of "$keeper_run")" -eq 2
bash "$script" -s "${given_args[@]}" > /dev/null 2>&1 || true
check "given binaries: the installed binaries never ran" test "$(grep -c "^$given_work/bin/" "$runs")" -eq 0

echo "$failures deploy_local WAL checks failed"
test "$failures" -eq 0
