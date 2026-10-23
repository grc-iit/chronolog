#!/usr/bin/env bash
# Checks the keeper WAL handling of deploy_cluster.sh --stop, --clean and
# --restart-keeper against made-up keeper hosts. A parallel-ssh function and an
# ssh function run each remote command locally, with a hostname shim that
# answers the host's name, and each host's WAL directory is a real directory
# under the fixture.
set -uo pipefail
repo_root=${1:?repository root required}
script="$repo_root/tools/deploy/deploy_cluster.sh"
fixture=$(mktemp -d /tmp/chronolog-deploy-cluster-wal-test.XXXXXX)
trap 'pkill -f "$fixture/bin/chrono-keeper" 2> /dev/null; rm -rf -- "$fixture"' EXIT
failures=0
pass() { echo "PASS: $1"; }
fail() {
    echo "FAIL: $1" >&2
    failures=$((failures + 1))
}
check() { # LABEL COMMAND...
    local label=$1
    shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}

# Load the production functions without running the script.
for fn in work_dir_hash list_keeper_wal_dirs keeper_wal_remote_cmd collect_keeper_wal_states \
    report_keeper_wal_exit_summaries clean_keeper_wal_dirs get_remote_hostname restart_keeper \
    stop clean check_op_validity parse_args; do
    source <(sed -n "/^${fn}() {/,/^}/p" "$script")
done
ERR='' INFO='' DEBUG='' WARN='' NC=''
verbose=false JOB_ID="" NUM_RECORDING_GROUP=1 force=false
usage() { exit 1; }

# Remote side: "hostname" answers FAKE_HOSTNAME; hosts listed in $down do not answer.
mkdir -p "$fixture/shim"
printf '#!/usr/bin/env bash\necho "${FAKE_HOSTNAME:?}"\n' > "$fixture/shim/hostname"
chmod +x "$fixture/shim/hostname"
down=""
parallel-ssh() { # -h HOSTS_FILE -i COMMAND, as deploy_cluster.sh calls it
    local hosts_file="" cmd="" n=0 host
    while [[ $# -gt 0 ]]; do
        case "$1" in
        -h) hosts_file=$2 && shift ;;
        -i) ;;
        *) cmd=$1 ;;
        esac
        shift
    done
    [[ -f "$hosts_file" ]] || return 1
    while read -r host; do
        n=$((n + 1))
        if [[ " $down " == *" $host "* ]]; then
            echo "[$n] 10:00:00 [FAILURE] $host Exited with error code 255"
            continue
        fi
        echo "[$n] 10:00:00 [SUCCESS] $host"
        FAKE_HOSTNAME=$host PATH="$fixture/shim:$PATH" bash -c "$cmd" < /dev/null
    done < "$hosts_file"
}
ssh() { # -n HOST COMMAND
    [[ "$1" == -n ]] || return 255
    [[ " $down " == *" $2 "* ]] && return 255
    # Like ssh, return only once every remote process has closed the session's
    # stdout and stderr: a process left in the background holding them keeps
    # it open.
    FAKE_HOSTNAME=$2 PATH="$fixture/shim:$PATH" bash -c "$3" < /dev/null 2>&1 | cat
    return "${PIPESTATUS[0]}"
}

# No space here: clean's existing rm lines leave the work dir unquoted. The WAL
# directories below hold one.
WORK_DIR="$fixture/work"
CONF_DIR="$WORK_DIR/conf"
MONITOR_DIR="$WORK_DIR/monitor"
OUTPUT_DIR="$WORK_DIR/output"
CONF_FILE="$CONF_DIR/default-chrono-conf.json"
CLIENT_CONF_FILE="$CONF_DIR/default-chrono-client-conf.json"
KEEPER_HOSTS="$CONF_DIR/hosts_keeper"
VISOR_HOSTS="$CONF_DIR/hosts_visor" GRAPHER_HOSTS="$CONF_DIR/hosts_grapher" PLAYER_HOSTS="$CONF_DIR/hosts_player"
KEEPER_BIN_DIR="$fixture/bin" KEEPER_BIN_FILE_NAME=chrono-keeper
KEEPER_BIN="$KEEPER_BIN_DIR/$KEEPER_BIN_FILE_NAME"
VISOR_BIN="$fixture/bin/chrono-visor" GRAPHER_BIN="$fixture/bin/chrono-grapher" PLAYER_BIN="$fixture/bin/chrono-player"
hash=$(printf '%s' "$WORK_DIR" | sha256sum | cut -c1-12)

