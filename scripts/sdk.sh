#!/usr/bin/env bash
# Runs a command inside the Flatpak SDK that fooyin's Flatpak is built with, so what we build there
# links against the same Qt and libraries fooyin loads at runtime.
#
#   scripts/sdk.sh <command> [args...]
#
# The repository is mounted at its own path and is the working directory.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"

sdk="$(flatpak info --show-sdk org.fooyin.fooyin 2>/dev/null || true)"
if [[ -z "$sdk" ]]; then
    echo "fooyin's Flatpak (org.fooyin.fooyin) isn't installed." >&2
    exit 1
fi
# flatpak info prints org.kde.Sdk/x86_64/6.11; flatpak run wants org.kde.Sdk//6.11.
sdk_ref="${sdk%%/*}//${sdk##*/}"

if ! flatpak info "$sdk_ref" >/dev/null 2>&1; then
    echo "Installing $sdk_ref for this user (a one-time download of about 1-2 GB)..." >&2
    flatpak install --user -y --noninteractive flathub "$sdk_ref"
fi

# Runs without network, as a runtime on its own does: fetch anything a build needs beforehand.
exec flatpak run --filesystem="$repo" --command=bash "$sdk_ref" -c 'cd "$0" && exec "$@"' "$repo" "$@"
