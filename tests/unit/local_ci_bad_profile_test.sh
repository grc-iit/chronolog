#!/usr/bin/env bash
# Checks that docker/local_distributed_ci.sh refuses a bad --nfs-profile
# before it touches anything: no sudo (the NFS export), no docker exec, no
# "docker compose down -v". A cluster left up by an earlier run with --keep has
# the same container names and volume, and a teardown would delete them.
# docker and sudo are stand-ins that only log their arguments.
set -uo pipefail
repo_root=${1:?repository root required}
dir=$(mktemp -d)
trap 'rm -rf -- "$dir"' EXIT
mkdir -p "$dir/bin"
for tool in docker sudo; do
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\n' "$tool" "$dir" > "$dir/bin/$tool"
    chmod +x "$dir/bin/$tool"
done
# sudo exportfs -v answers as if the export were up, so a run that reaches the
# NFS setup goes on to stage 1
echo '[ "$1 $2" = "exportfs -v" ] && echo "/srv/chronolog-nfs-test 127.0.0.1(rw)"' >> "$dir/bin/sudo"
echo 'exit 0' >> "$dir/bin/docker"
echo 'exit 0' >> "$dir/bin/sudo"
: > "$dir/calls"
# a copy of the repository's scripts, so the compose files the script writes
# next to it land in the temporary directory
mkdir -p "$dir/repo/docker" "$dir/repo/tests/distributed"
cp "$repo_root/docker/local_distributed_ci.sh" "$dir/repo/docker/"
cp -r "$repo_root/tests/distributed/nfs" "$dir/repo/tests/distributed/"
: > "$dir/repo/dynamic-compose.local-ci.yaml.kept"

failures=0
check() { local m=$1; shift; if "$@"; then echo "PASS: $m"; else echo "FAIL: $m" >&2; failures=$((failures + 1)); fi; }

PATH="$dir/bin:$PATH" timeout 60 "$dir/repo/docker/local_distributed_ci.sh" -i test-image --nfs \
    --nfs-export /srv/chronolog-nfs-test --nfs-profile Bad= > "$dir/out" 2>&1
rc=$?
check "exits non-zero" test "$rc" -ne 0
check "names the bad profile" grep -q "Bad" "$dir/out"
check "never runs sudo" bash -c "! grep -q '^sudo ' \"\$1\"" _ "$dir/calls"
check "never runs docker exec" bash -c "! grep -q '^docker exec' \"\$1\"" _ "$dir/calls"
check "never runs docker compose down" bash -c "! grep -q '^docker compose.* down' \"\$1\"" _ "$dir/calls"
check "never runs docker compose up" bash -c "! grep -q '^docker compose.* up' \"\$1\"" _ "$dir/calls"
if [ "$failures" -ne 0 ]; then
    echo "--- script output"; cat "$dir/out"; echo "--- calls"; cat "$dir/calls"
fi
echo "$failures local CI bad-profile checks failed"
test "$failures" -eq 0
