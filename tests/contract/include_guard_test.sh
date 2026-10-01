#!/usr/bin/env bash
set -euo pipefail
if rg -n '#[[:space:]]*include[[:space:]]*[<"][^>"]*(\.pb\.h|grpcpp)' "$1/include/chronolog"; then
    echo 'Contract headers expose protobuf or gRPC' >&2
    exit 1
fi
