#!/bin/sh
# Starts the binary for CHRONOLOG_ROLE. Extra arguments go to the binary.
set -eu
export GRPC_DNS_RESOLVER=${GRPC_DNS_RESOLVER:-native}
role=${CHRONOLOG_ROLE:-visor}
case "$role" in
    visor) bin=chrono_visor ;;
    keeper) bin=chrono_keeper ;;
    player) bin=chrono_player ;;
    grapher) bin=chrono_grapher ;;
    *)
        echo "chronolog-entrypoint: unknown CHRONOLOG_ROLE '$role' (visor, keeper, grapher, player)" >&2
        exit 64
        ;;
esac
path=/usr/local/bin/$bin
if [ ! -x "$path" ]; then
    echo "chronolog-entrypoint: $bin is not in this image (built targets: $(ls /usr/local/bin | grep -E '^chrono' | tr '\n' ' '))" >&2
    exit 65
fi
exec "$path" "$@"