# Three keeper hosts in two recording groups; each host's WAL is local to it.
setup() {
    rm -rf -- "$WORK_DIR" "$fixture/hosts"
    mkdir -p "$CONF_DIR" "$MONITOR_DIR" "$OUTPUT_DIR"
    printf 'nodeA\nnodeB\nnodeC\n' > "$KEEPER_HOSTS"
    printf 'nodeA\nnodeB\n' > "$KEEPER_HOSTS.1"
    printf 'nodeC\n' > "$KEEPER_HOSTS.2"
    command cp -f "$repo_root/conf/default_conf.json.in" "$CONF_FILE"
    local host group dir
    for host in nodeA nodeB nodeC; do
        group=1
        [[ "$host" == nodeC ]] && group=2
        dir="$fixture/hosts/$host/mnt nvme/$USER/$hash/$group"
        mkdir -p "$dir"
        jq --arg d "$dir" '.chrono_keeper.WAL.enabled = true | .chrono_keeper.WAL.dir = $d' "$CONF_FILE" \
            > "$CONF_FILE.$group.keeper.$host"
    done
    down=""
}
wal_dir_of() { jq -r '.chrono_keeper.WAL.dir' "$CONF_FILE".*.keeper."$1"; }
summary() { # HOST EVENTS BYTES
    local dir
    dir=$(wal_dir_of "$1")
    echo "pending_events=$2 pending_bytes=$3" > "$dir/exit-summary"
    touch "$dir/00000001.wal"
}
all_clean() { summary nodeA 0 0 && summary nodeB 0 0 && summary nodeC 0 0; }

