# Design notes

How the plugin works, why it's built this way, what was learned about fooyin and TIDAL along the
way, and what comes next. The [README](../README.md) covers building and trying it.

## Decisions

- **A plugin for fooyin, not a player of our own.** fooyin (a Qt 6 player modeled on Foobar2000)
  already does what was wanted from a desktop player; TIDAL was the missing part.
- **A third-party plugin, not a fork, and never sent upstream.** fooyin's `CONTRIBUTING.md` refuses
  contributions made with generative AI at any stage, but exempts independently developed
  third-party plugins. This code is AI-assisted, so it stays a separate plugin. Don't propose
  contributing it, or parts of it, to fooyin.
- **Two TIDAL logins.** The official v2 API (with a developer app) gives only 29-second previews,
  so full playback uses TIDAL's private v1 API with a client from one of TIDAL's own apps, as
  Strawberry and python-tidal do. v1 is used for playback only; everything else is meant to use v2
  (from phase 1). Accepted risk: TIDAL can change v1 or revoke the client, which would break
  playback but not search, favorites or playlists.
- **Built against the Flatpak.** fooyin is installed as the Flatpak `org.fooyin.fooyin`, so the
  plugin is built in the same SDK (`org.kde.Sdk`, currently 6.11) against fooyin's own headers for
  the installed version, and installed into the Flatpak's per-user plugin folder.
- **The Android player is separate and later:** a Kotlin/Media3 app using TIDAL's Android Player SDK.

## How playback works

```
playlist entry tidal://track/<id>
  └─ TidalDecoder (registered for the "tidal" scheme)
       ├─ TidalApi::stream()        v1 playbackinfo → DASH manifest → init + media segment URLs
       ├─ SegmentStreamDevice       downloads the segments in order on its own thread;
       │                            blocking reads, as one fragmented MP4
       └─ fooyin's FFmpeg decoder   decodes that MP4 (looked up by name in AudioLoader)
```

| File | What it does |
|---|---|
| `src/tidalplugin.*` | The plugin: core (logins), input (decoder registration), GUI (settings page, File > Add TIDAL track…). |
| `src/tidalsettingspage.*` | Settings > Integrations > TIDAL: the account and playback logins. |
| `src/callbackserver.*` | Waits on `localhost:47130` for the browser to come back from the account login. |
| `src/tidallogin.*` | Both logins' PKCE requests, code exchange, and the country lookup; blocking. |
| `src/tidaldecoder.*` | Wraps fooyin's FFmpeg decoder; fetches the stream, seeks, trims after a seek. |
| `src/segmentstreamdevice.*` | Sequential `QIODevice` over URLs fetched one after another. |
| `src/tidalapi.*` | v1 calls (track details, playbackinfo), DASH/BTS manifest parsing, `tidal://` paths. |
| `src/tidalsession.*` | One login in a JSON file (`account.json`, `session.json`); refreshes its token, thread-safe. |
| `src/blockinghttp.*` | Synchronous HTTP for threads without an event loop (fooyin's decoder threads). |
| `tools/tidal-probe.py` | Python probe of both APIs, with its own copy of the logins (`~/.config/fooyin-tidal`). |
| `tools/plugin-check.cpp` | Loads the plugin in fooyin's sandbox against the Flatpak's libraries. |
| `tools/stream-check.cpp` | Runs the streaming code for one track in fooyin's sandbox, optionally from a position. |

**Seeking:** each DASH media segment (about 4 s) starts at a known time from the manifest's
`SegmentTimeline`. A seek closes the current download and decoder, starts a new download at the
initialization segment plus the segment holding the target, and opens a fresh FFmpeg decoder.
Fragments carry their own `tfdt` timestamps, so FFmpeg reports real positions (checked: a stream
started at segment 26 of "Creep" begins at 99.845805 s). Decoded audio before the exact target is
dropped in `trimToSeekTarget()`. Segment URLs are signed, so the stream info is fetched again if
it's over five minutes old.

**Aborting:** fooyin's `AudioDecoder::requestAbort()` is one-shot (a `std::stop_token`), so the
decoder is finished once aborted. `interruptRead()` can run on another thread while a stream is
being opened, so the device to abort is held in an `std::atomic<std::shared_ptr>`.

## What was learned about fooyin (0.14.1)

- Plugins are Qt plugins with IID `org.fooyin.fooyin.plugin/2`, implementing any of `Plugin`,
  `CorePlugin`, `GuiPlugin`, `InputPlugin`. The C++ interface isn't stable: rebuild for every fooyin
  version (`scripts/build.sh` notices a new version and rebuilds against it).
- At startup, `CorePlugin::initialise` runs before input plugins' decoders are registered, so the
  decoder factory can use what the core initialisation set up.
- A decoder or reader declares URL schemes with `supportedSchemes()`; fooyin's `AudioLoader` then
  hands it `tidal://…` paths without opening them (`prepareSchemeSource`). The CD audio plugin
  (`src/plugins/cdda`) is the closest example of a scheme plugin; Radio Browser
  (`src/plugins/radiobrowser`) is the closest example of an online service with browser widgets.
- fooyin's own FFmpeg decoder is registered as `"FFmpeg"` (priority 1000) and reads from a
  `QIODevice` through custom AVIO callbacks; a sequential device disables seeking in FFmpeg.
- Decoded `AudioBuffer`s are stamped with FFmpeg's frame timestamps (ms), which is why restarting at
  a segment keeps positions right.
- Tracks the plugin builds must call `setMetadataWasRead(true)`, or fooyin tries to read their tags.
- fooyin's **File > Add stream URL** accepts only `http(s)`, hence the plugin's own menu action.
- In the Flatpak, fooyin's config is `~/.var/app/org.fooyin.fooyin/config/fooyin/` (the plugin
  keeps `tidal/session.json` there) and user plugins load from
  `~/.var/app/org.fooyin.fooyin/.local/lib/fooyin/plugins/`. Its sandbox can't read
  `~/.config/fooyin-tidal`, which is why the probe exports the login into fooyin's folder.

