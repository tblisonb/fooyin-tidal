#!/usr/bin/env bash
# Checks that the built plugin loads in the installed fooyin Flatpak: same Qt, same fooyin libraries,
# every symbol resolved. Doesn't start fooyin.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

plugin="$(ls build/src/fyplugin_tidal.so* 2>/dev/null | head -1)"
[[ -n "$plugin" ]] || { echo "Build it first: scripts/build.sh" >&2; exit 1; }

# fooyin's libraries live in /app/lib/fooyin inside its sandbox; the check tool must use those, not
# the copies built from source in .deps.
exec flatpak run --filesystem="$repo" --env=LD_LIBRARY_PATH=/app/lib/fooyin --env=QT_QPA_PLATFORM=offscreen \
    --command="$repo/build/tools/plugin-check" org.fooyin.fooyin "$repo/$plugin"
