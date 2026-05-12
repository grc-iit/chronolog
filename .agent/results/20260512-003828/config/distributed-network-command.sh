#!/usr/bin/env bash
set -euo pipefail
echo "node=$(hostname)"
echo "date=$(date '+%Y-%m-%d %H:%M %Z')"
echo '## ip -br addr'
ip -br addr || true
echo '## ip route'
ip route || true
echo '## interfaces'
for iface in $(ls /sys/class/net | sort); do
  echo "### iface=$iface"
  cat "/sys/class/net/$iface/operstate" 2>/dev/null || true
  ethtool "$iface" 2>/dev/null | sed -n '1,40p' || true
  ethtool -S "$iface" 2>/dev/null | sed -n '1,80p' || true
done
echo '## RDMA devices'
ls /sys/class/infiniband 2>/dev/null || true
command -v ibv_devinfo >/dev/null 2>&1 && ibv_devinfo || true
