#!/usr/bin/env bash
set -euo pipefail
engine=$1 project=$2
if [ "${RBUILD_HELD:-}" != stack ] && [ "${CHRONOLOG_STACK_LOCKED:-0}" != 1 ]; then
    exec flock "$HOME/chronolog-sprint/stack.lock" env CHRONOLOG_STACK_LOCKED=1 bash "$0" "$@"
fi
case "$engine" in
    docker) export DOCKER_HOST=unix:///run/user/1000/docker.sock; compose=(docker compose) ;;
    podman) unset DOCKER_HOST; compose=(podman compose) ;;
    *) exit 2 ;;
esac
compose+=(-p "$project" -f deploy/compose/compose.yaml -f deploy/compose/smoke.override.yaml -f build/smoke/python.override.yaml -f deploy/compose/stream.override.yaml)
export CHRONOLOG_STREAM_CHRONICLE="stream-smoke-$engine-$(date +%s)"
export CHRONOLOG_STREAM_HOST="dragon-$engine"
export CHRONOLOG_STREAM_SAMPLES=0 CHRONOLOG_STREAM_INTERVAL_MS=200
cleanup() {
    timeout 30 "${compose[@]}" logs --no-color influxdb grafana chrono-stream-collect chrono-stream-export > "build/smoke/$engine-stream-services.log" 2>&1 || true
    # Only the stream services: the base stack belongs to run_dragon.sh and its later steps.
    timeout 60 "${compose[@]}" rm -s -f -v influxdb grafana chrono-stream-collect chrono-stream-export || true
    timeout 30 "${compose[0]}" volume rm -f "${project}_stream-influx" > /dev/null 2>&1 || true
}
trap cleanup EXIT
timeout 240 "${compose[@]}" up -d --wait --wait-timeout 180 influxdb grafana chrono-stream-collect chrono-stream-export
python=build/smoke-venv/bin/python
timeout 45 "$python" - <<'PY'
import csv,http.client,io,json,os,time,urllib.request,urllib.error
headers={'Authorization':'Token '+os.getenv('INFLUX_TOKEN','chronolog-stream-token'),'Content-Type':'application/vnd.flux','Accept':'application/csv'}
for metric in ['system.cpu.utilization','system.memory.usage','system.network.io']:
    flux=f'from(bucket: "telemetry") |> range(start: -5m) |> filter(fn: (r) => r._measurement == "{metric}") |> limit(n: 1)'
    for attempt in range(100):
        try:
            req=urllib.request.Request('http://127.0.0.1:8086/api/v2/query?org=chronolog',data=flux.encode(),headers=headers)
            with urllib.request.urlopen(req,timeout=2) as response: text=response.read(1<<20).decode()
            rows=[row for row in csv.reader(io.StringIO(text)) if metric in row]
            if rows:
                print('PASS InfluxDB '+metric,flush=True)
                break
        except (urllib.error.URLError,OSError,http.client.HTTPException): pass
        time.sleep(.1)
    else: raise SystemExit('FAIL no Influx points for '+metric)
for attempt in range(100):
    try:
        with urllib.request.urlopen('http://127.0.0.1:3000/api/health',timeout=2) as response:
            if json.load(response).get('database')=='ok': break
    except (urllib.error.URLError,OSError,http.client.HTTPException): pass
    time.sleep(.1)
else: raise SystemExit('FAIL Grafana health')
print('PASS Grafana health',flush=True)
for attempt in range(100):
    try:
        with urllib.request.urlopen('http://127.0.0.1:3000/api/dashboards/uid/chronolog-stream',timeout=2) as response:
            dashboard=json.load(response)['dashboard']
            if len(dashboard['panels'])==3: break
    except (urllib.error.URLError,OSError,http.client.HTTPException): pass
    time.sleep(.1)
else: raise SystemExit('FAIL Grafana dashboard provisioning')
print('PASS Grafana CPU memory network dashboard',flush=True)
PY
timeout 30 "${compose[@]}" stop --timeout 15 chrono-stream-collect
timeout 30 "${compose[@]}" stop --timeout 15 chrono-stream-export
