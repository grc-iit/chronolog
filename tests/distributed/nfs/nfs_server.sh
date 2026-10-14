#!/usr/bin/env bash
# Exports a directory over NFS from this host for the NFS pass (#705), to
# 127.0.0.1 only: Docker's local volume driver mounts it on the host, so no
# other address needs it. Needs sudo.
#
# setup also turns off file leases (fs.leases-enable=0), which turns off NFS
# delegations, and restarts the server so none granted earlier survive. Every
# container mounts through the host's kernel, so the server sees one client:
# a delegation held by one container's mount is never recalled when another
# container writes, and the holder keeps reading its own stale copy, which
# separate hosts would not do. teardown removes the export and restores the
# lease setting, but leaves the NFS server running and the directory in
# place.
#
# Usage: nfs_server.sh setup|teardown --export DIR [--dry-run]
set -euo pipefail

die() {
    echo "nfs_server.sh: $*" >&2
    exit 1
}
EXPORTS_FILE=/etc/exports.d/chronolog-ci.exports
LEASES_FILE=/run/chronolog-ci-nfs.leases-enable

action=${1-}
shift || true
export_dir="" dry_run=0
while [ $# -gt 0 ]; do
    case "$1" in
        --export) export_dir=${2-}; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        *) die "unknown option: $1" ;;
    esac
done
[[ "$export_dir" == /* ]] || die "--export must be an absolute path"

run() {
    if [ "$dry_run" = 1 ]; then echo "+ $*"; else sudo "$@"; fi
}

case "$action" in
    setup)
        if [ "$dry_run" = 1 ] || ! command -v exportfs > /dev/null; then
            run apt-get update
            run apt-get install -y nfs-kernel-server nfs-common
        fi
        run mkdir -p "$export_dir"
        run chmod 1777 "$export_dir"
        run mkdir -p /etc/exports.d
        run sh -c "echo '$export_dir 127.0.0.1(rw,sync,no_subtree_check)' > $EXPORTS_FILE"
        # keep the value from before the first setup, not one an earlier setup left
        if [ "$dry_run" = 1 ] || [ ! -e "$LEASES_FILE" ]; then
            run sh -c "sysctl -n fs.leases-enable > $LEASES_FILE"
        fi
        run sysctl -w fs.leases-enable=0
        run systemctl restart nfs-server
        run exportfs -ra
        # exportfs -v starts each export with its path; a prefix match would
        # also accept another export whose path starts with this one
        [ "$dry_run" = 1 ] || sudo exportfs -v | awk -v d="$export_dir" '$1 == d { f = 1 } END { exit !f }' ||
            die "$export_dir is not exported"
        ;;
    teardown)
        run rm -f "$EXPORTS_FILE"
        # restore the lease setting first: exportfs -ra fails on any broken
        # line in /etc/exports or /etc/exports.d, and set -e then exits
        if [ "$dry_run" = 1 ] || [ -e "$LEASES_FILE" ]; then
            run sh -c "sysctl -w fs.leases-enable=\$(cat $LEASES_FILE) && rm -f $LEASES_FILE"
        fi
        run exportfs -ra
        ;;
    *) die "action must be setup or teardown" ;;
esac
