#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
export CHRONOLOG_TYPESCRIPT_CATALOG=$1 CHRONOLOG_TYPESCRIPT_PLAYER=$2
exec timeout 60 node --test --test-timeout=55000 "$root/build/typescript/package/test/client.test.js"
