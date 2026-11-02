# fooyin-tidal

A [TIDAL](https://tidal.com) plugin for the [fooyin](https://github.com/fooyin/fooyin) music player:
search TIDAL, play it alongside your own library, and manage your TIDAL playlists and favorites from
fooyin.

This is an independent third-party plugin. It isn't part of fooyin or affiliated with TIDAL, so please
don't take problems with it to either project.

**Status:** early (phase 1 under way). The plugin plays TIDAL tracks you add by link or ID
(**File > Add TIDAL track…**), at up to 24-bit FLAC, with seeking, logs in from fooyin's settings,
and has a TIDAL panel for search, favorites and playlists. Editing playlists and favorites is next.

Design notes, what was learned about fooyin and TIDAL, and the plan ahead are in
[docs/design.md](docs/design.md).

## How it fits together

- **Search, browsing, favorites and playlists** use TIDAL's official
  [developer API](https://developer.tidal.com) (`openapi.tidal.com/v2`), signed in with your own
  developer app.
- **Playback** registers a `tidal:` URL scheme with fooyin. When a TIDAL track starts, the plugin asks
  TIDAL's private v1 API for the track's stream, with a second login made with a client of one of
  TIDAL's own apps (developer apps only get previews; see below). The stream is DASH: the plugin
  downloads its segments in order, and fooyin's own FFmpeg decoder plays them as one MP4.

## Phase 0: what needs answering

1. Can a developer app log in from the desktop (PKCE with a `localhost` redirect)?
2. Does the official API's `/trackManifests/{id}` give such an app streams **without DRM**? Its
   `drmData` field is documented as "absence implies no DRM". If it does, the plugin needs no
   private API at all.
3. If it doesn't: does TIDAL's private v1 API (the one Strawberry uses for playback) accept the same
   login, and do the two APIs share track IDs?

### What the probe found (2026-10-27, fooyin 0.14.1)

1. **Login works** with PKCE and a `localhost` redirect, as long as the app asks only for scopes the
   developer dashboard offers (asking for the legacy `r_usr` got login error 1002).
2. **v2 manifests have no DRM, but they're previews.** `/trackManifests/{id}` gives FLAC (16-bit
   44.1 kHz) or AAC in DASH or HLS, with track and album ReplayGain values, but every track comes as
   `trackPresentation: PREVIEW`, 29 seconds long, with `previewReason: FULL_REQUIRES_HIGHER_ACCESS_TIER`.
   Developer accounts are on the `THIRD_PARTY` tier.
3. **v1 refuses a developer app's login** ("Client does not have required access tier").

So full-length playback needs a second login with a client ID from TIDAL's own apps (as Strawberry
does), and that login is used only for v1's playback info. Everything else stays on the official API.
v1 and v2 use the same track IDs (confirmed with the playback login). Search is
`GET /searchResults?filter[query]=…`; the older `/searchResults/{query}` form now returns 400.

### Running the probe

1. Sign in at <https://developer.tidal.com/dashboard> and create an app. Add the redirect URI
   `http://localhost:47130/callback`, and enable the scopes it offers for user data, collections,
   playlists, search and playback.
2. Then:

   ```sh
   tools/tidal-probe.py login --client-id <your app's Client ID>
   tools/tidal-probe.py search "an artist you like"     # pick a track ID
   tools/tidal-probe.py report <track ID>               # every check, as JSON
   tools/tidal-probe.py manifest <track ID> --play      # hear it, if there's no DRM (needs ffplay)
   ```

3. For playback, log in again with a client ID from one of TIDAL's own apps. The probe asks for the
   ID and secret without showing them:

   ```sh
   tools/tidal-probe.py login-v1 --pkce       # browser login; paste back the "Oops" page's URL
   tools/tidal-probe.py login-v1 --device     # or: approve a code at link.tidal.com
   tools/tidal-probe.py v1 <track ID> --play  # full-length? what quality? (needs ffplay)
   ```

   Using these clients isn't sanctioned by TIDAL, and TIDAL can revoke them.

