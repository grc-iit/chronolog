#!/usr/bin/env bash
set -euo pipefail
if rg -n 'proto/chronolog/internal' "$1/client" \
    -g 'CMakeLists.txt' -g '*.cmake' -g '*.json' -g '*.toml' -g '*.gyp' -g '*.gypi' -g 'Makefile*' -g '*.sh' -g '*.yaml' -g '*.yml' \
    -g 'setup.py' -g 'build*.py' -g 'build*.js' -g 'build*.ts' -g 'build.rs'; then
    echo 'SDK or binding build references the internal proto' >&2
    exit 1
fi
