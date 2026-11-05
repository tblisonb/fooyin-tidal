# fooyin-tidal

A [TIDAL](https://tidal.com) plugin for the [fooyin](https://github.com/fooyin/fooyin) music player:
search TIDAL, play it alongside your own library at up to 24-bit FLAC, and manage your TIDAL
playlists and favorites from fooyin.

This is an independent third-party plugin. It isn't part of fooyin or affiliated with TIDAL, so please
don't take problems with it to either project. It needs a paid TIDAL subscription.

**Playback uses TIDAL's private API with a client from one of TIDAL's own apps.** TIDAL doesn't
sanction that, and it can stop working whenever TIDAL changes the API or revokes the client. If it
does, search, favorites and playlists keep working (they use TIDAL's official API); only playback
stops. See [docs/design.md](docs/design.md) for why and how.

## Features

- A **TIDAL panel**: search, your favorite tracks, albums and artists, and your playlists (mixes
  included). Double-click a track to play it and the rest of its list after it. Albums, playlists
  and artists open in place, with **Back** to return.
- **Playback** of TIDAL tracks as ordinary fooyin playlist entries, mixed freely with local files,
  with seeking and album covers wherever fooyin shows artwork.
- **Editing your TIDAL playlists and favorites** from fooyin's own track menu, anywhere you
  right-click tracks, and from the **Playback** menu for the track that's playing. Your own music
  files are matched to TIDAL tracks first.
- Sortable, customizable columns, and lists cached on disk so they open straight away.

## Install

The plugin works with **fooyin's Flatpak** (`org.fooyin.fooyin`). fooyin's plugin interface changes
between versions, so a plugin build only loads into the fooyin version it was built for.

**From a release:** download `fyplugin_tidal-<version>-fooyin-<fooyin version>-x86_64.so` from
[Releases](https://github.com/tblisonb/fooyin-tidal/releases), for the fooyin version you have
(`flatpak info org.fooyin.fooyin`), then:

```sh
dest=~/.var/app/org.fooyin.fooyin/.local/lib/fooyin/plugins
mkdir -p "$dest" && install -m 644 fyplugin_tidal-*.so "$dest/fyplugin_tidal.so"
```

**From source**, for whatever fooyin version is installed (see [Building](#building)):

```sh
scripts/install.sh
```

Restart fooyin afterwards. After fooyin updates, install the build for the new version (or run
`scripts/install.sh` again).

## Setting up

The plugin uses two TIDAL logins, in **Settings > Integrations > TIDAL**:

1. **TIDAL Account**, for the panel's search, favorites and playlists, with a developer app of
   your own:
   - Sign in at <https://developer.tidal.com/dashboard> and create an app.
   - Add the redirect URI `http://localhost:47130/callback`, and enable the scopes it offers for
     user data, collections, playlists, search and playback.
   - Put the app's client ID in the settings and press **Log In**. TIDAL's login page opens in your
     browser and comes back to fooyin by itself.
2. **Playback**, with the client ID and secret of one of TIDAL's own apps (developer apps only get
   29-second previews). This plugin doesn't come with one: use one of the PKCE clients other
   open-source TIDAL players use, such as python-tidal's. **Log In** opens TIDAL's login page,
   which ends on an "Oops" page. Copy that page's whole address and go back to fooyin: the login
   finishes by itself. (Or paste the address into the box under it and press **Finish Login**.)

With both client IDs filled in, **Log In to Both** does the two logins one after the other: the
browser opens the account login, then, once that's done, the playback login. Your browser is still
logged in to TIDAL from the first, so the second usually goes straight to the "Oops" page, without
your password or an emailed code.

The logins are kept in `~/.var/app/org.fooyin.fooyin/config/fooyin/tidal/` (`account.json` and
`session.json`), readable only by you.

## Using it

Open **View > TIDAL**, or put the panel in your layout. To have it as a tab next to your playlist:
**Layout > Editing mode**, right-click the playlist, **Split > Tab Stack**, then right-click the new
tab bar, **Insert inside > Internet > TIDAL**, and turn editing mode off.

Pick favorites or playlists from the list at the top, or type a search and press Enter.
Double-click a track to play it and the rest of its list after it, in the order shown; double-click
an album, playlist or artist to open it, and **Back** to return. Click a column heading to sort
(**Added** sorts by when tracks went into the playlist or favorites); the sort is kept for every
list. Right-click a column heading to show or hide columns (Album Artist, Track No. and Release
Date are there too), turn auto-sizing off to set widths yourself, or reset them; drag headings to
reorder. Each TIDAL panel keeps its own columns, saved with fooyin's layout. Right-click tracks for
fooyin's usual track menu, or an album or playlist to play it, add it to the current playlist, or
send it to a new one.

### Editing playlists and favorites

Your TIDAL playlists and favorites can be edited from fooyin's track menu, anywhere you right-click
tracks (playlists, the library, the TIDAL panel, …), and from the **Playback** menu for the track
that's playing:

- **Add to TIDAL Playlist:** one of your own TIDAL playlists, or **New Playlist…**. In the Playback
  menu it's **Add Playing Track to TIDAL Playlist**.
- **Add to TIDAL Favorites** / **Remove from TIDAL Favorites** (the Playback menu has **Add Playing
  Track to TIDAL Favorites**, which can be given a shortcut under Settings > Shortcuts).
- In the panel, albums, artists and other people's playlists can be added to or removed from your
  favorites too, and inside one of your playlists **Remove from This TIDAL Playlist** removes the
  selected entries.

Tracks that aren't TIDAL's (your own files) are looked up on TIDAL first: by ISRC when the file has
one, else by searching for the artist and title and taking a track with the same title, artist and
length (within 3 seconds), preferring the same album. Tracks with no such match are left out, and
the message says which.

Changes are made on TIDAL right away. The panel's status line (and fooyin's status bar, if your
layout has one) says how each went, and the lists it changes are checked with TIDAL again. New
playlists are unlisted: anyone with the link can see them, but they aren't shown on your profile.

### Caches

Lists the panel has fetched are kept in `~/.var/app/org.fooyin.fooyin/cache/fooyin/tidal/`, so they
open straight away next time; TIDAL is then asked (one quick request) whether the list changed, and
it's fetched again only if it did. The reload button fetches the list again regardless.

Album covers are downloaded at 1280×1280 and kept in `cache/fooyin/tidal/covers/`, one per album;
covers not shown for 60 days are deleted.

**File > Add TIDAL track…** adds a single track by link (such as
`https://tidal.com/browse/track/58990486`) or ID to the current playlist.

## Building

fooyin is installed as a Flatpak, so the plugin is built inside the same SDK fooyin is built with
(`org.kde.Sdk`, installed for you on first use), against fooyin's own headers for the installed
version.

```sh
scripts/build.sh     # first run also builds fooyin's development files into .deps/ (a few minutes)
scripts/check.sh     # loads the plugin inside fooyin's sandbox, without starting fooyin
scripts/install.sh   # builds, checks, and copies it to ~/.var/app/org.fooyin.fooyin/.local/lib/fooyin/plugins
```

The build notices when fooyin's version changes and rebuilds against the new one.

`scripts/release.sh` builds a stripped copy for the installed fooyin version, tags `v<version>`
(from `CMakeLists.txt`) and publishes it as a GitHub release.

### Development tools

These run the plugin's code inside fooyin's sandbox without starting fooyin, with the logins from
fooyin's settings:

- `scripts/catalog-check.sh search <words>` (or `favorite-tracks`, `playlists`, `album <id>`, …)
  prints what the panel would list; `--version` prints what the cache compares, `--cache <dir>`
  stores a list in a cache and reads it back. It makes the panel's edits too, **on your account**:
  `create-playlist <name>`, `playlist-add` and `playlist-remove <playlist> <track>...`, `favorite`
  and `unfavorite <track|album|artist|playlist> <id>...`; `find <title> <artist> <album> <length ms>
  [ISRC]` shows which TIDAL track a file would be matched to; `raw <METHOD> <path> [json]` sends
  any request.
- `scripts/stream-check.sh <track ID> [start ms]` streams a track with the plugin's code and checks
  the result with ffprobe; with a start position it streams from there, as a seek does.
- `scripts/cover-check.sh <track ID>` reads a track's details and cover with the plugin's reader,
  into `build/cover-check/`, twice: the second read should come from its cache.
- `tools/tidal-probe.py` is the stand-alone probe the plugin started from (its own logins, in
  `~/.config/fooyin-tidal/probe.json`): `login`, `search`, `report <track>`, `manifest <track>`,
  `login-v1 --pkce|--device`, `v1 <track>`. What it found is in [docs/design.md](docs/design.md).

## License

GPL-3.0-or-later, like fooyin.
