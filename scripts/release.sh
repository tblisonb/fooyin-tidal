#!/usr/bin/env bash
# Builds the plugin for the installed fooyin Flatpak, tags v<version> (from CMakeLists.txt) and
# publishes a GitHub release with the stripped plugin attached.
#
#   scripts/release.sh [NOTES.md]    # NOTES.md opens the release notes
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

notes_intro="${1:-}"
version="$(sed -nE '/^project\(/,/\)/ s/^ *VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
fooyin="$(flatpak info org.fooyin.fooyin | awk '/^ *Version:/ {print $2}')"
arch="$(flatpak info org.fooyin.fooyin | awk '/^ *Arch:/ {print $2}')"
tag="v$version"
asset="fyplugin_tidal-$version-fooyin-$fooyin-$arch.so"

[[ -n "$version" ]] || { echo "No version in CMakeLists.txt." >&2; exit 1; }
if [[ -n "$(git status --porcelain)" ]]; then
    echo "The working tree has changes; commit them first." >&2
    exit 1
fi
git fetch -q origin
if [[ "$(git rev-parse HEAD)" != "$(git rev-parse '@{u}')" ]]; then
    echo "HEAD isn't the same as its upstream; push (or pull) first." >&2
    exit 1
fi
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null || git ls-remote --exit-code --tags origin "$tag" >/dev/null; then
    echo "$tag already exists; raise the version in CMakeLists.txt." >&2
    exit 1
fi

scripts/build.sh
scripts/check.sh

out="build/release"
rm -rf "$out" && mkdir -p "$out"
scripts/sdk.sh strip --strip-unneeded -o "$out/$asset" "$(ls build/src/fyplugin_tidal.so* | head -1)"
chmod 644 "$out/$asset"
(cd "$out" && sha256sum "$asset" > "$asset.sha256")

notes="$out/notes.md"
{
    if [[ -n "$notes_intro" ]]; then
        cat "$notes_intro"
        echo
    fi
    echo "**Built for fooyin $fooyin** (the Flatpak \`org.fooyin.fooyin\`, $arch). fooyin's plugin"
    echo "interface changes between versions, so this build loads only into fooyin $fooyin; for another"
    echo "version, build from source (\`scripts/install.sh\`)."
    echo
    echo "Install:"
    echo
    echo '```sh'
    echo 'dest=~/.var/app/org.fooyin.fooyin/.local/lib/fooyin/plugins'
    echo "mkdir -p \"\$dest\" && install -m 644 $asset \"\$dest/fyplugin_tidal.so\""
    echo '```'
    echo
    echo "Then restart fooyin and set up the two logins in Settings > Integrations > TIDAL (see the"
    echo "[README](https://github.com/tblisonb/fooyin-tidal#setting-up))."
} > "$notes"

git tag -a "$tag" -m "fooyin-tidal $version"
git push -q origin "$tag"
gh release create "$tag" "$out/$asset" "$out/$asset.sha256" --title "fooyin-tidal $version" --notes-file "$notes"
echo "Released $tag: $(gh release view "$tag" --json url -q .url)"
