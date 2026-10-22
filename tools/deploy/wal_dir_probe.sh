#!/usr/bin/env bash
# Picks the keeper WAL directory on this host; deploy_cluster.sh runs it on each
# keeper host over ssh. Prints WAL_DIR=<dir> and zero or more "WARNING: ..." lines
# on stdout, and creates <dir>. Exits nonzero, without a WAL_DIR line, on bad
# arguments or when <dir> cannot be created.
#
# The base directory is, in this order:
#  1. one whose <base>/<hash>/<group> already holds *.wal, among the candidates and
#     the fallback, wherever it ranks: a restarted keeper finds its leftovers there;
#  2. the first candidate that passes: an existing, writable <mount>/$USER on a
#     local block-device mount outside the boot disk, ranked NVMe, SATA SSD, HDD
#     (lsblk TRAN and ROTA), with free space minus the reserve at least max_bytes;
#  3. ${TMPDIR:-/tmp} as resolved on this host, with a warning.
# --wal-dir A:B:C replaces the discovered candidates with that ordered list (the
# boot-disk and media checks do not apply to it). A single --wal-dir path is used
# as given. <hash>/<group> is appended to the chosen base in every case.
#
# "Boot disk" compares the physical disk under the directory (findmnt SOURCE, then
# lsblk PKNAME up to the whole disk) with the one under /. When either is not a
# block device (overlay, tmpfs, NFS) it compares file-system device numbers.

set -uo pipefail

USER=${USER:-$(id -un)}

usage() {
  echo "Usage: $0 --work-dir-hash <h> --group <g> --max-bytes <n> --reserve-min <n> --reserve-percent <p> [--wal-dir A:B:C]" >&2
  exit 2
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

work_dir_hash=""
group=""
max_bytes=""
reserve_min=""
reserve_percent=""
wal_dir_list=""
wal_dir_given=false
while [[ $# -gt 0 ]]; do
  case "$1" in
  --work-dir-hash | --group | --max-bytes | --reserve-min | --reserve-percent | --wal-dir)
    [[ $# -ge 2 ]] || usage
    case "$1" in
    --work-dir-hash) work_dir_hash=$2 ;;
    --group) group=$2 ;;
    --max-bytes) max_bytes=$2 ;;
    --reserve-min) reserve_min=$2 ;;
    --reserve-percent) reserve_percent=$2 ;;
    --wal-dir)
      wal_dir_list=$2
      wal_dir_given=true
      ;;
    esac
    shift 2
    ;;
  *)
    echo "ERROR: unknown argument: $1" >&2
    usage
    ;;
  esac
