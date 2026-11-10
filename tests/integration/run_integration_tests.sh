#!/bin/bash
# Runs the integration scripts in this directory one at a time and writes a
# summary of their results.
#
# Each script deploys its own daemons on this host, so they cannot run side by
# side. Between scripts the runner stops any daemon left running from
# WORK_DIR's bin/ and puts back the installed conf template if a script left
# it patched. That happens when a script is killed by the time limit before
# its own cleanup ran.
#
# For each script it keeps, under LOG_DIR:
#   <script>.log      everything the script printed
#   <script>/monitor  the daemons' logs from WORK_DIR/monitor after the script
#
# and writes LOG_DIR/summary.md, a Markdown table with one row per script:
# pass, fail (a check failed, exit 1), error (the script could not run its
# checks, any other exit) or timeout, the "N passed, M failed" line the script
# printed last, and the time it took. When GITHUB_STEP_SUMMARY is set the
# table is appended to it as well.
#
# Usage: WORK_DIR=~/chronolog-install/chronolog BUILD_DIR=~/chronolog-build/Debug \
#        [LOG_DIR=<dir>] [SCRIPT_TIMEOUT=<seconds>] \
#        tests/integration/run_integration_tests.sh [script ...]
#
# With no arguments every *_test.sh in this directory runs, in name order. A
# script is named by its file name, with or without .sh. WORK_DIR and
# BUILD_DIR are passed on to the scripts, and so is every other variable in
# the environment, such as PROTOCOLS for keeper_restart_held_test.sh.
#
# Exits 0 when every script passed, 1 otherwise, and 2 for a bad argument.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export WORK_DIR="${WORK_DIR:-$HOME/chronolog-install/chronolog}"
export BUILD_DIR="${BUILD_DIR:-$HOME/chronolog-build/Debug}"
LOG_DIR="${LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/chronolog-integration.XXXXXX")}"
SCRIPT_TIMEOUT="${SCRIPT_TIMEOUT:-1800}"
CONF_TEMPLATE="$WORK_DIR/conf/default-chrono-conf.json"

say() { echo "[run_integration_tests $(date +%H:%M:%S)] $*"; }

scripts=()
if [ "$#" -eq 0 ]; then
    for f in "$HERE"/*_test.sh; do
        [ -f "$f" ] && scripts+=("$(basename "$f")")
    done
else
    for name in "$@"; do
        name="${name%.sh}.sh"
        if [ ! -f "$HERE/$name" ]; then
            say "no script $name in $HERE"
            exit 2
        fi
        scripts+=("$name")
    done
fi
if [ "${#scripts[@]}" -eq 0 ]; then
    say "no *_test.sh in $HERE"
    exit 2
fi
if [ ! -f "$CONF_TEMPLATE" ]; then
    say "no conf template at $CONF_TEMPLATE; is WORK_DIR an installed tree?"
    exit 2
fi

mkdir -p "$LOG_DIR"
command cp -f "$CONF_TEMPLATE" "$LOG_DIR/conf-template.orig"

# Daemons started from this install only: another install's daemons on the
# same host are not this run's to stop.
stop_daemons() {
    local bin
    for bin in chrono-visor chrono-keeper chrono-grapher chrono-player; do
        pkill -9 -f "$WORK_DIR/bin/$bin --config" 2> /dev/null
    done
    sleep 2
}

# Puts the template back and removes the backups a script makes of it.
# Prints what it did, or nothing when the script left the template as it was.
restore_template() {
    local leftover
    leftover=$(find "$WORK_DIR/conf" -maxdepth 1 -name 'default-chrono-conf.json?*' -print 2> /dev/null)
    if ! cmp -s "$LOG_DIR/conf-template.orig" "$CONF_TEMPLATE" || [ -n "$leftover" ]; then
        command cp -f "$LOG_DIR/conf-template.orig" "$CONF_TEMPLATE"
        [ -n "$leftover" ] && rm -f $leftover
        echo "conf template restored"
    fi
}

rows=()
failed=0
for script in "${scripts[@]}"; do
    name="${script%.sh}"
    stop_daemons
    say "running $script (limit ${SCRIPT_TIMEOUT}s)"
    start=$SECONDS
    timeout --kill-after=60 "$SCRIPT_TIMEOUT" bash "$HERE/$script" 2>&1 | tee "$LOG_DIR/$name.log"
    status=${PIPESTATUS[0]}
    took=$((SECONDS - start))
    case "$status" in
        0) result=pass ;;
        1) result=fail ;;
        124 | 137) result=timeout ;;
        *) result=error ;;
    esac
    checks=$(grep -oE '[0-9]+ passed, [0-9]+ failed' "$LOG_DIR/$name.log" | tail -1)
    stop_daemons
    note=$(restore_template)
    mkdir -p "$LOG_DIR/$name/monitor"
    find "$WORK_DIR/monitor" -maxdepth 1 -type f -exec cp -f {} "$LOG_DIR/$name/monitor/" \; 2> /dev/null
    [ "$result" = pass ] || failed=$((failed + 1))
    rows+=("| \`$script\` | $result | ${checks:-none printed} | $((took / 60))m $((took % 60))s | ${note:-} |")
    say "$script: $result (exit $status, ${checks:-no result line}, ${took}s)${note:+, $note}"
done

{
    echo "| Script | Result | Checks | Time | Note |"
    echo "|---|---|---|---:|---|"
    printf '%s\n' "${rows[@]}"
    echo
    echo "$((${#scripts[@]} - failed)) of ${#scripts[@]} scripts passed. Logs: \`$LOG_DIR\`."
} > "$LOG_DIR/summary.md"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    cat "$LOG_DIR/summary.md" >> "$GITHUB_STEP_SUMMARY"
fi
cat "$LOG_DIR/summary.md"

[ "$failed" -eq 0 ]
