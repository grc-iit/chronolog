#!/usr/bin/env bash
set -euo pipefail
visor=$1
client=$2
work=$(mktemp -d)
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$work"
}
trap cleanup EXIT
launch() {
    rm -f "$work"/*.sqlite*
    block1=$((10000 + (RANDOM % 4400) * 5))
    block2=$((10000 + (RANDOM % 4400) * 5))
    block3=$((10000 + (RANDOM % 4400) * 5))
python3 - "$work" "$block1" "$block2" "$block3" <<'PY'
import json,sys
from pathlib import Path
ports=[int(block)+offset for block in sys.argv[2:] for offset in range(3)]
peers=[dict(id=i+1,raft_endpoint=f'127.0.0.1:{ports[i*3]}',catalog_endpoint=f'127.0.0.1:{ports[i*3+1]}',internal_endpoint=f'127.0.0.1:{ports[i*3+2]}') for i in range(3)]
p=Path(sys.argv[1])
for i,peer in enumerate(peers):
    cfg=dict(membership_mode='dynamic',listen=peer['catalog_endpoint'],internal_listen=peer['internal_endpoint'],db_path=str(p/f'{i}.sqlite'),release_fence_timeout_ms=10,raft=dict(server_id=i+1,raft_endpoint=peer['raft_endpoint'],peers=peers))
    (p/f'{i}.json').write_text(json.dumps(cfg))
(p/'endpoints').write_text('\n'.join(peer['catalog_endpoint'] for peer in peers)+'\n')
PY
for i in 0 1 2; do
    "$visor" --config "$work/$i.json" >"$work/$i.log" 2>&1 &
    pids+=("$!")
done
    for attempt in $(seq 1 300); do
        local ready=0
        for i in 0 1 2; do
            kill -0 "${pids[i]}" 2>/dev/null || return 1
            if rg -q "catalog ready" "$work/$i.log"; then ready=$((ready+1)); fi
        done
        [ "$ready" -eq 3 ] && return 0
        sleep 0.1
    done
    return 1
}
for attempt in 1 2 3; do
    if launch; then break; fi
    cat "$work/"*.log
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    pids=()
    [ "$attempt" -lt 3 ] || exit 1
done
mapfile -t endpoints < "$work/endpoints"
timeout 100 "$client" "${endpoints[@]}" "${pids[@]}" "$work" &
client_pid=$!
for attempt in $(seq 1 900); do
    if ! kill -0 "$client_pid" 2>/dev/null; then break; fi
    if [[ -f "$work/restart" ]]; then
        for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
        pids=()
        for i in 0 1 2; do
            "$visor" --config "$work/$i.json" >>"$work/$i.log" 2>&1 &
            pids+=("$!")
        done
        touch "$work/restarted"
        break
    fi
    sleep 0.1
done
if ! wait "$client_pid"; then
    cat "$work/"*.log
    exit 1
fi
