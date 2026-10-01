#!/usr/bin/env bash
# Binary acceptance: serve CreateChronicle, CreateStory and Acquire, keep incarnations
# increasing across a restart, exit 0 on SIGTERM.
# Usage: acceptance.sh <chrono_visor> <acceptance_client>
set -u
visor=$1
client=$2
dir=$(mktemp -d)
pid=
cleanup() { [ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$dir"; }
trap cleanup EXIT

port=$((10000 + (RANDOM % 4400) * 5))
internal=$((port + 1))
cat > "$dir/visor.json" <<JSON
{"listen": "127.0.0.1:$port", "internal_listen": "127.0.0.1:$internal", "db_path": "$dir/catalog.sqlite",
 "keepers": [{"process_id": "keeper-1", "endpoint": "127.0.0.1:50052"}]}
JSON

start() {
  "$visor" --config "$dir/visor.json" > "$dir/visor.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 50); do
    grep -q "catalog ready" "$dir/visor.log" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || { cat "$dir/visor.log"; return 1; }
    sleep 0.1
  done
  cat "$dir/visor.log"
  return 1
}

stop() {
  kill -TERM "$pid"
  wait "$pid"
  rc=$?
  pid=
  return $rc
}

expect() {
  [ "$2" = "$3" ] || { echo "FAIL $1: got '$2', want '$3'"; exit 1; }
  echo "ok $1: $2"
}

addr=127.0.0.1:$port
start || exit 1
"$client" "$addr" create-chronicle acceptance >/dev/null || exit 1
story=$("$client" "$addr" create-story acceptance s1) || exit 1
expect "first acquire" "$("$client" "$addr" acquire "$story" writer-a)" 1
expect "second acquire" "$("$client" "$addr" acquire "$story" writer-a)" 2
stop; expect "SIGTERM exit" $? 0

start || exit 1
expect "acquire after restart" "$("$client" "$addr" acquire "$story" writer-a)" 3
stop; expect "SIGTERM exit" $? 0
echo "ACCEPTANCE PASSED"