done
[[ "${work_dir_hash}" =~ ^[0-9a-f]{1,64}$ ]] || usage
[[ "${group}" =~ ^[0-9]+$ ]] || usage
[[ "${max_bytes}" =~ ^[0-9]+$ ]] || usage
[[ "${reserve_min}" =~ ^[0-9]+$ ]] || usage
[[ "${reserve_percent}" =~ ^[0-9]+$ ]] && ((10#${reserve_percent} <= 100)) || usage
wal_dir_entries=()
if ${wal_dir_given}; then
  IFS=: read -ra wal_dir_entries <<<"${wal_dir_list}"
  [[ ${#wal_dir_entries[@]} -gt 0 ]] || usage
  for entry in "${wal_dir_entries[@]}"; do
    [[ "${entry}" == /* ]] || die "--wal-dir entry '${entry}' is not an absolute path"
  done
fi

warnings=()
warn() { warnings+=("$*"); }

trim() {
  local s=$1
  s=${s#"${s%%[![:space:]]*}"}
  s=${s%"${s##*[![:space:]]}"}
  echo "${s}"
}

# Sets MOUNT_TARGET, MOUNT_SOURCE and MOUNT_FSTYPE for the mount holding $1.
mount_of() {
  local line target source fstype
  line=$(findmnt -rn -o TARGET,SOURCE,FSTYPE --target "$1" 2>/dev/null | head -n 1) || return 1
  read -r target source fstype <<<"${line}"
  [[ -n "${target}" ]] || return 1
  # -r writes a space in a mount point as \x20
  printf -v MOUNT_TARGET '%b' "${target}"
  # a bind mount or a btrfs subvolume shows as /dev/sda1[/subdir]
  MOUNT_SOURCE=${source%%\[*}
  MOUNT_FSTYPE=${fstype}
}

# Prints the whole disk under block device $1, following PKNAME through
# partitions, LVM and RAID. Returns 1 when lsblk does not know the device.
top_disk() {
  local dev=$1 parent hops
  for hops in 1 2 3 4 5 6 7 8; do
    parent=$(lsblk -dno PKNAME "${dev}" 2>/dev/null) || return 1
    parent=$(trim "$(head -n 1 <<<"${parent}")")
    if [[ -z "${parent}" ]]; then
      echo "${dev##*/}"
      return 0
    fi
    dev="/dev/${parent}"
  done
  echo "${dev##*/}"
}

# Prints 0 for NVMe, 1 for a SATA SSD (non-rotational), 2 for an HDD or unknown.
disk_rank() {
  local tran rota
  tran=$(trim "$(lsblk -dno TRAN "/dev/$1" 2>/dev/null)")
  rota=$(trim "$(lsblk -dno ROTA "/dev/$1" 2>/dev/null)")
  if [[ "${tran}" == nvme ]]; then
    echo 0
  elif [[ "${rota}" == 0 ]]; then
    echo 1
  else
    echo 2
  fi
}

root_disk=""
if mount_of / && [[ "${MOUNT_SOURCE}" == /dev/* ]]; then
  root_disk=$(top_disk "${MOUNT_SOURCE}") || root_disk=""
fi
root_dev=$(stat -c %d / 2>/dev/null) || root_dev=""

# Returns 0 when existing path $1 is on the boot disk.
on_boot_disk() {
  local disk dev
  if [[ -n "${root_disk}" ]] && mount_of "$1" && [[ "${MOUNT_SOURCE}" == /dev/* ]] &&
    disk=$(top_disk "${MOUNT_SOURCE}"); then
    [[ "${disk}" == "${root_disk}" ]]
    return
  fi
  dev=$(stat -c %d "$1" 2>/dev/null) || return 1
  [[ -n "${root_dev}" && "${dev}" == "${root_dev}" ]]
}

# Sets FREE_BYTES and RESERVE_BYTES for existing path $1: the reserve is
# max(reserve_min, reserve_percent of the file system), as the keeper computes it.
space_of() {
  local line avail size
  line=$(df -B1 --output=avail,size "$1" 2>/dev/null | tail -n 1) || return 1
  read -r avail size <<<"${line}"
  [[ "${avail}" =~ ^[0-9]+$ && "${size}" =~ ^[0-9]+$ ]] || return 1
  FREE_BYTES=${avail}
  RESERVE_BYTES=$((size * 10#${reserve_percent} / 100))
  ((RESERVE_BYTES >= reserve_min)) || RESERVE_BYTES=${reserve_min}
}

# Returns 0 when $1 is an existing, writable directory with free space minus the
# reserve at least max_bytes.
usable() {
  [[ -d "$1" && -w "$1" && -x "$1" ]] || return 1
  space_of "$1" || return 1
  ((FREE_BYTES - RESERVE_BYTES >= max_bytes))
}

# Fills discovered_bases with every existing <mount>/$USER on a local block-device
# mount, and candidates with those outside the boot disk, ranked.
discovered_bases=()
candidates=()
discover() {
  local target source fstype base disk rank
  local ranked=() mounts
  mounts=$(findmnt -rn -o TARGET,SOURCE,FSTYPE 2>/dev/null) || return 0
  while read -r target source fstype; do
    [[ -n "${target}" ]] || continue
    printf -v target '%b' "${target}"
    source=${source%%\[*}
    [[ "${source}" == /dev/* ]] || continue
    base="${target%/}/${USER}"
    [[ -d "${base}" ]] || continue
    case " ${discovered_bases[*]} " in *" ${base} "*) continue ;; esac
    discovered_bases+=("${base}")
    if disk=$(top_disk "${source}"); then
      [[ -n "${root_disk}" && "${disk}" == "${root_disk}" ]] && continue
      rank=$(disk_rank "${disk}")
    else
      on_boot_disk "${base}" && continue
      rank=2
    fi
    ranked+=("${rank} ${base}")
  done <<<"${mounts}"
  for rank in 0 1 2; do
    for entry in "${ranked[@]}"; do
      [[ "${entry%% *}" == "${rank}" ]] && candidates+=("${entry#* }")
    done
  done
}

wal_suffix="${work_dir_hash}/${group}"
fallback=$(realpath -m "${TMPDIR:-/tmp}")

# Fills leftovers with the <base>/<hash>/<group> holding *.wal, for each base in
# "$@" in order, once each.
leftovers=()
find_leftovers() {
  local base dir
  for base in "$@"; do
    dir="$(realpath -m "${base}")/${wal_suffix}"
    case " ${leftovers[*]} " in *" ${dir} "*) continue ;; esac
    compgen -G "${dir}/*.wal" >/dev/null && leftovers+=("${dir}")
  done
  return 0
}

base=""
candidates_checked=()
if ${wal_dir_given} && [[ ${#wal_dir_entries[@]} -eq 1 ]]; then
  base=${wal_dir_entries[0]}
  discover
  find_leftovers "${discovered_bases[@]}" "${fallback}"
else
  if ${wal_dir_given}; then
    search=("${wal_dir_entries[@]}")
  else
    discover
    search=("${discovered_bases[@]}")
    candidates_checked=("${candidates[@]}")
  fi
  find_leftovers "${search[@]}" "${fallback}"
  if [[ ${#leftovers[@]} -gt 0 ]]; then
    base=${leftovers[0]%/"${wal_suffix}"}
  else
    if ${wal_dir_given}; then
      candidates_checked=("${wal_dir_entries[@]}")
    fi
    for candidate in "${candidates_checked[@]}"; do
      if usable "${candidate}"; then
        base=${candidate}
        break
      fi
    done
  fi
  if [[ -z "${base}" ]]; then
    base=${fallback}
    if ${wal_dir_given}; then
      warn "none of --wal-dir ${wal_dir_list} is an existing writable directory with ${max_bytes} bytes" \
        "(max_bytes) free above the reserve; using ${fallback}"
    else
      warn "no local non-OS drive has an existing writable <mount>/${USER} with ${max_bytes} bytes" \
        "(max_bytes) free above the reserve; using ${fallback}"
    fi
  fi
  # leftovers outside the list are only named
  if ${wal_dir_given}; then
    discover
    find_leftovers "${discovered_bases[@]}"
  fi
fi

wal_dir="${base%/}/${wal_suffix}"
mkdir -p -- "${wal_dir}" 2>/dev/null || die "cannot create WAL directory ${wal_dir}"

for dir in "${leftovers[@]}"; do
  [[ "${dir}" == "$(realpath -m "${wal_dir}")" ]] && continue
  warn "${dir} holds WAL segments of this install that a keeper using ${wal_dir} will not recover"
done

if mount_of "${wal_dir}"; then
  case "${MOUNT_FSTYPE}" in
  nfs | nfs4)
    warn "WAL directory ${wal_dir} is on ${MOUNT_FSTYPE}, a shared or network file system: a durable write is" \
      "only as durable as the server's export (it must be exported sync) and the server's storage cache, each" \
      "fsync is a round trip to the server, and the WAL's flock depends on the server's lock service"
    ;;
  lustre | gpfs | beegfs | cifs | smb3 | ceph | fuse.*)
    warn "WAL directory ${wal_dir} is on ${MOUNT_FSTYPE}, a shared or network file system: a durable write is" \
      "only as durable as that file system's own fsync, and the WAL's flock depends on its locking"
    ;;
  tmpfs)
    warn "WAL directory ${wal_dir} is on tmpfs: durable writes survive a keeper crash but not a node reboot"
    ;;
  overlay)
    warn "WAL directory ${wal_dir} is on overlay: the WAL lives in a container's writable layer and is lost" \
      "with the container; mount a host directory for it"
    ;;
  esac
fi
if on_boot_disk "${wal_dir}"; then
  warn "WAL directory ${wal_dir} is on the boot disk (the disk that holds /); prefer a local non-OS drive"
fi
if space_of "${wal_dir}"; then
  if ((FREE_BYTES < RESERVE_BYTES)); then
    warn "WAL directory ${wal_dir} has ${FREE_BYTES} bytes free, below the ${RESERVE_BYTES} byte reserve:" \
      "the keeper refuses durable writes until space is freed"
  elif ((FREE_BYTES - RESERVE_BYTES < max_bytes)); then
    warn "WAL directory ${wal_dir} has ${FREE_BYTES} bytes free; above the ${RESERVE_BYTES} byte reserve" \
      "that is less than max_bytes (${max_bytes})"
  fi
fi

for line in "${warnings[@]}"; do
  echo "WARNING: ${line}"
done
echo "WAL_DIR=${wal_dir}"
