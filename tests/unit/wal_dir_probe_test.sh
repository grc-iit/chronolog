#!/usr/bin/env bash
# Checks tools/deploy/wal_dir_probe.sh against made-up hosts: PATH shims for
# lsblk, findmnt, df and stat answer from a mount table and a disk table
# written by each case, while the <mount>/$USER directories are real ones
# under the fixture. Also checks that deploy_cluster.sh writes the probe's
# choice into a keeper's conf.
set -uo pipefail
repo_root=${1:?repository root required}
probe="$repo_root/tools/deploy/wal_dir_probe.sh"
fixture=$(mktemp -d /tmp/chronolog-wal-probe-test.XXXXXX)
trap 'rm -rf -- "$fixture"' EXIT
failures=0
pass() { echo "PASS: $1"; }
fail() {
    echo "FAIL: $1" >&2
    failures=$((failures + 1))
}

# Mount table: TARGET SOURCE FSTYPE AVAIL SIZE DEV. Disk table: NAME PKNAME TRAN ROTA, "-" for empty.
mounts="$fixture/mounts"
disks="$fixture/disks"
mkdir -p "$fixture/bin"
cat > "$fixture/bin/shim" << 'EOF'
#!/usr/bin/env bash
mounts=${PROBE_TEST_MOUNTS:?}
disks=${PROBE_TEST_DISKS:?}
# Prints the mount table line of the mount holding $1: the longest target
# that is $1 or one of its parent directories.
mount_of() {
    local path=$1 best="" best_len=-1 target rest
    while read -r target rest; do
        if [[ "$target" == / || "$path" == "$target" || "$path" == "$target"/* ]]; then
            if ((${#target} > best_len)); then
                best="$target $rest"
                best_len=${#target}
            fi
        fi
    done < "$mounts"
    [[ -n "$best" ]] && echo "$best"
}
dash() { [[ "$1" == - ]] && echo "" || echo "$1"; }
case "$(basename "$0")" in
findmnt)
    target_path=""
    while [[ $# -gt 0 ]]; do
        [[ "$1" == --target ]] && target_path=$2 && shift
        shift
    done
    if [[ -n "$target_path" ]]; then
        line=$(mount_of "$target_path") || exit 1
        read -r t s f _ <<< "$line"
        echo "$t $s $f"
    else
        while read -r t s f _; do echo "$t $s $f"; done < "$mounts"
    fi
    ;;
lsblk)
    column=${2:?}
    name=${3#/dev/}
    while read -r n pk tran rota; do
        [[ "$n" == "$name" ]] || continue
        case "$column" in
        PKNAME) dash "$pk" ;;
        TRAN) dash "$tran" ;;
        ROTA) echo "   $rota" ;;
        esac
        exit 0
    done < "$disks"
    exit 32
    ;;
df)
    line=$(mount_of "${!#}") || exit 1
    read -r _ _ _ avail size _ <<< "$line"
    echo "   Avail     Size"
    echo " $avail $size"
    ;;
stat)
    line=$(mount_of "${!#}") || exit 1
    read -r _ _ _ _ _ dev <<< "$line"
    echo "$dev"
    ;;
esac
EOF
chmod +x "$fixture/bin/shim"
for tool in findmnt lsblk df stat; do ln -s shim "$fixture/bin/$tool"; done
export PROBE_TEST_MOUNTS="$mounts" PROBE_TEST_DISKS="$disks"
export USER=walprobeuser
TEST_PATH="$fixture/bin:$PATH"

G=1000000000 # 1 GB
BIG=$((100 * G))
H=0123456789ab
F="$fixture/host"
STD_ARGS=(--work-dir-hash "$H" --group 2 --max-bytes $G --reserve-min 268435456 --reserve-percent 10)

# The root file system is sda2 on a SATA SSD; every case adds its own mounts.
reset_host() {
    rm -rf -- "$F"
    mkdir -p "$F/tmp"
    printf '%s\n' "/ /dev/sda2 ext4 $BIG $((4 * BIG)) 2050" > "$mounts"
    printf '%s\n' "sda - sata 0" "sda2 sda - 0" > "$disks"
}
# add_mount NAME SOURCE FSTYPE AVAIL SIZE DEV [nouser]: a mount at $F/mnt/NAME with a $USER directory
add_mount() {
    mkdir -p "$F/mnt/$1"
    [[ "${7:-}" == nouser ]] || mkdir -p "$F/mnt/$1/$USER"
    echo "$F/mnt/$1 $2 $3 $4 $5 $6" >> "$mounts"
}
add_disk() { echo "$1 $2 $3 $4" >> "$disks"; }
nvme_disk() { add_disk nvme0n1 - nvme 0 && add_disk nvme0n1p1 nvme0n1 - 0; }
ssd_disk() { add_disk sdb - sata 0 && add_disk sdb1 sdb - 0; }
hdd_disk() { add_disk sdc - sata 1 && add_disk sdc1 sdc - 1; }

run_probe() { # prints the probe's stdout; its exit status is the probe's
    PATH="$TEST_PATH" TMPDIR="$F/tmp" bash "$probe" "$@" 2> "$fixture/stderr"
}
# expect LABEL WANT_DIR WANT_WARNINGS [probe args...]: WANT_WARNINGS is a
# count, or a pattern that must match one warning when it is not a number
expect() {
    local label=$1 want_dir=$2 want_warnings=$3
    shift 3
    local out
    last_output=""
    if ! out=$(run_probe "$@"); then
        fail "$label: probe failed: $(cat "$fixture/stderr")"
        return
    fi
    local got_dir got_warnings
    got_dir=$(sed -n 's/^WAL_DIR=//p' <<< "$out")
    got_warnings=$(grep -c '^WARNING: ' <<< "$out")
    if [[ "$got_dir" != "$want_dir" ]]; then
        fail "$label: WAL_DIR '$got_dir', expected '$want_dir'"
    elif [[ "$want_warnings" =~ ^[0-9]+$ && "$got_warnings" -ne "$want_warnings" ]]; then
        fail "$label: $got_warnings warnings, expected $want_warnings: $out"
    elif [[ ! "$want_warnings" =~ ^[0-9]+$ ]] && ! grep -q -- "^WARNING: .*$want_warnings" <<< "$out"; then
        fail "$label: no warning matching '$want_warnings': $out"
    elif [[ "$(grep -vc '^WARNING: \|^WAL_DIR=' <<< "$out")" -ne 0 ]]; then
        fail "$label: unexpected output: $out"
    elif [[ ! -d "$got_dir" ]]; then
        fail "$label: $got_dir was not created"
    else
        pass "$label"
    fi
    last_output=$out
}

# Ranking
reset_host
ssd_disk
nvme_disk
add_mount ssd /dev/sdb1 ext4 $BIG $BIG 2065
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
expect "NVMe chosen over a SATA SSD listed first" "$F/mnt/nvme/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
ssd_disk
nvme_disk
# 1.2 GB free on 2 GB: 1.2 GB minus the 268 MB reserve is below max_bytes
add_mount nvme /dev/nvme0n1p1 xfs $((12 * G / 10)) $((2 * G)) 66305
add_mount ssd /dev/sdb1 ext4 $BIG $BIG 2065
expect "nearly full NVMe skipped for the SSD" "$F/mnt/ssd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
nvme_disk
# 20 GB free of 200 GB: the 10 % reserve (20 GB) leaves nothing for the WAL
add_mount nvme /dev/nvme0n1p1 xfs $((20 * G)) $((200 * G)) 66305
ssd_disk
add_mount ssd /dev/sdb1 ext4 $BIG $BIG 2065
expect "reserve_percent counts against free space" "$F/mnt/ssd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
hdd_disk
ssd_disk
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
add_mount ssd /dev/sdb1 ext4 $BIG $BIG 2065
expect "HDD ranked after a SATA SSD" "$F/mnt/ssd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
hdd_disk
ssd_disk
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
add_mount ssd /dev/sdb1 ext4 $((G / 2)) $BIG 2065
expect "HDD chosen when the SSD is full" "$F/mnt/hdd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
nvme_disk
ssd_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305 nouser
add_mount ssd /dev/sdb1 ext4 $BIG $BIG 2065
expect "mount without a \$USER directory skipped" "$F/mnt/ssd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
nvme_disk
ssd_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
chmod a-w "$F/mnt/nvme/$USER"
add_mount ssd /dev/sdb1 ext4 $BIG $BIG 2065
if [[ -w "$F/mnt/nvme/$USER" ]]; then
    pass "read-only \$USER directory skipped (not checkable as root)"
else
    expect "read-only \$USER directory skipped" "$F/mnt/ssd/$USER/$H/2" 0 "${STD_ARGS[@]}"
fi
chmod u+w "$F/mnt/nvme/$USER"

# Boot disk
reset_host
add_disk sda3 sda - 0
hdd_disk
add_mount scratch /dev/sda3 ext4 $BIG $BIG 2051
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
expect "partition of the boot disk skipped for an HDD" "$F/mnt/hdd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
add_disk sda3 sda - 0
add_disk dm-0 sda3 - 0
add_mount lvm /dev/dm-0 ext4 $BIG $BIG 64768
hdd_disk
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
expect "LVM volume on the boot disk skipped" "$F/mnt/hdd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
add_disk sda3 sda - 0
add_mount scratch /dev/sda3 ext4 $BIG $BIG 2051
expect "only the boot disk: fallback to TMPDIR" "$F/tmp/$H/2" "no local non-OS drive" "${STD_ARGS[@]}"
grep -q "^WARNING: .*boot disk" <<< "$last_output" && pass "fallback on the boot disk warns" ||
    fail "fallback on the boot disk warns: $last_output"

reset_host
add_mount tmpfs tmpfs tmpfs $BIG $BIG 45
add_mount nfs server:/export nfs4 $BIG $BIG 77
expect "tmpfs and NFS mounts are not discovered" "$F/tmp/$H/2" "no local non-OS drive" "${STD_ARGS[@]}"

# Sticky directory
reset_host
nvme_disk
hdd_disk
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
mkdir -p "$F/mnt/hdd/$USER/$H/2"
touch "$F/mnt/hdd/$USER/$H/2/00000000000000000001.wal"
expect "existing WAL directory wins over a better new candidate" "$F/mnt/hdd/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
nvme_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
mkdir -p "$F/tmp/$H/2"
touch "$F/tmp/$H/2/00000000000000000003.wal"
expect "existing WAL directory in the fallback wins" "$F/tmp/$H/2" "boot disk" "${STD_ARGS[@]}"

reset_host
nvme_disk
hdd_disk
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
# a full drive keeps its leftovers too
add_disk sdd - sata 0
add_disk sdd1 sdd - 0
add_mount full /dev/sdd1 ext4 1000 $BIG 2097
mkdir -p "$F/mnt/full/$USER/$H/2"
touch "$F/mnt/full/$USER/$H/2/00000000000000000001.wal"
expect "existing WAL directory on a full drive still wins" "$F/mnt/full/$USER/$H/2" "bytes free" "${STD_ARGS[@]}"

reset_host
nvme_disk
hdd_disk
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
mkdir -p "$F/mnt/hdd/$USER/$H/2" "$F/mnt/hdd/$USER/$H/3" "$F/mnt/hdd/$USER/ffffffffffff/2"
touch "$F/mnt/hdd/$USER/$H/2/notes.txt" "$F/mnt/hdd/$USER/$H/3/00000000000000000001.wal" \
    "$F/mnt/hdd/$USER/ffffffffffff/2/00000000000000000001.wal"
expect "no *.wal for this install and group: not sticky" "$F/mnt/nvme/$USER/$H/2" 0 "${STD_ARGS[@]}"

reset_host
nvme_disk
hdd_disk
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
mkdir -p "$F/mnt/hdd/$USER/$H/2" "$F/tmp/$H/2"
touch "$F/mnt/hdd/$USER/$H/2/00000000000000000001.wal" "$F/tmp/$H/2/00000000000000000001.wal"
expect "two existing WAL directories: the first is used, the other named" "$F/mnt/hdd/$USER/$H/2" \
    "$F/tmp/$H/2" "${STD_ARGS[@]}"

# --wal-dir
reset_host
nvme_disk
hdd_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
expect "--wal-dir list order beats the ranking" "$F/mnt/hdd/$USER/$H/2" 0 "${STD_ARGS[@]}" \
    --wal-dir "$F/mnt/hdd/$USER:$F/mnt/nvme/$USER"
expect "--wal-dir skips a missing entry" "$F/mnt/nvme/$USER/$H/2" 0 "${STD_ARGS[@]}" \
    --wal-dir "$F/mnt/missing:$F/mnt/nvme/$USER:$F/mnt/hdd/$USER"

reset_host
nvme_disk
hdd_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
mkdir -p "$F/mnt/hdd/$USER/$H/2"
touch "$F/mnt/hdd/$USER/$H/2/00000000000000000001.wal"
expect "--wal-dir list: an existing WAL directory in the list wins" "$F/mnt/hdd/$USER/$H/2" 0 "${STD_ARGS[@]}" \
    --wal-dir "$F/mnt/nvme/$USER:$F/mnt/hdd/$USER"

reset_host
nvme_disk
hdd_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
mkdir -p "$F/mnt/hdd/$USER/$H/2"
touch "$F/mnt/hdd/$USER/$H/2/00000000000000000001.wal"
expect "--wal-dir list names leftovers outside the list" "$F/mnt/nvme/$USER/$H/2" "$F/mnt/hdd/$USER/$H/2 holds" \
    "${STD_ARGS[@]}" --wal-dir "$F/mnt/nvme/$USER"
expect "--wal-dir list names leftovers outside the list (two entries)" "$F/mnt/nvme/$USER/$H/2" \
    "$F/mnt/hdd/$USER/$H/2 holds" "${STD_ARGS[@]}" --wal-dir "$F/mnt/nvme/$USER:$F/mnt/missing"

reset_host
nvme_disk
hdd_disk
add_mount nvme /dev/nvme0n1p1 xfs $((G / 2)) $BIG 66305
add_mount hdd /dev/sdc1 ext4 $BIG $BIG 2081
expect "--wal-dir list: a full first entry is skipped for the second" "$F/mnt/hdd/$USER/$H/2" 0 "${STD_ARGS[@]}" \
    --wal-dir "$F/mnt/nvme/$USER:$F/mnt/hdd/$USER"

# expect_refused LABEL ENTRY|REASON... -- PROBE_ARGS...: the probe exits nonzero
# with no WAL_DIR line and one "ERROR: <entry>: <reason>" line per entry, in
# order, and creates nothing under TMPDIR
expect_refused() {
    local label=$1 want=() out status got
    shift
    while [[ "$1" != -- ]]; do
        want+=("$1")
        shift
    done
    shift
    out=$(run_probe "$@")
    status=$?
    got=$(grep '^ERROR: ' <<< "$out")
    if ((status == 0)); then
        fail "$label: probe exited 0: $out"
    elif grep -q '^WAL_DIR=' <<< "$out"; then
        fail "$label: printed WAL_DIR: $out"
    elif [[ "$(grep -c '^ERROR: ' <<< "$out")" -ne ${#want[@]} ]]; then
        fail "$label: expected ${#want[@]} ERROR lines: $out"
    elif [[ -n "$(ls -A "$F/tmp")" ]]; then
        fail "$label: created $(ls -A "$F/tmp") under TMPDIR"
    else
        local i=0 line
        while IFS= read -r line; do
            if [[ "$line" != "ERROR: ${want[i]%%|*}: "*"${want[i]#*|}"* ]]; then
                fail "$label: ERROR line $((i + 1)) '$line', expected entry '${want[i]%%|*}' and '${want[i]#*|}'"
                return
            fi
            i=$((i + 1))
        done <<< "$got"
        pass "$label"
    fi
}

reset_host
nvme_disk
add_mount nvme /dev/nvme0n1p1 xfs $((G / 2)) $BIG 66305
touch "$F/afile"
expect_refused "--wal-dir list with no usable entry fails, one ERROR per entry" \
    "$F/mnt/nvme/$USER|less than max_bytes" "$F/mnt/missing|does not exist" "$F/afile|not a directory" -- \
    "${STD_ARGS[@]}" --wal-dir "$F/mnt/nvme/$USER:$F/mnt/missing:$F/afile"
expect_refused "--wal-dir list of two missing entries fails" \
    "$F/mnt/missing|does not exist" "$F/mnt/gone|does not exist" -- \
    "${STD_ARGS[@]}" --wal-dir "$F/mnt/missing:$F/mnt/gone"

reset_host
nvme_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
mkdir -p "$F/rootdir"
expect "single --wal-dir on the boot disk used as given, with a warning" "$F/rootdir/$H/2" "boot disk" \
    "${STD_ARGS[@]}" --wal-dir "$F/rootdir"
expect "single --wal-dir that does not exist yet is created" "$F/newdir/$H/2" 1 \
    "${STD_ARGS[@]}" --wal-dir "$F/newdir"

reset_host
nvme_disk
add_mount nvme /dev/nvme0n1p1 xfs $((G / 2)) $BIG 66305
expect "single --wal-dir on a full drive used, with a warning" "$F/mnt/nvme/$USER/$H/2" "bytes free" \
    "${STD_ARGS[@]}" --wal-dir "$F/mnt/nvme/$USER"

reset_host
nvme_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
mkdir -p "$F/mnt/nvme/$USER/$H/2"
touch "$F/mnt/nvme/$USER/$H/2/00000000000000000001.wal"
add_mount vol /dev/sdz1 ext4 $BIG $BIG 9999
expect "single --wal-dir names leftovers it does not use" "$F/mnt/vol/$H/2" "$F/mnt/nvme/$USER/$H/2" \
    "${STD_ARGS[@]}" --wal-dir "$F/mnt/vol"

# File system warnings
reset_host
add_mount tmpfs tmpfs tmpfs $BIG $BIG 45
expect "tmpfs warning" "$F/mnt/tmpfs/$USER/$H/2" "tmpfs: durable writes survive a keeper crash but not a node reboot" \
    "${STD_ARGS[@]}" --wal-dir "$F/mnt/tmpfs/$USER"
grep -q "boot disk" <<< "$last_output" && fail "tmpfs: no boot disk warning: $last_output" ||
    pass "tmpfs: device number differs from /, no boot disk warning"
for fstype in nfs nfs4; do
    reset_host
    add_mount nfs server:/export "$fstype" $BIG $BIG 77
    expect "$fstype warning" "$F/mnt/nfs/$USER/$H/2" "exported sync.*lock service" \
        "${STD_ARGS[@]}" --wal-dir "$F/mnt/nfs/$USER"
done
for fstype in lustre gpfs beegfs cifs smb3 ceph fuse.sshfs overlay; do
    reset_host
    add_mount net "src-$fstype" "$fstype" $BIG $BIG 88
    expect "$fstype warning" "$F/mnt/net/$USER/$H/2" "on $fstype" "${STD_ARGS[@]}" --wal-dir "$F/mnt/net/$USER"
done
reset_host
add_mount overlay overlay overlay $BIG $BIG 2050
expect "overlay on the root's device number warns about the boot disk" "$F/mnt/overlay/$USER/$H/2" "boot disk" \
    "${STD_ARGS[@]}" --wal-dir "$F/mnt/overlay/$USER"

reset_host
printf '%s\n' "/ /dev/sda2 ext4 100000000 $((2 * G)) 2050" > "$mounts"
expect "fallback below the reserve warns" "$F/tmp/$H/2" "below the 268435456 byte reserve" "${STD_ARGS[@]}"

# Arguments
reset_host
for bad in "--group 2 --max-bytes 1 --reserve-min 1 --reserve-percent 1" \
    "--work-dir-hash $H --group x --max-bytes 1 --reserve-min 1 --reserve-percent 1" \
    "--work-dir-hash ../.. --group 2 --max-bytes 1 --reserve-min 1 --reserve-percent 1" \
    "--work-dir-hash $H --group 2 --max-bytes 1 --reserve-min 1 --reserve-percent 101" \
    "--work-dir-hash $H --group 2 --max-bytes 1 --reserve-min 1 --reserve-percent 1 --wal-dir relative" \
    "--work-dir-hash $H --group 2 --max-bytes 1 --reserve-min 1 --reserve-percent 1 --bogus"; do
    # shellcheck disable=SC2086
    if out=$(run_probe $bad); then
        fail "refused: $bad: got $out"
    elif grep -q '^WAL_DIR=' <<< "$out"; then
        fail "refused without WAL_DIR: $bad"
    else
        pass "refused: $bad"
    fi
done

# deploy_cluster.sh: the probe runs over ssh and its choice goes into the conf
source <(sed -n '/^work_dir_hash() {/,/^}/p; /^assign_keeper_wal_dir() {/,/^}/p' \
    "$repo_root/tools/deploy/deploy_cluster.sh")
ERR='' INFO='' WARN='' NC=''
SCRIPT_DIR="$repo_root/tools/deploy"
WORK_DIR="$fixture/work dir"
CONF_DIR="$fixture/conf"
mkdir -p "$CONF_DIR"
ssh() { # runs the command locally, as keeper host $2 would
    [[ "$1" == -n ]] || return 255
    echo "$2" >> "$fixture/ssh-hosts"
    PATH="$TEST_PATH" TMPDIR="$F/tmp" bash -c "$3"
}
expected_hash=$(printf '%s' "$WORK_DIR" | sha256sum | cut -c1-12)
check_conf() { # LABEL WANT_DIR
    if [[ "$(jq -r '.chrono_keeper.WAL.enabled' "$conf")" == true &&
        "$(jq -r '.chrono_keeper.WAL.dir' "$conf")" == "$2" &&
        "$(jq -r '.chrono_keeper.WAL.segment_bytes' "$conf")" == 67108864 ]]; then
        pass "$1"
    else
        fail "$1: $(jq -c '.chrono_keeper.WAL' "$conf")"
    fi
}
reset_host
nvme_disk
add_mount nvme /dev/nvme0n1p1 xfs $BIG $BIG 66305
conf="$CONF_DIR/default-chrono-conf.json.3.keeper.node7"
command cp -f "$repo_root/conf/default_conf.json.in" "$conf"
WAL_DIR_LIST=""
cluster_out=$(assign_keeper_wal_dir node7 3 "$conf" 2> "$fixture/cluster-stderr")
check_conf "deploy_cluster: discovered directory written into the conf" "$F/mnt/nvme/$USER/$expected_hash/3"
grep -q "node7.*$F/mnt/nvme/$USER/$expected_hash/3" <<< "$cluster_out" && pass "deploy_cluster: chosen directory printed" ||
    fail "deploy_cluster: chosen directory printed: $cluster_out"
[[ "$(cat "$fixture/ssh-hosts")" == node7 ]] && pass "deploy_cluster: probe ran on the keeper host" ||
    fail "deploy_cluster: probe ran on the keeper host"

WAL_DIR_LIST="$F/tmp"
assign_keeper_wal_dir node7 3 "$conf" > /dev/null 2> "$fixture/cluster-stderr"
check_conf "deploy_cluster: --wal-dir passed to the probe" "$F/tmp/$expected_hash/3"
grep -q "WARNING: node7: .*boot disk" "$fixture/cluster-stderr" && pass "deploy_cluster: probe warnings shown with the host" ||
    fail "deploy_cluster: probe warnings shown with the host: $(cat "$fixture/cluster-stderr")"

WAL_DIR_LIST="relative/path"
if (assign_keeper_wal_dir node7 3 "$conf" > /dev/null 2>&1); then
    fail "deploy_cluster: a failed probe stops the deploy"
else
    pass "deploy_cluster: a failed probe stops the deploy"
fi

reset_host
WAL_DIR_LIST="$F/mnt/missing:$F/mnt/gone"
if (assign_keeper_wal_dir node7 3 "$conf" > /dev/null 2> "$fixture/cluster-stderr"); then
    fail "deploy_cluster: a --wal-dir list with no usable entry stops the deploy"
else
    pass "deploy_cluster: a --wal-dir list with no usable entry stops the deploy"
fi
grep -q "node7: ERROR: $F/mnt/missing: does not exist" "$fixture/cluster-stderr" &&
    grep -q "node7: ERROR: $F/mnt/gone: does not exist" "$fixture/cluster-stderr" &&
    pass "deploy_cluster: the probe's errors shown with the host" ||
    fail "deploy_cluster: the probe's errors shown with the host: $(cat "$fixture/cluster-stderr")"

# Arguments of deploy_cluster.sh and their pass-through from single_user_deploy.sh
source <(sed -n '/^parse_args() {/,/^}/p' "$repo_root/tools/deploy/deploy_cluster.sh")
WAL_ENABLED=0 WAL_DIR_LIST=""
parse_args --start --wal > /dev/null
[[ "$WAL_ENABLED" == 1 && -z "$WAL_DIR_LIST" ]] && pass "deploy_cluster: --wal turns the WAL on" ||
    fail "deploy_cluster: --wal turns the WAL on"
WAL_ENABLED=0 WAL_DIR_LIST=""
parse_args --start --wal-dir /a:/b > /dev/null
[[ "$WAL_ENABLED" == 1 && "$WAL_DIR_LIST" == /a:/b ]] && pass "deploy_cluster: --wal-dir turns the WAL on with its list" ||
    fail "deploy_cluster: --wal-dir turns the WAL on with its list"

source <(sed -n '/^delegate_to_deploy_cluster() {/,/^}/p; /^parse_args() {/,/^}/p' \
    "$repo_root/tools/deploy/single_user_deploy.sh")
DEBUG=''
DEPLOY_CLUSTER_SCRIPT="$fixture/fake_deploy_cluster.sh"
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$@" > "%s"\n' "$fixture/delegated-args" > "$DEPLOY_CLUSTER_SCRIPT"
chmod +x "$DEPLOY_CLUSTER_SCRIPT"
delegated() { # MODE ARGS...: runs single_user_deploy.sh's parse_args and delegation, prints the delegated args
    local mode=$1
    shift
    wal=false WAL_DIR_LIST="" JOB_ID="" verbose=false NUM_RECORDING_GROUP=1
    WORK_DIR="$fixture/work"
    MONITOR_DIR="$WORK_DIR/monitor" OUTPUT_DIR="$WORK_DIR/output"
    VISOR_BIN="$WORK_DIR/bin/chrono-visor" GRAPHER_BIN="$WORK_DIR/bin/chrono-grapher"
    KEEPER_BIN="$WORK_DIR/bin/chrono-keeper" PLAYER_BIN="$WORK_DIR/bin/chrono-player"
    CONF_FILE="$WORK_DIR/conf/default-chrono-conf.json" CLIENT_CONF_FILE="$WORK_DIR/conf/default-chrono-client-conf.json"
    VISOR_HOSTS="$WORK_DIR/conf/hosts_visor" GRAPHER_HOSTS="$WORK_DIR/conf/hosts_grapher"
    KEEPER_HOSTS="$WORK_DIR/conf/hosts_keeper"
    parse_args "$@" > /dev/null
    delegate_to_deploy_cluster "$mode" > /dev/null
    tr '\n' ' ' < "$fixture/delegated-args"
}
[[ "$(delegated --start --start --wal --wal-dir '/x y:/z')" == *"--wal --wal-dir /x y:/z "* ]] &&
    pass "single_user_deploy: --wal and --wal-dir passed on start" ||
    fail "single_user_deploy: --wal and --wal-dir passed on start: $(cat "$fixture/delegated-args")"
[[ "$(delegated --stop --stop --wal --wal-dir /z)" != *"--wal"* ]] &&
    pass "single_user_deploy: WAL options not passed on stop" ||
    fail "single_user_deploy: WAL options not passed on stop"

echo "$failures WAL directory probe checks failed"
test "$failures" -eq 0
