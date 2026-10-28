#!/usr/bin/env bash
# Streams one TIDAL track with the plugin's own code, inside fooyin's sandbox and with the plugin's
# playback login, into build/stream-check/<id>.mp4, then checks the file with ffprobe if it's
# installed. Doesn't start fooyin.
#
#   scripts/stream-check.sh <track id or link> [start ms]   # with a start: as a seek would
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

[[ $# -eq 1 || $# -eq 2 ]] || { echo "usage: scripts/stream-check.sh <track id or link> [start ms]" >&2; exit 2; }
[[ -x build/tools/stream-check ]] || { echo "Build it first: scripts/build.sh" >&2; exit 1; }

session="$HOME/.var/app/org.fooyin.fooyin/config/fooyin/tidal/session.json"
[[ -f "$session" ]] || { echo "No playback login for the plugin: run tools/tidal-probe.py export-session" >&2; exit 1; }

mkdir -p build/stream-check
id="$(basename "$1")"
out="$repo/build/stream-check/${id%%\?*}.mp4"

flatpak run --filesystem="$repo" --filesystem="$(dirname "$session")" --env=LD_LIBRARY_PATH=/app/lib/fooyin \
    --command="$repo/build/tools/stream-check" org.fooyin.fooyin "$session" "$1" "$out" ${2:+"$2"}

if command -v ffprobe >/dev/null; then
    ffprobe -v error -show_entries stream=codec_name,sample_rate,bits_per_raw_sample,channels:format=duration \
        -of compact "$out"
    echo "first timestamp: $(ffprobe -v error -show_entries packet=pts_time -of csv=p=0 -read_intervals '%+#1' "$out")"
fi
