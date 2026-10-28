#!/usr/bin/env bash
# Installs the built plugin where fooyin's Flatpak looks for your own plugins. Restart fooyin to load it.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

scripts/check.sh
dest="$HOME/.var/app/org.fooyin.fooyin/.local/lib/fooyin/plugins"
mkdir -p "$dest"
install -m 644 "$(ls build/src/fyplugin_tidal.so* | head -1)" "$dest/fyplugin_tidal.so"
echo "Installed to $dest. Restart fooyin to load it."
