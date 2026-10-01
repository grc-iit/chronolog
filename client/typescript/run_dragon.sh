#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
package="$root/build/typescript/package"
mkdir -p "$package"
rsync -a --exclude=build --exclude=node_modules "$root/client/typescript/" "$package/"
cmake --install "$root/build/dev" --prefix "$package/build/sdk" --component client
cd "$package"
export npm_config_cache="$root/build/typescript/npm-cache"
if [ -f "$root/client/typescript/package-lock.json" ]; then
    timeout 180 npm ci --ignore-scripts
else
    timeout 180 npm install --ignore-scripts
fi
export npm_config_nodedir="$package/node_modules/.chronolog-node-headers"
export CMAKE_PREFIX_PATH="$package/build/sdk"
export CMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
export VCPKG_INSTALLED_DIR="$root/build/dev/vcpkg_installed"
timeout 240 npm run build