# --stop: the processes are stopped by no-op stand-ins that print a line into
# stop's output, so the table's place among the stops shows. The keepers' stand-in
# prints only after a pause, as a keeper exits only after it writes its summary, so
# the table comes first unless stop waits for the keepers to exit.
parallel_remote_stop_processes() {
    [[ "$(basename "$2")" == chrono-keeper ]] && sleep 0.3
    echo "stop $(basename "$2")"
}
parallel_remote_check_all() { :; }
check_hosts_files() { :; }
check_work_dir() { :; }
run_stop() { # prints the output; returns stop's status
    (stop) > "$fixture/stop-out" 2>&1
}
row() { grep -E "^$1 +$2 +" "$fixture/stop-out"; }
in_order() { # PATTERN...: the first line matching each comes after the first matching the one before
    local prev=0 n
    while [[ $# -gt 0 ]]; do
        n=$(grep -n -m 1 -E "$1" "$fixture/stop-out" | cut -d: -f1)
        [[ -n "$n" ]] && ((n > prev)) || return 1
        prev=$n
        shift
    done
}

setup
all_clean
if run_stop; then pass "stop: every summary clean exits 0"; else fail "stop: every summary clean exits 0: $(cat "$fixture/stop-out")"; fi
row nodeA 1 | grep -q ' clean ' && row nodeC 2 | grep -q ' clean ' && pass "stop: a row per keeper with its group" ||
    fail "stop: a row per keeper with its group: $(cat "$fixture/stop-out")"
grep -q "nodeB .*$(wal_dir_of nodeB)" "$fixture/stop-out" && pass "stop: the row names the WAL directory" ||
    fail "stop: the row names the WAL directory"

setup
summary nodeA 0 0
summary nodeB 7 4096
summary nodeC 0 0
if run_stop; then fail "stop: pending events exit nonzero"; else pass "stop: pending events exit nonzero"; fi
row nodeB 1 | grep -qE ' 7 +4096 +pending ' && pass "stop: pending counts shown" ||
    fail "stop: pending counts shown: $(cat "$fixture/stop-out")"
# The players and the keepers stop together, in either order.
grep -q '^stop chrono-player$' "$fixture/stop-out" && pass "stop: players stopped" || fail "stop: players stopped"
in_order '^stop chrono-keeper$' '^stop chrono-grapher$' '^stop chrono-visor$' &&
    pass "stop: keepers, graphers, then visor stopped before failing" ||
    fail "stop: keepers, graphers, then visor stopped before failing: $(cat "$fixture/stop-out")"
in_order '^stop chrono-keeper$' '^HOST +GROUP +PENDING_EVENTS' '^stop chrono-grapher$' &&
    pass "stop: the table is printed after the keepers stop and before the graphers do" ||
    fail "stop: the table is printed after the keepers stop and before the graphers do: $(cat "$fixture/stop-out")"

# A count of 2^64 wraps to 0 in bash arithmetic; it is still pending.
setup
all_clean
summary nodeB 18446744073709551616 4096
if run_stop; then fail "stop: a 2^64 pending count exits nonzero"; else pass "stop: a 2^64 pending count exits nonzero"; fi
row nodeB 1 | grep -qE ' 18446744073709551616 +4096 +pending ' && pass "stop: a 2^64 pending count shown as pending" ||
    fail "stop: a 2^64 pending count shown as pending: $(cat "$fixture/stop-out")"
summary nodeB 000 0
if run_stop; then pass "stop: an all-zero count is clean"; else fail "stop: an all-zero count is clean: $(cat "$fixture/stop-out")"; fi

setup
summary nodeA 0 0
summary nodeC 0 0
touch "$(wal_dir_of nodeB)/00000001.wal"
if run_stop; then fail "stop: a missing summary exits nonzero"; else pass "stop: a missing summary exits nonzero"; fi
row nodeB 1 | grep -q ' missing ' && pass "stop: missing summary shown" || fail "stop: missing summary shown: $(cat "$fixture/stop-out")"

setup
all_clean
echo "garbage" > "$(wal_dir_of nodeC)/exit-summary"
if run_stop; then fail "stop: an unreadable summary exits nonzero"; else pass "stop: an unreadable summary exits nonzero"; fi
row nodeC 2 | grep -q ' missing ' && pass "stop: unreadable summary shown as missing" || fail "stop: unreadable summary shown as missing"

setup
all_clean
down="nodeC"
if run_stop; then fail "stop: an unreachable host exits nonzero"; else pass "stop: an unreachable host exits nonzero"; fi
row nodeC 2 | grep -q ' unreachable ' && pass "stop: unreachable host shown" || fail "stop: unreachable host shown: $(cat "$fixture/stop-out")"

# No summary was written, so stop cannot tell the keeper exited clean.
setup
summary nodeA 0 0
summary nodeC 0 0
if run_stop; then fail "stop: an empty WAL directory exits nonzero"; else pass "stop: an empty WAL directory exits nonzero"; fi
row nodeB 1 | grep -q ' empty ' && pass "stop: empty WAL directory shown" || fail "stop: empty WAL directory shown: $(cat "$fixture/stop-out")"
grep -q "absent or empty" "$fixture/stop-out" && pass "stop: the error names an absent or empty WAL directory" ||
    fail "stop: the error names an absent or empty WAL directory: $(cat "$fixture/stop-out")"
rmdir "$(wal_dir_of nodeB)"
if run_stop; then fail "stop: an absent WAL directory exits nonzero"; else pass "stop: an absent WAL directory exits nonzero"; fi
row nodeB 1 | grep -q ' empty ' && pass "stop: absent WAL directory shown as empty" ||
    fail "stop: absent WAL directory shown as empty: $(cat "$fixture/stop-out")"

setup
for conf in "$CONF_FILE".*.keeper.*; do
    jq '.chrono_keeper.WAL.enabled = false' "$conf" > "$conf.tmp" && mv "$conf.tmp" "$conf"
done
if run_stop; then pass "stop: WAL off exits 0"; else fail "stop: WAL off exits 0: $(cat "$fixture/stop-out")"; fi
grep -q PENDING_EVENTS "$fixture/stop-out" && fail "stop: WAL off prints no table" || pass "stop: WAL off prints no table"

# --clean
parallel_remote_check_processes() { :; }
clean_output_dir() { :; }
run_clean() { (clean) > "$fixture/clean-out" 2>&1; }

setup
all_clean
if run_clean; then pass "clean: clean summaries exit 0"; else fail "clean: clean summaries exit 0: $(cat "$fixture/clean-out")"; fi
[[ ! -e "$fixture/hosts/nodeA/mnt nvme/$USER/$hash" && ! -e "$fixture/hosts/nodeC/mnt nvme/$USER/$hash" ]] &&
    pass "clean: WAL directories and their empty hash directory removed" || fail "clean: WAL directories and their empty hash directory removed"
[[ -d "$fixture/hosts/nodeA/mnt nvme/$USER" ]] && pass "clean: the base directory is kept" || fail "clean: the base directory is kept"
compgen -G "$CONF_FILE.*" > /dev/null && fail "clean: generated confs removed" || pass "clean: generated confs removed"

for case_ in pending pending-2^64 missing unreachable; do
    setup
    all_clean
    case $case_ in
    pending) summary nodeB 3 300 ;;
    pending-2^64) summary nodeB 18446744073709551616 300 ;;
    missing) rm -f "$(wal_dir_of nodeB)/exit-summary" ;;
    unreachable) down=nodeB ;;
    esac
    if run_clean; then fail "clean: refuses with a $case_ keeper"; else pass "clean: refuses with a $case_ keeper"; fi
    [[ -d "$(wal_dir_of nodeA)" && -d "$(wal_dir_of nodeB)" && -d "$(wal_dir_of nodeC)" ]] &&
        pass "clean: $case_ refusal removes no WAL directory" || fail "clean: $case_ refusal removes no WAL directory"
    [[ -f "$CONF_FILE.1.keeper.nodeB" && -f "$KEEPER_HOSTS.1" ]] && pass "clean: $case_ refusal keeps the confs" ||
        fail "clean: $case_ refusal keeps the confs"
    grep -q "nodeB" "$fixture/clean-out" && grep -q -- --force "$fixture/clean-out" &&
        pass "clean: $case_ refusal names the host and --force" || fail "clean: $case_ refusal names the host and --force: $(cat "$fixture/clean-out")"
