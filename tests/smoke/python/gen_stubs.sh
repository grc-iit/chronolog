#!/usr/bin/env bash
# Generates Python gRPC stubs for the public chronolog.v1 API only.
# Usage: gen_stubs.sh <out_dir> [proto_root]
# The internal protos (chronolog.internal.v1) are never generated here.
set -euo pipefail

out=${1:?usage: gen_stubs.sh <out_dir> [proto_root]}
root=${2:-"$(cd "$(dirname "$0")/../../.." && pwd)/proto"}
python=${PYTHON:-python3}

protos=()
for file in "$root"/chronolog/v1/*.proto; do
    [ -e "$file" ] && protos+=("${file#"$root"/}")
done
[ "${#protos[@]}" -gt 0 ] || { echo "gen_stubs: no .proto files under $root/chronolog/v1" >&2; exit 1; }

for file in "${protos[@]}"; do
    case "$file" in
        chronolog/v1/*) ;;
        *) echo "gen_stubs: refusing non-public proto $file" >&2; exit 1 ;;
    esac
done
if grep -lq "chronolog.internal" "$root"/chronolog/v1/*.proto 2>/dev/null; then
    echo "gen_stubs: a public proto references chronolog.internal" >&2
    exit 1
fi

mkdir -p "$out"
"$python" -m grpc_tools.protoc "-I$root" "--python_out=$out" "--grpc_python_out=$out" "${protos[@]}"
touch "$out/chronolog/__init__.py" "$out/chronolog/v1/__init__.py"
echo "gen_stubs: generated ${#protos[@]} file(s) into $out"
