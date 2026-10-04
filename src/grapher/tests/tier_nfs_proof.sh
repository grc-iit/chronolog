#!/usr/bin/env bash
set -euo pipefail
binary=${1:?test binary required}
root=$(mktemp -d /mnt/nfs/chronolog-sprint/tiers/tier2g-XXXXXXXX)
trap 'chmod -R u+rwx "$root"; rm -rf "$root"' EXIT
filter=${2:-GrapherTierFaults.*:GrapherTierInterleavings.*}
TIER2G_NFS_ROOT="$root" timeout 120 "$binary" --gtest_filter="$filter"
