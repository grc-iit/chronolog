#!/usr/bin/env bash
# NFS pass of the distributed pipeline (#705), run as grc-iit in c1 after the
# first pass. For every --pass profile:expect[:warn|nowarn] it redeploys
# ChronoLog with the archive on <mount root>/<profile>, which every container
# mounts with that profile's NFS options, and runs the visibility test, plus
# the destroy test on the destroy profile when the pass expects clean.
#   expect   clean (every replay complete) or omission (the negative control:
#            a replay misses events yet reports success; the last is complete)
#   warn     deploy_cluster.sh must warn about the archive mount; nowarn: it
#            must not; left out: not checked
#
# Usage: nfs_pass.sh --work-dir W --repo-dir R --probe P --pass SPEC [--pass SPEC ...]
#            [--destroy-profile NAME|none] [--mount-root /mnt/nfs] [--cache-secs 60]
#            [--log-dir /tmp/nfs-pass]
# Exit 0 when every pass met its expectations.
set -uo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/nfs_common.sh"
usage() {
    sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

WORK_DIR="" REPO_DIR="" PROBE="" MOUNT_ROOT=/mnt/nfs DESTROY_PROFILE=default CACHE_SECS=60
LOG_DIR=/tmp/nfs-pass
passes=()
while [ $# -gt 0 ]; do
    case "$1" in
        --work-dir) WORK_DIR=${2-}; shift 2 ;;
        --repo-dir) REPO_DIR=${2-}; shift 2 ;;
        --probe) PROBE=${2-}; shift 2 ;;
        --pass) passes+=("${2-}"); shift 2 ;;
        --destroy-profile) DESTROY_PROFILE=${2-}; shift 2 ;;
        --mount-root) MOUNT_ROOT=${2-}; shift 2 ;;
        --cache-secs) CACHE_SECS=${2-}; shift 2 ;;
        --log-dir) LOG_DIR=${2-}; shift 2 ;;
        *) usage ;;
    esac
