#!/usr/bin/env bash
# Builds the plugin against the installed fooyin Flatpak's version, inside its SDK.
#
#   scripts/build.sh           # builds build/src/fyplugin_tidal.so
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

version="$(flatpak info org.fooyin.fooyin | awk '/^ *Version:/ {print $2}')"
prefix=".deps/fooyin-$version"
[[ -f "$prefix/.complete" ]] || scripts/build-fooyin.sh "$version"

# A build directory made for another fooyin version is stale; start over.
if [[ -f build/CMakeCache.txt ]] && ! grep -q "Fooyin_DIR:PATH=$repo/$prefix/" build/CMakeCache.txt; then
    rm -rf build
fi

scripts/sdk.sh cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$repo/$prefix" \
    -DCMAKE_SKIP_BUILD_RPATH=ON
scripts/sdk.sh cmake --build build
