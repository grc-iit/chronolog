#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 RESULT_DIR [BASE_PATH...]" >&2
  exit 2
fi

RESULT_DIR=$1
shift

if [[ $# -gt 0 ]]; then
  BASE_PATHS=("$@")
else
  BASE_PATHS=(
    "${RESULT_DIR}/shared"
    "/mnt/nvme/${USER:-unknown}/chronolog-fsync-probe"
    "/mnt/ssd/${USER:-unknown}/chronolog-fsync-probe"
  )
fi

HOST=$(hostname)
HOST_DIR="${RESULT_DIR}/hosts/${HOST}"
mkdir -p "${HOST_DIR}"

printf 'host=%s\n' "${HOST}" >"${HOST_DIR}/manifest.env"
printf 'timestamp=%s\n' "$(date --iso-8601=seconds)" >>"${HOST_DIR}/manifest.env"

for base in "${BASE_PATHS[@]}"; do
  label=$(printf '%s' "${base}" | sed 's#[^A-Za-z0-9_.-]#_#g')
  work_dir="${base}/${HOST}"
  mkdir -p "${work_dir}"

  for bs in 1024 65536; do
    size=$((bs * 4096))
    if (( size < 16777216 )); then
      size=16777216
    fi

    out="${HOST_DIR}/${label}-bs${bs}.json"
    fio \
      --name="fdatasync_bs${bs}" \
      --directory="${work_dir}" \
      --filename="probe-bs${bs}.dat" \
      --rw=write \
      --ioengine=sync \
      --bs="${bs}" \
      --size="${size}" \
      --fdatasync=1 \
      --numjobs=1 \
      --iodepth=1 \
      --output-format=json \
      --output="${out}"
    rm -f "${work_dir}/probe-bs${bs}.dat"
  done
done
