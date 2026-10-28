#!/usr/bin/env bash
# Builds fooyin's development files (headers, CMake package, libraries) for the version of fooyin's
# Flatpak that's installed, into .deps/fooyin-<version>. The plugin is compiled against these; at
# runtime it uses the libraries already loaded by fooyin itself.
#
# Run it once, and again after fooyin's Flatpak updates to a new version.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

version="${1:-$(flatpak info org.fooyin.fooyin | awk '/^ *Version:/ {print $2}')}"
src=".deps/src/fooyin-$version"
prefix=".deps/fooyin-$version"

if [[ -f "$prefix/.complete" ]]; then
    echo "fooyin $version is already built in $prefix"
    exit 0
fi

if [[ ! -d "$src" ]]; then
    mkdir -p .deps/src
    git clone --quiet --depth 1 --branch "v$version" https://github.com/fooyin/fooyin.git "$src"
fi

# fooyin fetches KDSingleApplication while configuring, but the SDK sandbox has no network, so
# fetch it here at the tag fooyin asks for and hand CMake the source.
kdsa_tag="$(sed -n 's/^ *GIT_TAG *//p' "$src/3rdparty/kdsingleapplication.cmake")"
kdsa=".deps/src/kdsingleapplication-$kdsa_tag"
if [[ ! -d "$kdsa" ]]; then
    git clone --quiet --depth 1 --branch "$kdsa_tag" https://github.com/KDAB/KDSingleApplication.git "$kdsa"
fi

scripts/sdk.sh cmake -S "$src" -B "$src/build" -G Ninja \
    -DFETCHCONTENT_SOURCE_DIR_KDSINGLEAPPLICATION="$repo/$kdsa" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$repo/$prefix" \
    -DINSTALL_HEADERS=ON \
    -DBUILD_PLUGINS=OFF \
    -DBUILD_TRANSLATIONS=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_CCACHE=OFF
scripts/sdk.sh cmake --build "$src/build"
scripts/sdk.sh cmake --install "$src/build"
touch "$prefix/.complete"
echo "Built fooyin $version into $prefix"
