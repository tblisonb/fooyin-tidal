# fooyin-tidal

A [TIDAL](https://tidal.com) plugin for the [fooyin](https://github.com/fooyin/fooyin) music player:
search TIDAL, play it alongside your own library, and manage your TIDAL playlists and favorites from
fooyin.

This is an independent third-party plugin. It isn't part of fooyin or affiliated with TIDAL, so please
don't take problems with it to either project.

**Status:** phase 0, a feasibility spike. The plugin builds and loads but doesn't do anything yet;
`tools/tidal-probe.py` tests what TIDAL's APIs allow before the plugin depends on it.

## How it fits together

- **Search, browsing, favorites and playlists** use TIDAL's official
  [developer API](https://developer.tidal.com) (`openapi.tidal.com/v2`), signed in with your own
  developer app.
- **Playback** registers a `tidal:` URL scheme with fooyin. When a TIDAL track starts, the plugin asks
  TIDAL for that track's stream and hands it to fooyin's own decoders. Which API gives a stream fooyin
  can play is what phase 0 finds out (see below).

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
v2's track IDs look like the numbers v1 and TIDAL's links use; checking that needs the v1 login. Search is
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

The login is kept in `~/.config/fooyin-tidal/probe.json`, readable only by you.

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
