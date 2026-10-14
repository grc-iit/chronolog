#!/usr/bin/env bash
# Checks what nfs_server.sh would run, without sudo: the export goes to
# 127.0.0.1 only, delegations are off while it is up, and teardown removes
# only what setup added.
set -uo pipefail
repo_root=${1:?repository root required}
srv="$repo_root/tests/distributed/nfs/nfs_server.sh"
failures=0
check() { local m=$1; shift; if "$@"; then echo "PASS: $m"; else echo "FAIL: $m" >&2; failures=$((failures + 1)); fi; }

out=$("$srv" setup --export /srv/chronolog-nfs --dry-run)
check "exports to 127.0.0.1 only" grep -q "/srv/chronolog-nfs 127.0.0.1(rw,sync,no_subtree_check)" <<< "$out"
check "never exports to every host" bash -c "! grep -q '\*(' <<< \"\$1\"" _ "$out"
check "writes its own exports file" grep -q "/etc/exports.d/chronolog-ci.exports" <<< "$out"
check "re-exports" grep -q "exportfs -ra" <<< "$out"
check "updates the package lists before installing" \
    bash -c "grep -n . <<< \"\$1\" | awk -F: '/apt-get update/ { u = \$1 } /apt-get install/ { i = \$1 } END { exit !(u && i > u) }'" _ "$out"
check "saves the lease setting" grep -q "sysctl -n fs.leases-enable > /run/chronolog-ci-nfs.leases-enable" <<< "$out"
check "turns leases off" grep -q "sysctl -w fs.leases-enable=0" <<< "$out"
check "restarts the server after turning leases off" \
    bash -c "grep -n . <<< \"\$1\" | awk -F: '/leases-enable=0/ { l = \$1 } /systemctl restart nfs-server/ { r = \$1 } END { exit !(l && r > l) }'" _ "$out"
out=$("$srv" teardown --export /srv/chronolog-nfs --dry-run)
check "teardown removes its exports file" grep -q "rm -f /etc/exports.d/chronolog-ci.exports" <<< "$out"
check "teardown restores the lease setting" grep -q "/run/chronolog-ci-nfs.leases-enable" <<< "$out"
check "teardown restores the lease setting before re-exporting, which can fail" \
    bash -c "grep -n . <<< \"\$1\" | awk -F: '/leases-enable/ { l = \$1 } /exportfs -ra/ { e = \$1 } END { exit !(l && e > l) }'" _ "$out"
check "teardown keeps the server running" bash -c "! grep -qE 'systemctl (stop|disable)' <<< \"\$1\"" _ "$out"
check "teardown keeps the directory" bash -c "! grep -q 'rm -rf' <<< \"\$1\"" _ "$out"
check "refuses a relative export" bash -c "! \"\$1\" setup --export srv --dry-run > /dev/null 2>&1" _ "$srv"
check "refuses an unknown action" bash -c "! \"\$1\" restart --export /srv/x --dry-run > /dev/null 2>&1" _ "$srv"

echo "$failures NFS server checks failed"
test "$failures" -eq 0