done

setup
all_clean
summary nodeB 3 300
rm -f "$(wal_dir_of nodeC)/exit-summary"
force=true
if run_clean; then pass "clean --force: exits 0"; else fail "clean --force: exits 0: $(cat "$fixture/clean-out")"; fi
force=false
[[ ! -e "$fixture/hosts/nodeB/mnt nvme/$USER/$hash" && ! -e "$fixture/hosts/nodeC/mnt nvme/$USER/$hash" ]] &&
    pass "clean --force: pending and missing WAL directories removed" || fail "clean --force: pending and missing WAL directories removed"

setup
all_clean
rm -f "$(wal_dir_of nodeA)"/*
rmdir "$(wal_dir_of nodeA)"
rm -f "$(wal_dir_of nodeB)"/*
if run_clean; then pass "clean: absent and empty WAL directories hold nothing to refuse"; else
    fail "clean: absent and empty WAL directories hold nothing to refuse: $(cat "$fixture/clean-out")"
fi

setup
other="$fixture/hosts/nodeA/elsewhere"
mkdir -p "$other"
touch "$other/keep"
jq --arg d "$other" '.chrono_keeper.WAL.dir = $d' "$CONF_FILE.1.keeper.nodeA" > "$fixture/tmp.json" &&
    mv "$fixture/tmp.json" "$CONF_FILE.1.keeper.nodeA"
echo "pending_events=0 pending_bytes=0" > "$other/exit-summary"
summary nodeB 0 0
summary nodeC 0 0
run_clean
[[ -f "$other/keep" ]] && pass "clean: a WAL directory not named <hash>/<group> is not removed" ||
    fail "clean: a WAL directory not named <hash>/<group> is not removed"
grep -q "$other" "$fixture/clean-out" && pass "clean: the directory left behind is named" || fail "clean: the directory left behind is named"

# --restart-keeper
mkdir -p "$KEEPER_BIN_DIR"
# no exec: the fake keeper keeps "<bin> --config <conf>" as its command line for
# restart_keeper's pgrep, and runs 30 s
printf '#!/usr/bin/env bash\necho "launched $*" >> "%s"\nfor _ in $(seq 30); do sleep 1; done\n' "$fixture/launches" > "$KEEPER_BIN"
chmod +x "$KEEPER_BIN"
setup
echo "old run" > "$MONITOR_DIR/chrono-keeper-nodeB.launch.log"
restart_start=$SECONDS
if (restart_keeper nodeB) > "$fixture/restart-out" 2>&1; then pass "restart: exits 0"; else fail "restart: exits 0: $(cat "$fixture/restart-out")"; fi
# the fake keeper runs 30 s; a launch that keeps the ssh session open returns only then
(( SECONDS - restart_start < 10 )) && pass "restart: returns while the relaunched keeper runs" ||
    fail "restart: returns while the relaunched keeper runs (took $((SECONDS - restart_start)) s)"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [[ -s "$fixture/launches" ]] && break
    sleep 0.2
done
[[ "$(cat "$fixture/launches" 2> /dev/null)" == "launched --config $CONF_FILE.1.keeper.nodeB" ]] &&
    pass "restart: keeper launched with that host's generated conf" ||
    fail "restart: keeper launched with that host's generated conf: $(cat "$fixture/launches" 2> /dev/null)"
head -n 1 "$MONITOR_DIR/chrono-keeper-nodeB.launch.log" | grep -q "old run" &&
    pass "restart: the old launch log is kept" || fail "restart: the old launch log is kept"
if (restart_keeper nodeB) > "$fixture/restart-out" 2>&1; then fail "restart: refuses while the keeper runs"; else
    grep -q "still running" "$fixture/restart-out" && pass "restart: refuses while the keeper runs" ||
        fail "restart: refuses while the keeper runs: $(cat "$fixture/restart-out")"
fi
[[ $(wc -l < "$fixture/launches") -eq 1 ]] && pass "restart: no second keeper launched" || fail "restart: no second keeper launched"
pkill -f "$fixture/bin/chrono-keeper"
if (restart_keeper nodeZ) > "$fixture/restart-out" 2>&1; then fail "restart: refuses a host without a conf"; else
    grep -q "nodeZ" "$fixture/restart-out" && pass "restart: refuses a host without a conf" || fail "restart: refuses a host without a conf"
fi
down=nodeB
if (restart_keeper nodeB) > /dev/null 2>&1; then fail "restart: refuses an unreachable host"; else pass "restart: refuses an unreachable host"; fi

# --start with --wal-dir: each keeper host runs the real wal_dir_probe.sh over
# the fake ssh while its conf is generated, before anything is launched
for fn in start generate_conf_for_each_recording_group generate_conf_for_each_keeper \
    generate_conf_for_each_grapher generate_conf_for_each_player assign_keeper_wal_dir wal_enabled; do
    source <(sed -n "/^${fn}() {/,/^}/p" "$script")
done
SCRIPT_DIR="$repo_root/tools/deploy"
LIB_DIR="$fixture/lib"
VISOR_BIN_DIR="$fixture/bin" GRAPHER_BIN_DIR="$fixture/bin" PLAYER_BIN_DIR="$fixture/bin"
VISOR_BIN_FILE_NAME=chrono-visor GRAPHER_BIN_FILE_NAME=chrono-grapher PLAYER_BIN_FILE_NAME=chrono-player
prepare_hosts() { :; }
check_bin_files() { :; }
update_visor_ip() { :; }
update_visor_monitor_file_path() { :; }
update_client_monitor_file_path() { :; }
check_rpc_comm_conf() { :; }
check_archive_mount() { :; }
get_host_ip() { echo 127.0.0.1; }
parallel_remote_launch_processes() { echo "launch $4 from $3" >> "$fixture/start-launches"; }
run_start() { # WAL_DIR_LIST: prints start's output into start-out; returns start's status
    setup
    rm -f -- "$CONF_FILE".*.keeper.* "$fixture/start-launches"
    printf 'nodeV\n' > "$GRAPHER_HOSTS.1"
    printf 'nodeV\n' > "$GRAPHER_HOSTS.2"
    printf 'nodeV\n' > "$PLAYER_HOSTS.1"
    printf 'nodeV\n' > "$PLAYER_HOSTS.2"
    NUM_RECORDING_GROUP=2 WAL_ENABLED=1 WAL_DIR_LIST=$1
    # the probe's fallback, so a probe that falls back writes into the fixture
    rm -rf -- "$fixture/tmp"
    mkdir -p "$fixture/tmp"
    (
        export TMPDIR="$fixture/tmp"
        start
    ) > "$fixture/start-out" 2>&1
    local status=$?
    NUM_RECORDING_GROUP=1 WAL_ENABLED=0 WAL_DIR_LIST=""
    return "$status"
}

if run_start "$fixture/missing:$fixture/gone"; then
    fail "start: a --wal-dir list with no usable entry fails the deploy"
else
    pass "start: a --wal-dir list with no usable entry fails the deploy"
fi
[[ ! -e "$fixture/start-launches" ]] && pass "start: no process launched when a probe fails" ||
    fail "start: no process launched when a probe fails: $(cat "$fixture/start-launches")"
grep -q "nodeA: ERROR: $fixture/missing: does not exist" "$fixture/start-out" &&
    grep -q "nodeA: ERROR: $fixture/gone: does not exist" "$fixture/start-out" &&
    pass "start: the failure names the host and each entry's error" ||
    fail "start: the failure names the host and each entry's error: $(cat "$fixture/start-out")"
grep -q "nodeB\|nodeC" "$fixture/start-out" && fail "start: stops at the first host whose probe fails" ||
    pass "start: stops at the first host whose probe fails"
[[ -z "$(ls -A "$fixture/tmp")" ]] && pass "start: no fallback directory created" ||
    fail "start: no fallback directory created: $(ls -A "$fixture/tmp")"

mkdir -p "$fixture/usable"
if run_start "$fixture/missing:$fixture/usable"; then
    pass "start: a --wal-dir list whose second entry is usable deploys"
else
    fail "start: a --wal-dir list whose second entry is usable deploys: $(cat "$fixture/start-out")"
fi
[[ "$(wal_dir_of nodeC)" == "$fixture/usable/$hash/2" ]] && pass "start: the usable entry is written into the conf" ||
    fail "start: the usable entry is written into the conf: $(wal_dir_of nodeC)"
grep -q "launch chrono-keeper from $KEEPER_HOSTS.2" "$fixture/start-launches" && pass "start: keepers launched" ||
    fail "start: keepers launched: $(cat "$fixture/start-launches")"

# Arguments, and their pass-through from single_user_deploy.sh
reset_args() { start=false stop=false clean=false restart=false force=false RESTART_KEEPER_HOST=""; }
reset_args
parse_args --restart-keeper nodeB > /dev/null
[[ "$restart" == true && "$RESTART_KEEPER_HOST" == nodeB ]] && (check_op_validity) &&
    pass "args: --restart-keeper selects the restart mode with its host" || fail "args: --restart-keeper selects the restart mode with its host"
reset_args
parse_args --clean --force > /dev/null
[[ "$force" == true && "$clean" == true ]] && pass "args: --force with --clean" || fail "args: --force with --clean"
reset_args
parse_args --stop --restart-keeper nodeB > /dev/null
(check_op_validity) > /dev/null 2>&1 && fail "args: --restart-keeper with another mode refused" ||
    pass "args: --restart-keeper with another mode refused"

source <(sed -n '/^delegate_to_deploy_cluster() {/,/^}/p; /^parse_args() {/,/^}/p; /^check_op_validity() {/,/^}/p' \
    "$repo_root/tools/deploy/single_user_deploy.sh")
DEPLOY_CLUSTER_SCRIPT="$fixture/fake_deploy_cluster.sh"
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$@" > "%s"\n' "$fixture/delegated-args" > "$DEPLOY_CLUSTER_SCRIPT"
chmod +x "$DEPLOY_CLUSTER_SCRIPT"
delegated() { # MODE ARGS...: runs single_user_deploy.sh's parse_args and delegation, prints the delegated args
    local mode=$1
    shift
    build=false install=false start=false stop=false clean=false restart=false force=false
    wal=false WAL_DIR_LIST="" RESTART_KEEPER_HOST="" JOB_ID="" verbose=false NUM_RECORDING_GROUP=1
    WORK_DIR="$fixture/work"
    MONITOR_DIR="$WORK_DIR/monitor" OUTPUT_DIR="$WORK_DIR/output"
    VISOR_BIN="$WORK_DIR/bin/chrono-visor" GRAPHER_BIN="$WORK_DIR/bin/chrono-grapher"
    KEEPER_BIN="$WORK_DIR/bin/chrono-keeper" PLAYER_BIN="$WORK_DIR/bin/chrono-player"
    CONF_FILE="$WORK_DIR/conf/default-chrono-conf.json" CLIENT_CONF_FILE="$WORK_DIR/conf/default-chrono-client-conf.json"
    VISOR_HOSTS="$WORK_DIR/conf/hosts_visor" GRAPHER_HOSTS="$WORK_DIR/conf/hosts_grapher"
    KEEPER_HOSTS="$WORK_DIR/conf/hosts_keeper"
    parse_args "$@" > /dev/null
    (check_op_validity) > /dev/null 2>&1 || {
        echo "invalid"
        return
    }
    rm -f "$fixture/delegated-args"
    delegate_to_deploy_cluster "$mode" > /dev/null
    tr '\n' ' ' < "$fixture/delegated-args"
}
[[ "$(delegated --clean --clean --force)" == "--clean "*"--force "* ]] &&
    pass "single_user_deploy: --force passed on clean" || fail "single_user_deploy: --force passed on clean: $(delegated --clean --clean --force)"
[[ "$(delegated --stop --stop --force)" != *"--force"* ]] &&
    pass "single_user_deploy: --force not passed on stop" || fail "single_user_deploy: --force not passed on stop"
[[ "$(delegated --restart-keeper --restart-keeper nodeB)" == "--restart-keeper nodeB "* ]] &&
    pass "single_user_deploy: --restart-keeper passed with its host" ||
    fail "single_user_deploy: --restart-keeper passed with its host: $(delegated --restart-keeper --restart-keeper nodeB)"
[[ "$(delegated --restart-keeper --restart-keeper nodeB --stop)" == invalid ]] &&
    pass "single_user_deploy: --restart-keeper with another mode refused" || fail "single_user_deploy: --restart-keeper with another mode refused"

echo "$failures deploy_cluster WAL checks failed"
test "$failures" -eq 0
