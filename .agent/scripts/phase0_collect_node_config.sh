#!/usr/bin/env bash

set -euo pipefail

echo "node=$(hostname)"
echo "timestamp=$(date '+%Y-%m-%d %H:%M %Z')"
echo "slurm_job_id=${SLURM_JOB_ID:-}"
echo "slurm_nodeid=${SLURM_NODEID:-}"
echo "slurm_procid=${SLURM_PROCID:-}"

echo "## cpu"
lscpu 2>/dev/null | sed -n '1,80p' || true

echo "## memory"
free -h 2>/dev/null || true

echo "## ip -br addr"
ip -br addr 2>/dev/null || true

echo "## ip route"
ip route 2>/dev/null || true

echo "## rdma"
ls /sys/class/infiniband 2>/dev/null || true
if command -v ibv_devinfo >/dev/null 2>&1; then
  ibv_devinfo 2>/dev/null || true
fi

echo "## network interfaces"
for iface in $(ls /sys/class/net 2>/dev/null | sort); do
  echo "### iface=${iface}"
  cat "/sys/class/net/${iface}/operstate" 2>/dev/null || true
  if command -v ethtool >/dev/null 2>&1; then
    ethtool "${iface}" 2>/dev/null | sed -n '1,45p' || true
  fi
done
