#!/bin/sh
# Starts the binary for CHRONOLOG_ROLE. Extra arguments go to the binary.
set -eu
role=${CHRONOLOG_ROLE:-stub}
case "$role" in
    visor) bin=chrono_visor ;;
    keeper) bin=chrono_keeper ;;
    player) bin=chrono_player ;;
    stub) bin=chronolog_stub_server ;;
    *)
        echo "chronolog-entrypoint: unknown CHRONOLOG_ROLE '$role' (visor, keeper, player, stub)" >&2
        exit 64
        ;;
esac
path=/usr/local/bin/$bin
if [ ! -x "$path" ]; then
    echo "chronolog-entrypoint: $bin is not in this image (built targets: $(ls /usr/local/bin | grep -E '^chrono' | tr '\n' ' '))" >&2
    exit 65
fi
exec "$path" "$@"
