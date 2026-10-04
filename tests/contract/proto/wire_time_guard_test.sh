#!/usr/bin/env bash
# W10.9: "All timestamps on the wire are int64 nanoseconds, authority_tick_ns included. No Duration or Timestamp
# well-known types in `chronolog.v1`, so SDKs in every language compare plain integers." Fails if a .proto under
# proto/chronolog/v1 imports google/protobuf/duration.proto or timestamp.proto or names either type.
set -euo pipefail
violations() {
    local file
    while IFS= read -r -d '' file; do
        # Comments are stripped first, so a comment that names a type is not a use of it.
        sed -e 's|//.*||' "$file" |
            grep -nE 'google/protobuf/(duration|timestamp)\.proto|(^|[^A-Za-z0-9_])(Duration|Timestamp)([^A-Za-z0-9_]|$)' |
            sed -e "s|^|$file:|" || true
    done < <(find "$1" -name '*.proto' -print0)
}
v1="$1/proto/chronolog/v1"
[ -n "$(find "$v1" -name '*.proto' -print -quit)" ] || { echo "no .proto files under $v1" >&2; exit 1; }
# The matcher must see each forbidden form, or an empty result proves nothing.
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
for form in 'import "google/protobuf/duration.proto";' 'import "google/protobuf/timestamp.proto";' \
    'google.protobuf.Duration lease = 1;' '.google.protobuf.Timestamp at = 2;' 'repeated Timestamp at = 3;'; do
    printf 'syntax = "proto3";\n// Duration and Timestamp in a comment are fine.\n%s\n' "$form" > "$probe/probe.proto"
    [ -n "$(violations "$probe")" ] || { echo "guard does not detect: $form" >&2; exit 1; }
done
printf 'syntax = "proto3";\n// Duration and Timestamp in a comment are fine.\nint64 lease_duration_ns = 1;\n' > "$probe/probe.proto"
[ -z "$(violations "$probe")" ] || { echo "guard flags plain int64 nanoseconds" >&2; exit 1; }
found=$(violations "$v1")
if [ -n "$found" ]; then
    echo "W10.9: chronolog.v1 uses a Duration or Timestamp well-known type:" >&2
    echo "$found" >&2
    exit 1
fi