The logins are kept in `~/.config/fooyin-tidal/probe.json`, readable only by you.

## Trying it

```sh
scripts/build.sh && scripts/install.sh
```

Restart fooyin and open **Settings > Integrations > TIDAL**, which has two logins:

- **TIDAL Account**, for the TIDAL panel's search, favorites and playlists: the client ID of your
  developer app (see "Running the probe" above for setting one up). **Log In** opens TIDAL's login
  page, which comes back to fooyin by itself.
- **Playback**: the client ID and secret of one of TIDAL's own apps. **Log In** opens TIDAL's login
  page, which ends on an "Oops" page; copy that page's whole address into the box under it and
  press **Finish Login**.

The logins are kept in `~/.var/app/org.fooyin.fooyin/config/fooyin/tidal/` (`account.json` and
`session.json`), readable only by you.

Then open **View > TIDAL**, or put the panel in your layout. To have it as a tab next to your
playlist: **Layout > Editing mode**, right-click the playlist, **Split > Tab Stack**, then
right-click the new tab bar, **Insert inside > Internet > TIDAL**, and turn editing mode off.
Pick favorites or playlists from the list at the top, or type a search and press Enter.
Double-click a track to play it and the rest of its list after it, in the order shown; double-click
an album, playlist or artist to open it, and **Back** to return. Click a column heading to sort
(**Added** sorts by when tracks went into the playlist or favorites); the sort is kept for every
list. Right-click a column heading to show or hide columns (Album Artist, Track No. and Release
Date are there too), turn auto-sizing off to set widths yourself, or reset them; drag headings to
reorder. Each TIDAL panel keeps its own columns, saved with fooyin's layout. Right-click tracks for fooyin's usual track menu, or an album or playlist to play it, add it to
the current playlist, or send it to a new one.

Lists the panel has fetched are kept in `~/.var/app/org.fooyin.fooyin/cache/fooyin/tidal/`, so they
open straight away next time; TIDAL is then asked (one quick request) whether the list changed, and
it's fetched again only if it did. The reload button fetches the list again regardless.

TIDAL tracks have their album covers wherever fooyin shows artwork (the cover widget, playlist
thumbnails, the lyrics background, …): fooyin asks the plugin as it would read a cover embedded in
a file. Covers are downloaded at 1280×1280 and kept in `cache/fooyin/tidal/covers/`, one per album;
covers not shown for 60 days are deleted.

**File > Add TIDAL track…** still adds a single track with a link such as
`https://tidal.com/browse/track/58990486` or just the number. The track goes to the current
playlist and plays like any other.

`scripts/catalog-check.sh search <words>` (or `favorite-tracks`, `playlists`, `album <id>`, …) runs
the panel's calls to the official API inside fooyin's sandbox and prints what the panel would list;
`--version` prints what the cache compares, `--cache <dir>` stores a list in a cache and reads it back.

`scripts/stream-check.sh <track ID> [start ms]` runs the plugin's streaming code inside fooyin's
sandbox without starting fooyin, and checks the result with ffprobe. With a start position it
streams from there, as a seek does.

`scripts/cover-check.sh <track ID>` reads a track's details and cover with the plugin's reader, into
`build/cover-check/`, twice: the second read should come from its cache.

## Building

fooyin is installed as a Flatpak, so the plugin is built inside the same SDK fooyin is built with
(`org.kde.Sdk`, installed for you on first use), against fooyin's own headers for the installed
version.

```sh
scripts/build.sh     # first run also builds fooyin's development files into .deps/ (a few minutes)
scripts/check.sh     # loads the plugin inside fooyin's sandbox, without starting fooyin
scripts/install.sh   # copies it to ~/.var/app/org.fooyin.fooyin/.local/lib/fooyin/plugins
```

fooyin's plugin interface isn't stable between versions, so after fooyin updates, run
`scripts/build.sh` and `scripts/install.sh` again: the build notices the new version and rebuilds
against it.

## License

GPL-3.0-or-later, like fooyin.
