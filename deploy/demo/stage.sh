#!/usr/bin/env bash
# Shared by tests/smoke/run.sh and deploy/demo/chronolog-demo.
#   stage.sh targets                  prints the CMake targets the runtime image and the demo CLIs need
#   stage.sh binaries ROOT            fills ROOT/build/image-stage and ROOT/build/demo-bin from ROOT/build/dev
#   stage.sh image ENGINE ROOT IMAGE  builds the runtime image from ROOT/build/image-stage
set -euo pipefail

runtime_targets=(chrono_visor chrono_keeper chrono_player chrono_grapher chronolog_kvs_example chronolog_pubsub_example
                 chronolog_sql_example chronolog_stream_collect chronolog_stream_export chronolog_stream_example
                 chronolog_ldms_fake_ldmsd chronolog_ldms_example)
cli_targets=(chronolog_kvs_cli chronolog_sql_cli chronolog_pubsub_example chronolog_admin)

binary_name() {
    case "$1" in
        chronolog_kvs_cli) echo chronolog_kvs ;;
        chronolog_sql_cli) echo chronolog_sql ;;
        *) echo "$1" ;;
    esac
}

find_binary() {
    local found
    # Newest first: a build directory configured before a source move still holds the old outputs.
    found=$(find "$1/build/dev" -type f -name "$2" -perm -u+x -printf '%T@ %p\n' | sort -rn | head -n 1 | cut -d' ' -f2-)
    [ -n "$found" ] || { echo "stage: $2 was not built"; exit 1; }
    echo "$found"
}

case "${1:-}" in
    targets)
        printf '%s\n' "${runtime_targets[@]}" chronolog_kvs_cli chronolog_sql_cli chronolog_admin
        ;;
    binaries)
        root=${2:?root}
        stage=$root/build/image-stage
        rm -rf "$stage" "$root/build/demo-bin" "$root/build/lib"
        mkdir -p "$stage" "$root/build/demo-bin" "$root/build/lib"
        for target in "${runtime_targets[@]}"; do
            cp "$(find_binary "$root" "$target")" "$stage/"
        done
        cp "$root/deploy/containers/entrypoint.sh" "$stage/"
        cp -L "$root/build/dev/client/cpp/libchronolog_client.so.4" "$stage/"
        cp -L "$root/build/dev/client/cpp/libchronolog_client.so.4" "$root/build/lib/"
        ln -sf libchronolog_client.so.4 "$root/build/lib/libchronolog_client.so"
        for target in "${cli_targets[@]}"; do
            name=$(binary_name "$target")
            cp "$(find_binary "$root" "$name")" "$root/build/demo-bin/"
        done
        ;;
    image)
        engine=${2:?engine} root=${3:?root} image=${4:?image}
        "$engine" build -f "$root/deploy/containers/runtime-local.Containerfile" -t "$image" "$root/build/image-stage"
        ;;
    *)
        echo "usage: stage.sh targets | binaries ROOT | image ENGINE ROOT IMAGE" >&2
        exit 2
        ;;
esac
