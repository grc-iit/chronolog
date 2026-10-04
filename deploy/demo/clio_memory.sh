#!/usr/bin/env bash
# Clio-coder shared-memory demo: one headless session remembers three facts in ChronoLog, a second, fresh session
# recalls them, and the script checks that the recall is complete and returns the same three event ids.
#   PATH must hold the install prefix's bin (chronolog, chronolog-mcp) and clio-coder.
#   CLIO_PROVIDER_URL   OpenAI-compatible endpoint of the model server (default http://mini:8080/v1)
#   CLIO_MODEL          model id served there (default qwopus3.8v2-27b-dense-q4km)
# The run uses a scratch CLIO_CODER_HOME and XDG_STATE_HOME under a temp dir, so neither your Clio-coder config nor
# your default ChronoLog instance is touched. It prints PASS clio-memory or FAIL <reason>, stops the instance and
# removes the temp dir. Exit status 0 means PASS.
set -uo pipefail

provider_url=${CLIO_PROVIDER_URL:-http://mini:8080/v1}
model=${CLIO_MODEL:-qwopus3.8v2-27b-dense-q4km}
reason=""

for tool in chronolog chronolog-mcp clio-coder python3; do
    command -v "$tool" > /dev/null 2>&1 || { echo "FAIL $tool is not on PATH"; exit 1; }
done
chronolog_bin=$(command -v chronolog)
mcp_bin=$(command -v chronolog-mcp)

tmp=$(mktemp -d "${TMPDIR:-/tmp}/clio-memory.XXXXXX")
unset CHRONOLOG_HOME
export CLIO_CODER_HOME=$tmp/clio XDG_STATE_HOME=$tmp/xdg
mkdir -p "$CLIO_CODER_HOME/config" "$XDG_STATE_HOME" "$tmp/work"

cat > "$CLIO_CODER_HOME/config/settings.yaml" << EOF
version: 2
targets:
  - id: demo
    runtime: llamacpp
    url: $provider_url
    defaultModel: $model
    lifecycle: user-managed
chat:
  target: demo
  model: $model
EOF

cat > "$CLIO_CODER_HOME/config/mcp.yaml" << EOF
version: 1
servers:
  - id: chronolog
    command: $chronolog_bin
    args: [run, --up, default, --, $mcp_bin]
    env:
      CHRONOLOG_MCP_IDENTITY: clio/main
      CHRONOLOG_CHRONICLE: agent-memory
    actionClass: execute
EOF

stop_stack() {
    local out
    out=$(timeout 240 chronolog down 2>&1) || { echo "chronolog down: $out" >&2; return 1; }
}

clio_session() {
    (cd "$tmp/work" && timeout 660 clio-coder run --no-delegate --autonomy yolo --timeout 600 --json-events terminal \
        "$2" > "$tmp/s$1.jsonl" 2> "$tmp/s$1.err")
    local status=$?
    if [ $status -ne 0 ]; then
        reason="session $1 exited with status $status: $(tail -n 3 "$tmp/s$1.err" | tr '\n' ' ')"
        return 1
    fi
}

check() {
    python3 - "$CLIO_CODER_HOME" "$tmp/s1.jsonl" "$tmp/s2.jsonl" << 'PY'
import glob, json, sys

home, remember_events, recall_events = sys.argv[1:4]
facts = ['alpha: the build host is dragon', 'beta: the gate holds mini', 'gamma: reports go to orch/out']
operations = ['clio-memory-1', 'clio-memory-2', 'clio-memory-3']


def ledger(events_path):
    for line in open(events_path):
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get('type') == 'session':
            found = glob.glob(f"{home}/state/sessions/*/{event['id']}/current.jsonl")
            if found:
                return found[0]
    return None


def tool_results(path):
    for line in open(path):
        entry = json.loads(line)
        if entry.get('kind') != 'message' or entry.get('role') != 'tool_result' or entry['payload'].get('isError'):
            continue
        try:
            body = json.loads(entry['payload']['result']['content'][0]['text'])
        except (ValueError, KeyError, IndexError, TypeError):
            continue
        if isinstance(body, dict):
            yield body


def event_id(identifier):
    return tuple(identifier[key] for key in ('story_id', 'writer_id', 'incarnation', 'sequence'))


def fail(message):
    print(message)
    sys.exit(1)


first, second = ledger(remember_events), ledger(recall_events)
if first is None or second is None:
    fail('no session ledger found for session ' + ('1' if first is None else '2'))

stored = {}
for body in tool_results(first):
    if body.get('stored') == 'durable' and body.get('operation_id') in operations:
        stored[body['operation_id']] = event_id(body['receipt']['event_id'])
if sorted(stored) != operations or len(set(stored.values())) != 3:
    fail(f'session 1 stored {len(stored)} of 3 memories durably: {sorted(stored)}')

recall = None
for body in tool_results(second):
    if 'events' in body:
        recall = body
if recall is None:
    fail('session 2 made no successful context_recall')
if recall.get('answer_complete') is not True:
    fail(f"session 2 recall answer_complete is {recall.get('answer_complete')}")
recalled = [event_id(event['id']) for event in recall['events']]
if sorted(recalled) != sorted(stored.values()):
    fail(f'session 2 recalled event ids {sorted(recalled)}, session 1 stored {sorted(stored.values())}')
texts = [event.get('content', {}).get('data') for event in recall['events']]
if sorted(texts) != sorted(facts):
    fail(f'session 2 recalled texts {texts}')
PY
}

demo() {
    clio_session 1 'Use the chronolog MCP server through the gateway. Call context_open with name clio-memory, create true and access read_write, and keep the session_handle it returns. Then call context_remember three times with that session_handle, operation ids clio-memory-1, clio-memory-2 and clio-memory-3, and these texts in order: "alpha: the build host is dragon", "beta: the gate holds mini", "gamma: reports go to orch/out". Print each remember verdict and EventId exactly as returned.' || return 1
    clio_session 2 'Use the chronolog MCP server through the gateway. Call context_open with name clio-memory and keep the session_handle it returns. Then call context_recall with that session_handle. Print the result verdict and answer_complete fields, then every event with its text and its EventId exactly as returned. Do not call context_remember.' || return 1
    reason=$(check) || return 1
}

trap 'stop_stack > /dev/null 2>&1; rm -rf "$tmp"; echo "FAIL interrupted"; exit 130' INT TERM

demo
status=$?
if ! stop_stack && [ $status -eq 0 ]; then
    reason="chronolog down failed"
    status=1
fi
rm -rf "$tmp"
if [ $status -eq 0 ]; then
    echo "PASS clio-memory"
else
    echo "FAIL $reason"
fi
exit $status
