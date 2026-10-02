#!/usr/bin/env bash
set -euo pipefail
root=$1
fail=0

if grep -rEn '#[[:space:]]*include[[:space:]]*[<"][^>"]*(\.pb\.h|grpcpp)' "$root/include/chronolog"; then
    echo 'Contract headers expose protobuf or gRPC' >&2
    fail=1
fi

# ARCHITECTURE.md M11.3: only src/chrono-common/rpc/ may depend on gRPC inside chrono-common.
if grep -rEn '#[[:space:]]*include[[:space:]]*[<"][^>"]*(\.pb\.h|grpcpp)' "$root/src/chrono-common" \
    --exclude-dir=rpc; then
    echo 'src/chrono-common/ outside rpc/ includes gRPC or protobuf' >&2
    fail=1
fi

if grep -rEn '#[[:space:]]*include[[:space:]]*[<"][^>"]*\.pb\.h' "$root/src/chrono-common/rpc" \
    --exclude='*_test.cpp'; then
    echo 'src/chrono-common/rpc/ non-test files include generated protobuf types' >&2
    fail=1
fi

# Every peer channel comes from rpc/: no channel creation in non-test service sources.
if grep -rEn 'CreateChannel|CreateCustomChannel' "$root"/src/chrono-* \
    --exclude-dir=tests --exclude='*_test.cpp' --exclude-dir=rpc; then
    echo 'Channel created outside src/chrono-common/rpc/' >&2
    fail=1
fi

exit $fail
