#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
if [ ! -f "$root/build/typescript/package/build/dev/chronolog_node.node" ]; then
    echo 'Build client/typescript/run.sh before the TypeScript acceptance test'
    exit 77
fi
exec timeout 90 bash "$root/client/cpp/tests/acceptance.sh" "$@" "$root/client/typescript/test/run_stack_client.sh"
