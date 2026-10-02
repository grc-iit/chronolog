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
pc=$(find "$scratch/sdk" -name chronolog.pc | head -n 1)
[ -n "$pc" ] || { echo "install: chronolog.pc was not installed"; exit 1; }
if grep -qi thallium "$pc"; then echo "install: chronolog.pc still requires thallium"; exit 1; fi
lib=$(find "$scratch/sdk" -name 'libchronolog_client.so' | head -n 1)
vcpkg_pc=$(ls -d "$build_dir"/vcpkg_installed/*/lib/pkgconfig | grep -v '/vcpkg/' | paste -sd:)
export PKG_CONFIG_PATH="$(dirname "$pc"):$vcpkg_pc"
"${CXX:-c++}" -std=c++20 "$source_dir/consumer/main.cpp" -o "$scratch/pc_consumer" $(pkg-config --cflags --libs --static chronolog) \
    -Wl,-rpath,"$(dirname "$lib")" -pthread
timeout 10 "$scratch/pc_consumer"
