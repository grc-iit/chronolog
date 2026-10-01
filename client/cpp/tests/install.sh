#!/usr/bin/env bash
set -eu
build_dir=$1
source_dir=$2
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cmake --install "$build_dir" --prefix "$scratch/sdk" --component client
cp -r "$source_dir/consumer" "$scratch/consumer"
cd "$scratch/consumer"
for static in OFF ON; do
    cmake --preset dev -DCMAKE_PREFIX_PATH="$scratch/sdk" -DVCPKG_INSTALLED_DIR="$build_dir/vcpkg_installed" -DUSE_STATIC="$static"
    cmake --build --preset dev --parallel 2
    timeout 10 build/dev/consumer
done
