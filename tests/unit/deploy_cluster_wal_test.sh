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
    visor_catalog_path update_visor_catalog_path warn_catalog_on_nfs clean_visor_catalog \
    stop clean check_op_validity parse_args; do
    source <(sed -n "/^${fn}() {/,/^}/p" "$script")
done
ERR='' INFO='' DEBUG='' WARN='' NC=''
# The fixture's WAL paths follow the <mount>/$USER layout. deploy_cluster.sh
# sets USER from whoami at its top, which is not loaded here, and a container
# may not set it.
USER=${USER:-$(whoami)}
verbose=false JOB_ID="" NUM_RECORDING_GROUP=1 force=false CATALOG_DIR=""
usage() { exit 1; }

# Remote side: "hostname" answers FAKE_HOSTNAME; hosts listed in $down do not answer.
mkdir -p "$fixture/shim"
printf '#!/usr/bin/env bash\necho "${FAKE_HOSTNAME:?}"\n' > "$fixture/shim/hostname"
chmod +x "$fixture/shim/hostname"
down="" host_suffix="" # what `hostname` adds to the visor host nodeV, as an FQDN does
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
        [[ -z "$host" || "$host" == \#* ]] && continue # parallel-ssh skips these
        n=$((n + 1))
        if [[ " $down " == *" $host "* ]]; then
            echo "[$n] 10:00:00 [FAILURE] $host Exited with error code 255"
            continue
        fi
        echo "[$n] 10:00:00 [SUCCESS] $host"
        FAKE_HOSTNAME=$host$([[ $host == nodeV ]] && echo "$host_suffix") PATH="$fixture/shim:$PATH" bash -c "$cmd" < /dev/null
    done < "$hosts_file"
}
minpath="$fixture/minpath" # dirname, head, sed and bash only: no findmnt, no stat
mkdir -p "$minpath"
for tool in bash dirname head sed; do ln -sf "$(command -v "$tool")" "$minpath/$tool"; done
ssh() { # -n [-o OPTION]... HOST COMMAND
    [[ "$1" == -n ]] || return 255
    shift
    : > "$fixture/ssh-options"
    while [[ "$1" == -o ]]; do
        echo "$2" >> "$fixture/ssh-options"
        shift 2
    done
    set -- x "$@"
    [[ " $down " == *" $2 "* ]] && return 255
    if [[ -n "${MINPATH:-}" ]]; then
        FAKE_HOSTNAME=$2$([[ $2 == nodeV ]] && echo "$host_suffix") PATH="$minpath" bash -c "$3" < /dev/null 2>&1 | cat
        return "${PIPESTATUS[0]}"
    fi
    # Like ssh, return only once every remote process has closed the session's
    # stdout and stderr: a process left in the background holding them keeps
    # it open.
    FAKE_HOSTNAME=$2$([[ $2 == nodeV ]] && echo "$host_suffix") PATH="$fixture/shim:$PATH" bash -c "$3" < /dev/null 2>&1 | cat
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

# The deployment lifecycle (deployment_lifecycle.py) owns the processes and has
# its own tests. Here it is a stand-in that logs each call, prints a line into
# the command's output and returns LIFECYCLE_STATUS. Its stop prints only after a
# pause, as the managed stop returns only after every keeper has exited and
# written its summary, so the table comes first unless stop waits for it.
LIFECYCLE_STATUS=0
lifecycle() {
    # The real one passes the --host routes in LIFECYCLE_HOST_ARGS first.
    echo "lifecycle ${LIFECYCLE_HOST_ARGS[*]:+${LIFECYCLE_HOST_ARGS[*]} }$*" >> "$fixture/lifecycle-calls"
    # Options such as --budget-ms come before the command.
    while [[ "$1" == --* ]]; do shift 2; done
    [[ "$1" == stop ]] && sleep 0.3
    echo "lifecycle $1"
    return "$LIFECYCLE_STATUS"
}
lifecycle_hosts() { :; }
fence_mutation() { :; }
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
grep -q '^lifecycle stop$' "$fixture/stop-out" && pass "stop: the managed stop ran before failing" ||
    fail "stop: the managed stop ran before failing: $(cat "$fixture/stop-out")"
# The stop budget covers the keeper's confirmation wait from the conf template
# (150 s when it sets none) plus 180 s.
confirm_secs=$(jq -r '.chrono_keeper.DataStoreInternals.shutdown_confirm_timeout_secs // 150' "$CONF_FILE")
grep -qE -- "--budget-ms $(((confirm_secs + 180) * 1000)) stop\$" "$fixture/lifecycle-calls" &&
    pass "stop: the stop budget covers the keeper's confirmation wait" ||
    fail "stop: the stop budget covers the keeper's confirmation wait: $(grep stop "$fixture/lifecycle-calls")"
restart_advice() { # FILE: the advice after a WAL that is not clean, whole deployment restarted with the same options
    local text
    text=$(tr -s ' \n' ' ' < "$1")
    [[ "$text" == *"Start the deployment again with the same options, including --wal or --wal-dir, and the same host order"*"then stop it again"* &&
        "$text" == *"--restart-keeper <host> is only for a deployment whose visor and graphers still run"* ]]
}
restart_advice "$fixture/stop-out" && pass "stop: the error says to start again with the same options and host order" ||
    fail "stop: the error says to start again with the same options and host order: $(cat "$fixture/stop-out")"
in_order '^lifecycle stop$' '^HOST +GROUP +PENDING_EVENTS' &&
    pass "stop: the table is printed after the managed stop returns" ||
    fail "stop: the table is printed after the managed stop returns: $(cat "$fixture/stop-out")"

# An incomplete managed stop leaves the keepers running: there is no table yet.
setup
all_clean
LIFECYCLE_STATUS=27
run_stop
status=$?
LIFECYCLE_STATUS=0
[[ $status -eq 27 ]] && pass "stop: an incomplete managed stop exits 27" ||
    fail "stop: an incomplete managed stop exits 27 (got $status): $(cat "$fixture/stop-out")"
grep -q PENDING_EVENTS "$fixture/stop-out" && fail "stop: an incomplete managed stop prints no table" ||
    pass "stop: an incomplete managed stop prints no table"

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
    restart_advice "$fixture/clean-out" && pass "clean: $case_ refusal says to start again with the same options" ||
        fail "clean: $case_ refusal says to start again with the same options: $(cat "$fixture/clean-out")"
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

# The visor catalog: the path written into the conf, and --clean's removal of
# the files that conf names on the visor host.
catalog_dir="$fixture/hosts/nodeV/local"
catalog_setup() { # a visor host with a catalog directory and files, named by an absolute catalog_db_path
    setup
    all_clean
    CATALOG_DIR=""
    printf 'nodeV\n' > "$VISOR_HOSTS"
    mkdir -p "$catalog_dir"
    jq --arg p "$catalog_dir/catalog.sqlite" '.chrono_visor.catalog_db_path = $p' "$CONF_FILE" > "$fixture/tmp.json" &&
        mv "$fixture/tmp.json" "$CONF_FILE"
    touch "$catalog_dir/catalog.sqlite" "$catalog_dir/catalog.sqlite-wal" "$catalog_dir/catalog.sqlite-shm" "$catalog_dir/other"
}
clean_output_dir() { # records whether the catalog was already gone when the archive is removed
    if [[ -e "$catalog_dir/catalog.sqlite" ]]; then echo catalog-present > "$fixture/order"; else echo catalog-gone > "$fixture/order"; fi
}
catalog_setup
rm -f "$fixture/order"
if run_clean; then pass "catalog clean: exits 0"; else fail "catalog clean: exits 0: $(cat "$fixture/clean-out")"; fi
[[ ! -e "$catalog_dir/catalog.sqlite" && ! -e "$catalog_dir/catalog.sqlite-wal" && ! -e "$catalog_dir/catalog.sqlite-shm" ]] &&
    pass "catalog clean: the catalog and its -wal and -shm files are removed" || fail "catalog clean: the catalog and its -wal and -shm files are removed"
[[ -f "$catalog_dir/other" ]] && pass "catalog clean: an unrelated file in the directory is kept" || fail "catalog clean: an unrelated file in the directory is kept"
[[ "$(grep -c 'Removed visor catalog file' "$fixture/clean-out")" -eq 3 ]] && grep -q "catalog.sqlite-shm on nodeV" "$fixture/clean-out" &&
    pass "catalog clean: each removed file is named" || fail "catalog clean: each removed file is named: $(cat "$fixture/clean-out")"
[[ "$(cat "$fixture/order" 2> /dev/null)" == catalog-gone ]] && pass "catalog clean: the catalog goes before the archive" ||
    fail "catalog clean: the catalog goes before the archive"

catalog_setup
down=nodeV
rm -f "$fixture/order"
if run_clean; then fail "catalog clean: a visor host that did not answer fails clean"; else pass "catalog clean: a visor host that did not answer fails clean"; fi
grep -q "WARNING: nodeV did not answer; $catalog_dir/catalog.sqlite is not removed" "$fixture/clean-out" &&
    pass "catalog clean: the unanswered host and the catalog are named" || fail "catalog clean: the unanswered host and the catalog are named: $(cat "$fixture/clean-out")"
[[ -f "$catalog_dir/catalog.sqlite" && -f "$catalog_dir/catalog.sqlite-wal" && -f "$catalog_dir/catalog.sqlite-shm" ]] &&
    pass "catalog clean: the files stay when the host did not answer" || fail "catalog clean: the files stay when the host did not answer"
[[ ! -e "$fixture/order" ]] && pass "catalog clean: the archive is not removed when the catalog is not" ||
    fail "catalog clean: the archive is not removed when the catalog is not"
grep -q "Visor catalog not removed; the archive in $OUTPUT_DIR is kept. Run --clean again" "$fixture/clean-out" &&
    pass "catalog clean: the failure says the archive is kept" || fail "catalog clean: the failure says the archive is kept: $(cat "$fixture/clean-out")"
compgen -G "$CONF_FILE.*" > /dev/null && pass "catalog clean: the generated confs stay for the next --clean" ||
    fail "catalog clean: the generated confs stay for the next --clean"
down=""

# A hosts_visor entry that is not what `hostname` prints (an FQDN here), and a
# comment line in the file: both are clean.
catalog_setup
printf '# visor\nnodeV\n' > "$VISOR_HOSTS"
host_suffix=.example.org
rm -f "$fixture/order"
if run_clean; then pass "catalog clean: an entry that differs from hostname is clean"; else
    fail "catalog clean: an entry that differs from hostname is clean: $(cat "$fixture/clean-out")"
fi
[[ ! -e "$catalog_dir/catalog.sqlite" ]] && grep -q "catalog.sqlite-shm on nodeV" "$fixture/clean-out" &&
    ! grep -q "did not answer" "$fixture/clean-out" && pass "catalog clean: the files go and the entry is named, not hostname" ||
    fail "catalog clean: the files go and the entry is named, not hostname: $(cat "$fixture/clean-out")"
host_suffix=""
catalog_setup
printf '# visor\nnodeV\n' > "$VISOR_HOSTS"
if run_clean; then pass "catalog clean: a # comment line in hosts_visor is skipped"; else
    fail "catalog clean: a # comment line in hosts_visor is skipped: $(cat "$fixture/clean-out")"
fi
down=""

# A file that cannot be removed is not a clean host.
if [[ "$(id -u)" != 0 ]]; then
    catalog_setup
    rm -f "$fixture/order"
    chmod a-w "$catalog_dir"
    if run_clean; then fail "catalog clean: a file that cannot be removed fails clean"; else pass "catalog clean: a file that cannot be removed fails clean"; fi
    chmod u+w "$catalog_dir"
    grep -q "WARNING: $catalog_dir/catalog.sqlite is not removed on nodeV" "$fixture/clean-out" &&
        pass "catalog clean: the file and the host are named" || fail "catalog clean: the file and the host are named: $(cat "$fixture/clean-out")"
    [[ -f "$catalog_dir/catalog.sqlite" && ! -e "$fixture/order" ]] && pass "catalog clean: the catalog and the archive stay" ||
        fail "catalog clean: the catalog and the archive stay"
else
    echo "SKIP: catalog clean: a non-writable catalog directory (running as root)"
fi

# A directory the user chose with --catalog-dir is kept when it is emptied.
catalog_setup
CATALOG_DIR="$catalog_dir"
rm -f "$catalog_dir/other"
run_clean
[[ -d "$catalog_dir" && ! -e "$catalog_dir/catalog.sqlite" ]] &&
    pass "catalog clean: an emptied --catalog-dir directory is kept" || fail "catalog clean: an emptied --catalog-dir directory is kept"
CATALOG_DIR=""

# A reinstall resets the conf to the template's relative value: clean finds the
# catalog where start put it, under the work directory.
setup
printf 'nodeV\n' > "$VISOR_HOSTS"
mkdir -p "$WORK_DIR/visor"
touch "$WORK_DIR/visor/catalog.sqlite" "$WORK_DIR/visor/catalog.sqlite-wal"
if run_clean; then pass "catalog clean: a relative catalog_db_path in the conf cleans"; else
    fail "catalog clean: a relative catalog_db_path in the conf cleans: $(cat "$fixture/clean-out")"
fi
[[ ! -e "$WORK_DIR/visor" ]] && pass "catalog clean: a relative catalog_db_path is read under the work dir, and its directory goes" ||
    fail "catalog clean: a relative catalog_db_path is read under the work dir, and its directory goes"
# --catalog-dir given to --clean names the catalog after a reinstall.
setup
printf 'nodeV\n' > "$VISOR_HOSTS"
mkdir -p "$catalog_dir"
touch "$catalog_dir/catalog.sqlite"
CATALOG_DIR="$catalog_dir"
run_clean
[[ ! -e "$catalog_dir/catalog.sqlite" ]] && pass "catalog clean: --catalog-dir names the catalog when the conf holds the template value" ||
    fail "catalog clean: --catalog-dir names the catalog when the conf holds the template value"
CATALOG_DIR=""
clean_output_dir() { :; }

# The path update writes. update writes tmp.json in the working directory.
catalog_path_after() { # prints chrono_visor.catalog_db_path after update_visor_catalog_path
    (cd "$fixture" && update_visor_catalog_path && jq -r '.chrono_visor.catalog_db_path' "$CONF_FILE")
}
setup
[[ "$(catalog_path_after)" == "$WORK_DIR/visor/catalog.sqlite" ]] && pass "catalog path: the template's relative value goes under the work dir" ||
    fail "catalog path: the template's relative value goes under the work dir: $(catalog_path_after)"
catalog_setup
[[ "$(catalog_path_after)" == "$catalog_dir/catalog.sqlite" ]] && pass "catalog path: an absolute catalog_db_path in the conf survives" ||
    fail "catalog path: an absolute catalog_db_path in the conf survives: $(catalog_path_after)"
CATALOG_DIR="$fixture/hosts/nodeV/chosen"
ln -sfn "$fixture/hosts/nodeV/real" "$fixture/link"
mkdir -p "$fixture/hosts/nodeV/real"
CATALOG_DIR="$fixture/link" ; [[ "$(catalog_path_after)" == "$fixture/link/catalog.sqlite" ]] &&
    pass "catalog path: a symlink in --catalog-dir is not resolved" || fail "catalog path: a symlink in --catalog-dir is not resolved: $(catalog_path_after)"
CATALOG_DIR="$fixture/hosts/nodeV/chosen"
[[ "$(catalog_path_after)" == "$fixture/hosts/nodeV/chosen/catalog.sqlite" ]] && pass "catalog path: --catalog-dir wins over the conf" ||
    fail "catalog path: --catalog-dir wins over the conf: $(catalog_path_after)"
CATALOG_DIR=""
mv "$CONF_FILE" "$CONF_FILE.gone"
visor_catalog_path > "$fixture/path-out" 2> "$fixture/path-err"
[[ "$(cat "$fixture/path-out")" == "$WORK_DIR/visor/catalog.sqlite" && ! -s "$fixture/path-err" ]] &&
    pass "catalog path: a missing conf falls back to work_dir/visor without a jq error" ||
    fail "catalog path: a missing conf falls back to work_dir/visor without a jq error: $(cat "$fixture/path-out" "$fixture/path-err")"
mv "$CONF_FILE.gone" "$CONF_FILE"

# The NFS warning, with findmnt answering for the visor host.
printf '#!/usr/bin/env bash\necho "${FAKE_FSTYPE:?}"\n' > "$fixture/shim/findmnt"
chmod +x "$fixture/shim/findmnt"
catalog_setup
FAKE_FSTYPE=nfs4 warn_catalog_on_nfs > "$fixture/nfs-out" 2>&1
grep -q "WARNING: nodeV: the visor catalog $catalog_dir/catalog.sqlite is on nfs4" "$fixture/nfs-out" && grep -q -- --catalog-dir "$fixture/nfs-out" &&
    pass "catalog nfs: a catalog on NFS gets a warning naming --catalog-dir" || fail "catalog nfs: a catalog on NFS gets a warning naming --catalog-dir: $(cat "$fixture/nfs-out")"
FAKE_FSTYPE=ext4 warn_catalog_on_nfs > "$fixture/nfs-out" 2>&1
[[ ! -s "$fixture/nfs-out" ]] && pass "catalog nfs: a local file system gets no warning" || fail "catalog nfs: a local file system gets no warning"
FAKE_FSTYPE=ext4 warn_catalog_on_nfs > /dev/null 2>&1
grep -qx "BatchMode=yes" "$fixture/ssh-options" && grep -qx "ConnectTimeout=10" "$fixture/ssh-options" &&
    pass "catalog nfs: the probe cannot prompt or hang" || fail "catalog nfs: the probe cannot prompt or hang: $(cat "$fixture/ssh-options")"
down=nodeV
FAKE_FSTYPE=nfs4 warn_catalog_on_nfs > "$fixture/nfs-out" 2>&1
[[ ! -s "$fixture/nfs-out" ]] && pass "catalog nfs: a host that does not answer gets no warning" || fail "catalog nfs: a host that does not answer gets no warning"
down=""
verbose=true MINPATH=1 warn_catalog_on_nfs > "$fixture/nfs-out" 2>&1
grep -q "nodeV: neither findmnt nor stat is available" "$fixture/nfs-out" && ! grep -q WARNING "$fixture/nfs-out" &&
    pass "catalog nfs: a host with neither findmnt nor stat is reported when verbose" ||
    fail "catalog nfs: a host with neither findmnt nor stat is reported when verbose: $(cat "$fixture/nfs-out")"
verbose=false MINPATH=1 warn_catalog_on_nfs > "$fixture/nfs-out" 2>&1
[[ ! -s "$fixture/nfs-out" ]] && pass "catalog nfs: the missing tools are not reported unless verbose" || fail "catalog nfs: the missing tools are not reported unless verbose"
rm -f "$fixture/shim/findmnt"

# --restart-keeper: the lifecycle relaunches the keeper, and refuses while the
# recorded one still runs (test_deployment_lifecycle.py checks that part).
setup
rm -f "$fixture/lifecycle-calls"
if (restart_keeper nodeB) > "$fixture/restart-out" 2>&1; then pass "restart: exits 0"; else fail "restart: exits 0: $(cat "$fixture/restart-out")"; fi
[[ "$(cat "$fixture/lifecycle-calls" 2> /dev/null)" == "lifecycle --host nodeB restart-keeper --executable $KEEPER_BIN --config $CONF_FILE.1.keeper.nodeB --output $MONITOR_DIR/chrono-keeper-nodeB.launch.log" ]] &&
    pass "restart: the lifecycle relaunches that host's keeper with its generated conf" ||
    fail "restart: the lifecycle relaunches that host's keeper with its generated conf: $(cat "$fixture/lifecycle-calls" 2> /dev/null)"
LIFECYCLE_STATUS=27
if (restart_keeper nodeB) > "$fixture/restart-out" 2>&1; then fail "restart: a refused relaunch exits nonzero"; else
    pass "restart: a refused relaunch exits nonzero"
fi
LIFECYCLE_STATUS=0
rm -f "$fixture/lifecycle-calls"
if (restart_keeper nodeZ) > "$fixture/restart-out" 2>&1; then fail "restart: refuses a host without a conf"; else
    grep -q "nodeZ" "$fixture/restart-out" && pass "restart: refuses a host without a conf" || fail "restart: refuses a host without a conf"
fi
down=nodeB
if (restart_keeper nodeB) > /dev/null 2>&1; then fail "restart: refuses an unreachable host"; else pass "restart: refuses an unreachable host"; fi
[[ ! -e "$fixture/lifecycle-calls" ]] && pass "restart: nothing relaunched for a refused host" ||
    fail "restart: nothing relaunched for a refused host: $(cat "$fixture/lifecycle-calls")"

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
launch_hosts() { echo "launch $(basename "$2") from $1" >> "$fixture/start-launches"; }
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
CATALOG_DIR=""
parse_args --start --catalog-dir /x/y > /dev/null
[[ "$CATALOG_DIR" == /x/y ]] && pass "args: --catalog-dir sets the catalog directory" || fail "args: --catalog-dir sets the catalog directory"
reset_args
(parse_args --start --catalog-dir "") > "$fixture/args-out" 2>&1 && fail "args: an empty --catalog-dir is refused" ||
    { grep -q -- "--catalog-dir needs a directory" "$fixture/args-out" && pass "args: an empty --catalog-dir is refused"; } ||
    fail "args: an empty --catalog-dir is refused: $(cat "$fixture/args-out")"
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
    wal=false WAL_DIR_LIST="" CATALOG_DIR="" RESTART_KEEPER_HOST="" JOB_ID="" verbose=false NUM_RECORDING_GROUP=1
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

[[ "$(delegated --start --start --catalog-dir /var/local/chronolog)" == *"--catalog-dir /var/local/chronolog "* ]] &&
    pass "single_user_deploy: --catalog-dir passed on start" || fail "single_user_deploy: --catalog-dir passed on start: $(delegated --start --start --catalog-dir /var/local/chronolog)"
[[ "$(delegated --clean --clean --catalog-dir /var/local/chronolog)" == *"--catalog-dir /var/local/chronolog "* ]] &&
    pass "single_user_deploy: --catalog-dir passed on clean" || fail "single_user_deploy: --catalog-dir passed on clean"
[[ "$(delegated --stop --stop --catalog-dir /var/local/chronolog)" != *"--catalog-dir"* ]] &&
    pass "single_user_deploy: --catalog-dir not passed on stop" || fail "single_user_deploy: --catalog-dir not passed on stop"
(delegated --start --start --catalog-dir "") > /dev/null 2>&1 && fail "single_user_deploy: an empty --catalog-dir is refused" ||
    pass "single_user_deploy: an empty --catalog-dir is refused"

echo "$failures deploy_cluster WAL checks failed"
test "$failures" -eq 0