done
for v in WORK_DIR REPO_DIR PROBE; do [ -n "${!v}" ] || usage; done
[ ${#passes[@]} -gt 0 ] || usage
[[ "$CACHE_SECS" =~ ^[0-9]+$ ]] || usage
[ -x "$PROBE" ] || {
    echo "probe not found: $PROBE" >&2
    exit 2
}
# a probe built before the destroy mode prints only its usage for it, which
# the destroy test would report as a failed destroy
if [ "$DESTROY_PROFILE" != none ] && ! grep -qa 'write|replay|destroy' "$PROBE"; then
    echo "$PROBE has no destroy mode: it was built from older sources; rebuild it" >&2
    exit 2
fi
for spec in "${passes[@]}"; do
    [[ "$spec" =~ ^[a-z0-9][a-z0-9_-]*:(clean|omission)(:(warn|nowarn))?$ ]] || {
        echo "bad --pass '$spec': use profile:clean|omission[:warn|nowarn]" >&2
        exit 2
    }
done
mkdir -p "$LOG_DIR"
HOSTS_ALL="$WORK_DIR/conf/hosts_all"
[ -s "$HOSTS_ALL" ] || {
    echo "no hosts in $HOSTS_ALL" >&2
    exit 2
}
mapfile -t all_hosts < <(grep . "$HOSTS_ALL")
# clients run outside c1 when there are other containers
mapfile -t others < <(grep -vx c1 "$HOSTS_ALL" | grep .)
[ ${#others[@]} -gt 0 ] || others=(c1)
pick() { echo "${others[$(($1 % ${#others[@]}))]}"; }
VW=$(pick 0) VR=$(pick 1) DW=$(pick 2) DR=$(pick 3)

spack_env() {
    cd "$REPO_DIR" && source /home/grc-iit/spack/share/spack/setup-env.sh &&
        { spack env activate -d . 2> /dev/null || spack env activate -p . 2> /dev/null || true; }
}
stop_all() {
    (
        spack_env
        ./tools/deploy/single_user_deploy.sh -s -w "$WORK_DIR"
    ) > /dev/null 2>&1 || true
    local h
    for h in "${all_hosts[@]}"; do
        run_on "$h" 'pkill -9 -f "chrono-(visor|keeper|grapher|player) --config"' > /dev/null 2>&1 || true
    done
    sleep 2
}
# save_daemon_logs N: keeps pass N's daemon logs, which the next pass's
# "single_user_deploy.sh -c" deletes
save_daemon_logs() {
    mkdir -p "$LOG_DIR/pass-$1-monitor"
    cp "$WORK_DIR"/monitor/*.log "$LOG_DIR/pass-$1-monitor/" 2> /dev/null || true
}

results=() failed=0 i=0
for spec in "${passes[@]}"; do
    i=$((i + 1))
    IFS=: read -r profile expect warn <<< "$spec"
    echo "════ NFS pass $i: profile=$profile expect=$expect warning=${warn:--} ════"
    stop_all
    (
        spack_env
        ./tools/deploy/single_user_deploy.sh -c -w "$WORK_DIR"
    ) > /dev/null 2>&1 || true

    not_mounted=""
    for h in "${all_hosts[@]}"; do
        run_on "$h" "mountpoint -q '$MOUNT_ROOT/$profile'" > /dev/null 2>&1 || not_mounted+="$h "
    done
    if [ -n "$not_mounted" ]; then
        echo "❌ $MOUNT_ROOT/$profile is not a mount point on: $not_mounted"
        results+=("PASS $i $profile expect=$expect not-mounted-on=${not_mounted% }")
        failed=1
        continue
    fi

    archive="$MOUNT_ROOT/$profile/pass-$i-$(date +%s)/output"
    mkdir -p "$archive"
    (
        spack_env
        ./tools/deploy/single_user_deploy.sh -d -w "$WORK_DIR" -u "$archive"
    ) > "$LOG_DIR/pass-$i-deploy.log" 2>&1
    deploy_rc=$?
    if [ "$deploy_rc" -ne 0 ]; then
        echo "❌ deploy failed ($deploy_rc); last lines of $LOG_DIR/pass-$i-deploy.log:"
        tail -20 "$LOG_DIR/pass-$i-deploy.log"
        results+=("PASS $i $profile expect=$expect deploy-failed")
        failed=1
        stop_all
        save_daemon_logs "$i"
        continue
    fi
    sleep 15
    warnings=$(grep -cE 'mounts .* over NFS with' "$LOG_DIR/pass-$i-deploy.log" || true)
    grep -E 'mounts .* over NFS with' "$LOG_DIR/pass-$i-deploy.log" || true
    case "${warn:-}" in
        warn) [ "$warnings" -gt 0 ] && wres=ok || wres=bad ;;
        nowarn) [ "$warnings" -eq 0 ] && wres=ok || wres=bad ;;
        *) wres=- ;;
    esac

    common=(--work-dir "$WORK_DIR" --probe "$PROBE" --cache-secs "$CACHE_SECS")
    vis=("$here/nfs_visibility_test.sh" "${common[@]}" --writer "$VW" --reader "$VR"
        --chronicle "nfsvis$i" --story s --expect "$expect" --log-dir "$LOG_DIR/pass-$i-vis")
    des=("$here/nfs_destroy_test.sh" "${common[@]}" --writer "$DW" --reader "$DR" --archive "$archive"
        --chronicle "nfsdes$i" --story s --log-dir "$LOG_DIR/pass-$i-des")
    dres=-
    if [ "$profile" = "$DESTROY_PROFILE" ] && [ "$expect" = clean ]; then
        # two probes on one host collide on the client port: side by side
        # only when the four hosts differ
        if [ "$(printf '%s\n' "$VW" "$VR" "$DW" "$DR" | sort -u | wc -l)" -eq 4 ]; then
            "${des[@]}" > "$LOG_DIR/pass-$i-destroy.out" 2>&1 &
            des_pid=$!
            "${vis[@]}" | tee "$LOG_DIR/pass-$i-visibility.out"
            vrc=${PIPESTATUS[0]}
            wait "$des_pid"
            drc=$?
        else
            "${vis[@]}" | tee "$LOG_DIR/pass-$i-visibility.out"
            vrc=${PIPESTATUS[0]}
            "${des[@]}" > "$LOG_DIR/pass-$i-destroy.out" 2>&1
            drc=$?
        fi
        cat "$LOG_DIR/pass-$i-destroy.out"
        [ "$drc" -eq 0 ] && dres=passed || dres=failed
    else
        "${vis[@]}" | tee "$LOG_DIR/pass-$i-visibility.out"
        vrc=${PIPESTATUS[0]}
    fi
    vres=$(tail -1 "$LOG_DIR/pass-$i-visibility.out" | awk '$1 == "VISIBILITY" { print $2 }')

    hits=$(cat "$WORK_DIR"/monitor/chrono-grapher*.log "$WORK_DIR"/monitor/chrono-player*.log 2> /dev/null |
        grep -E 'Stale file handle|ESTALE|unable to open file|H5Fopen failed' || true)
    if [ -n "$hits" ]; then
        echo "log lines about stale handles or HDF5 open failures:"
        head -20 <<< "$hits"
        [ "$expect" = clean ] && lres=bad || lres=reported
    else
        lres=ok
    fi

    stop_all
    save_daemon_logs "$i"
    bash "$REPO_DIR/tools/deploy/archive_cleanup.sh" "$archive" || true
    rmdir "$archive" "$(dirname "$archive")" 2> /dev/null || true
    results+=("PASS $i $profile expect=$expect deploy-warning=$wres visibility=${vres:-none} destroy=$dres logs=$lres")
    if [ "$vrc" -ne 0 ] || [ "$wres" = bad ] || [ "$dres" = failed ] || [ "$lres" = bad ]; then failed=1; fi
done

echo ""
echo "════ NFS pass summary (logs in $LOG_DIR) ════"
printf '%s\n' "${results[@]}"
exit "$failed"