## What was learned about TIDAL (October 2026)

**Official API** (`https://openapi.tidal.com/v2`, JSON:API; spec at
`https://tidal-music.github.io/tidal-api-reference/tidal-api-oas.json`):

- Login: PKCE at `https://login.tidal.com/authorize`, tokens from
  `https://auth.tidal.com/v1/oauth2/token`. A `http://localhost:<port>/callback` redirect works.
  Ask only for scopes the developer dashboard offers (`user.read collection.read collection.write
  playlists.read playlists.write search.read playback` work); asking for the legacy `r_usr` got
  login error 1002. The login page is behind bot protection, so it can't be tested with curl.
- Search is `GET /searchResults?filter[query]=…&include=tracks` (a collection with one result whose
  relationships list the hits). The older `/searchResults/{query}` returns 400 `INVALID_RESOURCE_ID`.
- `GET /users/me` gives the country (and the account's name and email: don't log it).
- Playlists, favorites ("userCollection…"), tracks, albums and artists all have v2 endpoints,
  including playlist create/edit/delete and item add/remove/reorder.
- `GET /trackManifests/{id}` returns DRM-free FLAC or AAC (DASH or HLS) with ReplayGain values,
  but for a developer app (tier `THIRD_PARTY`) every track is a 29 s `PREVIEW`, with
  `previewReason: FULL_REQUIRES_HIGHER_ACCESS_TIER`.

**Private API** (`https://api.tidal.com/v1`), for playback:

- Rejects a developer app's token ("Client does not have required access tier").
- Works with python-tidal's PKCE client (TIDAL's Android app): login at `login.tidal.com/authorize`
  with `redirect_uri=https://tidal.com/android/login/auth`, `appMode=android`, a
  `client_unique_key`, scopes `r_usr w_usr w_sub`. The browser ends on an "Oops" page whose URL
  holds the code to exchange. python-tidal's other ("device") client uses the TV-style
  `device_authorization` flow instead and is said to be limited to CD quality.
- `GET /tracks/{id}/playbackinfo?audioquality=HI_RES_LOSSLESS&playbackmode=STREAM&assetpresentation=FULL&countryCode=…`
  returns the best quality available up to that: 24-bit FLAC where TIDAL has hi-res, else 16-bit.
  Both came as a DASH manifest (`application/dash+xml`, base64): one representation, a
  `SegmentTemplate` with an initialization URL and `$Number$` media URLs, segments of 176128 samples
  (about 4 s at 44.1 kHz). The other manifest type, `application/vnd.tidal.bts`, is JSON with
  direct URLs (`encryptionType` must be `NONE`); the plugin handles it but hasn't seen one.
- v1 and v2 use the same track IDs (checked: `58990486` is "Creep" in both, same ISRC).
- `GET /tracks/{id}` gives title, version, artists, album, track and volume number, duration (s).

Track IDs to test with: `58990486` (Radiohead, "Creep", CD quality) and `290136822` (LANNÉ,
"Dark Side Of The Moon", 24-bit).

## Build and environment pitfalls

- `flatpak run` of a runtime on its own (the SDK) has **no network**; `build-fooyin.sh` therefore
  fetches KDSingleApplication on the host and passes it to CMake.
- A build-tree RPATH to `.deps/` would make the load check resolve fooyin's libraries from `.deps`
  instead of the Flatpak's; `build.sh` passes `-DCMAKE_SKIP_BUILD_RPATH=ON`.
- fooyin builds as C++23; the plugin matches it.
- fooyin is single-instance: launching it again hands off to the running copy. Quit it fully before
  starting it from a terminal to see `fooyin.tidal` log lines.

## Next: phase 1

Roughly in this order:

1. **Logins in fooyin's settings.** Done (2026-10-28), waiting to be tried with real logins: a
   settings page (a `SettingsPage` like the scrobbler's, under Integrations) with the account login
   (PKCE, the browser returning to `CallbackServer` on `localhost:47130`) and the playback login
   (browser login, then the "Oops" address pasted into a box). Tokens are in `account.json` and
   `session.json` (mode 0600) in the plugin's config folder; the probe's `export-session` is gone.
   The account's country comes from v2 `/users/me`, the playback login's from v1 `/sessions`.
   Open question: whether to keep secrets in KWallet/Secret Service via a portal instead of a file.
2. **A TIDAL panel** (a widget registered with fooyin's `WidgetProvider`, as Radio Browser does):
   search (tracks, albums, artists, playlists), your favorites, your playlists; double-click plays
   in context, the rest of the album or list following.
3. **Track details from v2** instead of v1 (artists, album artists, release date, ISRC, ReplayGain
   from `trackManifests`' normalization data if usable), and **cover art** (v2 album `coverArt`).
4. **Playlists:** open a TIDAL playlist as a fooyin playlist; create, rename, delete; add, remove and
   reorder tracks; "Add to TIDAL playlist…" and favorite/unfavorite in the context menu.
5. **Polish:** a quality setting, errors shown in fooyin rather than only the log, a clear message
   when v1 playback stops working.

Ideas for later: matching TIDAL tracks to local files by ISRC; caching streams for gapless
transitions; limiting how far ahead `SegmentStreamDevice` downloads.
