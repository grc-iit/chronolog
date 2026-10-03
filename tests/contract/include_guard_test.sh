#!/usr/bin/env bash
set -euo pipefail
root=$1
fail=0

# grep status 2 is a failed check, never a clean tree.
check() {
    local message=$1 status=0
    shift
    grep -rEn "$@" || status=$?
    case $status in
        0) echo "$message" >&2; fail=1 ;;
        1) ;;
        *) echo "Include guard could not inspect sources: $message" >&2; fail=1 ;;
    esac
}

wire='#[[:space:]]*include[[:space:]]*[<"][^>"]*(\.pb\.h|grpcpp/|grpc/|google/protobuf/)'
grpc='#[[:space:]]*include[[:space:]]*[<"][^>"]*(\.grpc\.pb\.h|grpcpp/|grpc/)'
protobuf='#[[:space:]]*include[[:space:]]*[<"][^>"]*(\.pb\.h|google/protobuf/)'
check 'Contract headers expose protobuf or gRPC' "$wire" "$root/include/chronolog"

# M11.3: rpc/ may use gRPC; tier/ may use protobuf message types only.
check 'src/chrono-common/ outside rpc/ and tier/ includes gRPC or protobuf' \
    "$wire" "$root/src/chrono-common" --exclude-dir=rpc --exclude-dir=tier
check 'src/chrono-common/tier/ includes gRPC' "$grpc" "$root/src/chrono-common/tier"
check 'src/chrono-common/rpc/ non-test files include protobuf types' \
    "$protobuf" "$root/src/chrono-common/rpc" --exclude-dir=tests --exclude='*_test.cpp'

# M11.2 applies to production sources; cross-service test binaries remain a follow-up.
for service in visor keeper grapher player; do
    others=''
    for peer in visor keeper grapher player; do
        if [ "$peer" != "$service" ]; then
            others+="${others:+|}$peer"
        fi
    done
    check "src/chrono-$service/ includes another service" \
        "#[[:space:]]*include[[:space:]]*[<\"][^>\"]*chrono-($others)/" \
        "$root/src/chrono-$service" --exclude-dir=tests --exclude='*_test.cpp'
done

# Every peer channel comes from rpc/: no channel creation in non-test service sources.
check 'Channel created outside src/chrono-common/rpc/' \
    'CreateChannel|CreateCustomChannel' "$root"/src/chrono-* \
    --exclude-dir=tests --exclude='*_test.cpp' --exclude-dir=rpc

exit "$fail"
