#!/usr/bin/env bash
# Reads one TIDAL track's album cover with the plugin's own reader, inside fooyin's sandbox and
# with the plugin's playback login, into build/cover-check/<id>.jpg. Uses a cover cache of its own
# (build/cover-check/cache), so the first read downloads and the second comes from the cache.
# Doesn't start fooyin.
#
#   scripts/cover-check.sh <track id or link>
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

[[ $# -eq 1 ]] || { echo "usage: scripts/cover-check.sh <track id or link>" >&2; exit 2; }
[[ -x build/tools/cover-check ]] || { echo "Build it first: scripts/build.sh" >&2; exit 1; }

session="$HOME/.var/app/org.fooyin.fooyin/config/fooyin/tidal/session.json"
[[ -f "$session" ]] || { echo "No playback login for the plugin: log in under Settings > Integrations > TIDAL in fooyin" >&2; exit 1; }

mkdir -p build/cover-check/cache
id="$(basename "$1")"
out="$repo/build/cover-check/${id%%\?*}.jpg"

flatpak run --filesystem="$repo" --filesystem="$(dirname "$session")" --env=LD_LIBRARY_PATH=/app/lib/fooyin \
    --command="$repo/build/tools/cover-check" org.fooyin.fooyin "$session" "$repo/build/cover-check/cache" "$1" "$out"
