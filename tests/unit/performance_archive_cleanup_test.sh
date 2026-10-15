#!/usr/bin/env bash
set -euo pipefail
repo_root=${1:?repository root required}
fixture=$(mktemp -d /tmp/chronolog-performance-cleanup-test.XXXXXX)
trap 'rm -rf -- "$fixture"' EXIT

# Run the remote command locally; the cleanup itself is the production code.
mpssh() { shift 2; bash -c "$1"; }
export -f mpssh
run_shell() { bash -c "$2"; }
log() { echo "$*"; }
section() { echo "$*"; }
source <(sed -n '/^clean_output_dir() {/,/^}/p' "$repo_root/tests/performance/ares_test.sh")

OUTPUT_DIR="$fixture/archive"
DEPLOY_SCRIPT="$repo_root/tools/deploy/deploy_cluster.sh"
VISOR_HOSTS="$fixture/hosts"
KEEPER_HOSTS="$fixture/missing-keepers"
GRAPHER_HOSTS="$fixture/missing-graphers"
PLAYER_HOSTS="$fixture/missing-players"
touch "$VISOR_HOSTS"
mkdir -p "$OUTPUT_DIR/C/S" "$OUTPUT_DIR/%manifest" "$OUTPUT_DIR/other-app/cache" "$OUTPUT_DIR/other-empty/child"
touch "$OUTPUT_DIR/C/S/60.1.1000.1.0.vlen.h5" "$OUTPUT_DIR/other-app/cache/data.vlen.h5"
printf '%s\n' '{"op":"publish","chronicle":"C","story":"S","file":"C/S/60.1.1000.1.0.vlen.h5","start":1,"end":2}' > "$OUTPUT_DIR/%manifest/1.log"
clean_output_dir

failures=0
for kept in "$OUTPUT_DIR/other-app/cache/data.vlen.h5" "$OUTPUT_DIR/other-empty/child"; do
    if [[ ! -e "$kept" ]]; then
        echo "FAIL: unrelated path deleted: $kept" >&2
        failures=$((failures + 1))
    fi
done
for removed in "$OUTPUT_DIR/C/S/60.1.1000.1.0.vlen.h5" "$OUTPUT_DIR/%manifest"; do
    if [[ -e "$removed" ]]; then
        echo "FAIL: owned archive path remains: $removed" >&2
        failures=$((failures + 1))
    fi
done
echo "$failures performance cleanup checks failed"
test "$failures" -eq 0
