#!/usr/bin/env bash
# Prints a docker compose override that mounts one NFS export in every
# container once per mount profile, at <mount root>/<profile>. A profile is
# name=options; every mount also gets nosharecache, so each mount keeps a
# cache of its own, and vers=4.2 unless the profile names a version. Docker's
# local volume driver makes the mount on the host when the container is
# created, so the containers need no extra privileges.
#
# Usage: nfs_compose_override.sh --containers N --server ADDR --export DIR
#            --profile name=options [--profile ...] [--mount-root DIR]
set -euo pipefail

die() {
    echo "nfs_compose_override.sh: $*" >&2
    exit 1
}

containers="" server="" export_dir="" mount_root=/mnt/nfs
profiles=()
while [ $# -gt 0 ]; do
    case "$1" in
        --containers) containers=${2-}; shift 2 ;;
        --server) server=${2-}; shift 2 ;;
        --export) export_dir=${2-}; shift 2 ;;
        --mount-root) mount_root=${2-}; shift 2 ;;
        --profile) profiles+=("${2-}"); shift 2 ;;
        *) die "unknown option: $1" ;;
    esac
done

[[ "$containers" =~ ^[1-9][0-9]*$ ]] || die "--containers must be a positive number"
[ -n "$server" ] || die "--server is required"
[[ "$export_dir" == /* ]] || die "--export must be an absolute path"
[[ "$mount_root" == /* ]] || die "--mount-root must be an absolute path"
[ ${#profiles[@]} -gt 0 ] || die "give at least one --profile name=options"

names=() options=()
for profile in "${profiles[@]}"; do
    [[ "$profile" == *=* ]] || die "profile '$profile' is not name=options"
    name=${profile%%=*}
    opts=${profile#*=}
    [[ "$name" =~ ^[a-z0-9][a-z0-9_-]*$ ]] || die "profile name '$name': use lower-case letters, digits, - and _"
    for seen in "${names[@]}"; do
        [ "$seen" != "$name" ] || die "profile '$name' is given twice"
    done
    [[ "$opts" =~ ^[A-Za-z0-9_=,.:/-]*$ ]] || die "profile '$name': options may hold only letters, digits and _=,.:/-"
    [[ ",$opts," != *,sharecache,* ]] || die "profile '$name': sharecache would make the mounts share one cache"
    full="addr=${server},nosharecache"
    [[ ",$opts," =~ ,(vers|nfsvers)= ]] || full+=",vers=4.2"
    [ -z "$opts" ] || full+=",$opts"
    names+=("$name")
    options+=("$full")
done

echo "services:"
for i in $(seq 1 "$containers"); do
    echo "  c$i:"
    echo "    volumes:"
    for name in "${names[@]}"; do
        echo "      - nfs_${name}_c$i:${mount_root}/${name}"
    done
done
echo ""
echo "volumes:"
for i in $(seq 1 "$containers"); do
    for k in "${!names[@]}"; do
        echo "  nfs_${names[$k]}_c$i:"
        echo "    driver: local"
        echo "    driver_opts:"
        echo "      type: nfs"
        echo "      o: \"${options[$k]}\""
        echo "      device: \":${export_dir}\""
    done
done
