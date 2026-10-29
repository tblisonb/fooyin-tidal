#!/usr/bin/env bash
# Runs the panel's calls to TIDAL's official API inside fooyin's sandbox, with the plugin's account
# login, and prints what the panel would list. Doesn't start fooyin.
#
#   scripts/catalog-check.sh search <query>
#   scripts/catalog-check.sh favorite-tracks|favorite-albums|favorite-artists|playlists [--all]
#   scripts/catalog-check.sh album|playlist|artist <id> [--all]
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

[[ $# -ge 1 ]] || { sed -n '5,7s/^# *//p' "$0" >&2; exit 2; }
[[ -x build/tools/catalog-check ]] || { echo "Build it first: scripts/build.sh" >&2; exit 1; }

account="$HOME/.var/app/org.fooyin.fooyin/config/fooyin/tidal/account.json"
[[ -f "$account" ]] || { echo "No account login: log in under Settings > Integrations > TIDAL in fooyin" >&2; exit 1; }

flatpak run --filesystem="$repo" --filesystem="$(dirname "$account")" --env=LD_LIBRARY_PATH=/app/lib/fooyin \
    --command="$repo/build/tools/catalog-check" org.fooyin.fooyin "$account" "$@"
