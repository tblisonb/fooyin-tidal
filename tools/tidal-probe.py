#!/usr/bin/env python3
"""Phase 0 probe: answers the questions the plugin's design depends on, before any C++ does.

  1. Can a TIDAL developer app log in from a desktop (PKCE with a localhost redirect)?
  2. Does the official v2 API hand that app track manifests without DRM, which fooyin could play?
  3. If not, does the private v1 API accept the same login, and do v1 and v2 share track IDs?

Standard library only. Logins are kept in ~/.config/fooyin-tidal/probe.json (readable only by you).

  tools/tidal-probe.py login --client-id <id>     # opens the browser; run once
  tools/tidal-probe.py search "artist or title"   # find a track ID
  tools/tidal-probe.py track <id>                 # its v2 metadata
  tools/tidal-probe.py manifest <id> [--play]     # question 2
  tools/tidal-probe.py v1 <id>                    # question 3
  tools/tidal-probe.py report <id>                # all of the above, for pasting back
"""

import argparse
import base64
import hashlib
import http.server
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

AUTHORIZE_URL = "https://login.tidal.com/authorize"
TOKEN_URL = "https://auth.tidal.com/v1/oauth2/token"
API_V2 = "https://openapi.tidal.com/v2"
API_V1 = "https://api.tidal.com/v1"
DEFAULT_PORT = 47130
# The scopes a developer app can enable and the plugin will use. The API reference also lists the
# legacy r_usr for trackManifests, but the developer dashboard doesn't offer it, and asking for a
# scope the app lacks can make TIDAL's login page refuse the request.
DEFAULT_SCOPES = ("user.read collection.read collection.write playlists.read playlists.write "
                  "search.read playback")

STATE_DIR = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "fooyin-tidal")
STATE_FILE = os.path.join(STATE_DIR, "probe.json")
OUT_DIR = os.path.join(STATE_DIR, "probe-out")


def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def save_state(state):
    os.makedirs(STATE_DIR, mode=0o700, exist_ok=True)
    fd = os.open(STATE_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(state, f, indent=2)


class HttpError(Exception):
    def __init__(self, status, body):
        super().__init__(f"HTTP {status}: {body[:500]}")
        self.status = status
        self.body = body


def request(method, url, *, params=None, headers=None, form=None):
    if params:
        url += "?" + urllib.parse.urlencode(params, doseq=True)
    data = urllib.parse.urlencode(form).encode() if form is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = resp.read().decode("utf-8", "replace")
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        raise HttpError(e.code, e.read().decode("utf-8", "replace")) from None


# ---- Login ----

def cmd_login(args):
    state = load_state()
    client_id = args.client_id or state.get("client_id")
    if not client_id:
        sys.exit("Pass --client-id: the Client ID of your app at https://developer.tidal.com/dashboard")
    redirect_uri = f"http://localhost:{args.port}/callback"

    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    expected_state = secrets.token_urlsafe(16)
    url = AUTHORIZE_URL + "?" + urllib.parse.urlencode({
        "response_type": "code",
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "scope": args.scopes,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        "state": expected_state,
    })

    result = {}

    class Callback(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            result.update({k: v[0] for k, v in query.items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(b"Done. You can close this tab and go back to the terminal.")

        def log_message(self, *_):
            pass

    server = http.server.HTTPServer(("127.0.0.1", args.port), Callback)
    print(f"Opening TIDAL's login page. If no browser opens, visit:\n\n  {url}\n")
    print(f"Your app's redirect URI must be exactly {redirect_uri}")
    print(f"and it must have every one of these scopes enabled: {args.scopes}\n")
    webbrowser.open(url)
    while not result:
        server.handle_request()
    server.server_close()

    if "error" in result:
        sys.exit(f"Login failed: {result.get('error')}: {result.get('error_description', '')}")
    if result.get("state") != expected_state:
        sys.exit("Login failed: the reply's state doesn't match this login")

    token = request("POST", TOKEN_URL, form={
        "grant_type": "authorization_code",
        "client_id": client_id,
        "code": result["code"],
        "redirect_uri": redirect_uri,
        "code_verifier": verifier,
    })
    state.update(client_id=client_id, scopes=args.scopes)
    store_token(state, token)
    save_state(state)
    print(f"Logged in. Granted scopes: {token.get('scope', '(not stated)')}")


def store_token(state, token):
    state["access_token"] = token["access_token"]
    if token.get("refresh_token"):
        state["refresh_token"] = token["refresh_token"]
    state["expires_at"] = time.time() + int(token.get("expires_in", 3600)) - 60
    if token.get("user_id"):
        state["user_id"] = str(token["user_id"])


def access_token(state):
    if "access_token" not in state:
        sys.exit("Not logged in. Run: tools/tidal-probe.py login --client-id <id>")
    if time.time() >= state.get("expires_at", 0):
        token = request("POST", TOKEN_URL, form={
            "grant_type": "refresh_token",
            "client_id": state["client_id"],
            "refresh_token": state["refresh_token"],
        })
        store_token(state, token)
        save_state(state)
    return state["access_token"]


# ---- v2 ----

def v2(state, path, params=None):
    headers = {"Authorization": f"Bearer {access_token(state)}", "Accept": "application/vnd.api+json"}
    return request("GET", API_V2 + path, params=params, headers=headers)


def country(state):
    if "country" not in state:
        me = v2(state, "/users/me")
        attrs = me.get("data", {}).get("attributes", {})
        state["country"] = attrs.get("country", "US")
        state["user_id"] = me.get("data", {}).get("id", state.get("user_id"))
        save_state(state)
    return state["country"]


def included(doc, kind):
    return {item["id"]: item for item in doc.get("included", []) if item.get("type") == kind}


def cmd_search(args):
    state = load_state()
    doc = v2(state, f"/searchResults/{urllib.parse.quote(args.query, safe='')}/relationships/tracks",
             {"countryCode": country(state), "include": "tracks"})
    tracks = included(doc, "tracks")
    for ref in doc.get("data", [])[:args.limit]:
        attrs = tracks.get(ref["id"], {}).get("attributes", {})
        print(f"{ref['id']:>12}  {attrs.get('title', '?')}  ({attrs.get('duration', '')})")


def track_summary(state, track_id):
    doc = v2(state, f"/tracks/{track_id}", {"countryCode": country(state), "include": "artists,albums"})
    attrs = doc["data"]["attributes"]
    artists = [a["attributes"].get("name") for a in included(doc, "artists").values()]
    albums = [a["attributes"].get("title") for a in included(doc, "albums").values()]
    return {
        "id": doc["data"]["id"],
        "title": attrs.get("title"),
        "artists": artists,
        "albums": albums,
        "duration": attrs.get("duration"),
        "isrc": attrs.get("isrc"),
        "mediaTags": attrs.get("mediaTags"),
    }


def cmd_track(args):
    print(json.dumps(track_summary(load_state(), args.id), indent=2, ensure_ascii=False))


def fetch_manifest(state, track_id, manifest_type, uri_scheme, formats):
    params = {
        "manifestType": manifest_type,
        "formats": formats,
        "uriScheme": uri_scheme,
        "usage": "PLAYBACK",
        "adaptive": "false",
    }
    doc = v2(state, f"/trackManifests/{track_id}", params)
    attrs = doc.get("data", {}).get("attributes", {})
    uri = attrs.get("uri", "")
    summary = {
        "request": f"{manifest_type}, {uri_scheme}, {','.join(formats)}",
        "formats": attrs.get("formats"),
        "trackPresentation": attrs.get("trackPresentation"),
        "previewReason": attrs.get("previewReason"),
        "drm": attrs["drmData"].get("drmSystem") if attrs.get("drmData") else None,
        "trackReplayGain": attrs.get("trackAudioNormalizationData"),
        "albumReplayGain": attrs.get("albumAudioNormalizationData"),
        "uri": (uri[:80] + "...") if len(uri) > 80 else uri,
    }
    return summary, uri


def save_manifest(track_id, manifest_type, uri):
    """Writes the manifest to a file ffprobe/ffplay can open, and returns its path (or the URL)."""
    if uri.startswith("data:"):
        header, _, payload = uri.partition(",")
        body = base64.b64decode(payload) if ";base64" in header else urllib.parse.unquote(payload).encode()
        os.makedirs(OUT_DIR, exist_ok=True)
        path = os.path.join(OUT_DIR, f"{track_id}.{'m3u8' if manifest_type == 'HLS' else 'mpd'}")
        with open(path, "wb") as f:
            f.write(body)
        return path
    return uri


def ffprobe(source):
    if not shutil.which("ffprobe"):
        return "ffprobe not installed"
    proc = subprocess.run(
        ["ffprobe", "-v", "error", "-protocol_whitelist", "file,http,https,tcp,tls,crypto,data",
         "-show_entries", "stream=codec_name,sample_rate,bits_per_raw_sample,channels:format=duration",
         "-of", "json", source],
        capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        return "ffprobe failed: " + proc.stderr.strip()[-300:]
    return json.loads(proc.stdout)


def cmd_manifest(args):
    state = load_state()
    summary, uri = fetch_manifest(state, args.id, args.type, args.uri, args.formats.split(","))
    print(json.dumps(summary, indent=2))
    if summary["drm"] or not uri:
        return
    source = save_manifest(args.id, args.type, uri)
    print("ffprobe:", json.dumps(ffprobe(source), indent=2))
    if args.play:
        subprocess.run(["ffplay", "-nodisp", "-autoexit", "-loglevel", "warning",
                        "-protocol_whitelist", "file,http,https,tcp,tls,crypto,data", source])


# ---- v1 ----

def cmd_v1(args):
    print(json.dumps(v1_summary(load_state(), args.id), indent=2))


def v1_summary(state, track_id):
    headers = {"Authorization": f"Bearer {access_token(state)}"}
    out = {}
    try:
        track = request("GET", f"{API_V1}/tracks/{track_id}", params={"countryCode": country(state)},
                        headers=headers)
        out["track"] = {"title": track.get("title"), "artist": (track.get("artist") or {}).get("name"),
                        "isrc": track.get("isrc")}
    except HttpError as e:
        out["track"] = str(e)
    try:
        info = request("GET", f"{API_V1}/tracks/{track_id}/playbackinfo", headers=headers, params={
            "audioquality": "LOSSLESS", "playbackmode": "STREAM", "assetpresentation": "FULL"})
        out["playbackinfo"] = {k: info.get(k) for k in
                               ("audioQuality", "assetPresentation", "manifestMimeType", "bitDepth", "sampleRate")}
    except HttpError as e:
        out["playbackinfo"] = str(e)
    return out


# ---- Report ----

def cmd_report(args):
    state = load_state()
    report = {"granted_scopes": state.get("scopes"), "country": None, "track": None, "manifests": [], "v1": None}

    def attempt(fn):
        try:
            return fn()
        except HttpError as e:
            return str(e)

    report["country"] = attempt(lambda: country(state))
    report["track"] = attempt(lambda: track_summary(state, args.id))
    for manifest_type in ("HLS", "MPEG_DASH"):
        for formats in (["FLAC_HIRES", "FLAC"], ["FLAC"], ["AACLC"]):
            def one(manifest_type=manifest_type, formats=formats):
                summary, uri = fetch_manifest(state, args.id, manifest_type, "DATA", formats)
                if not summary["drm"] and uri:
                    summary["ffprobe"] = ffprobe(save_manifest(args.id, manifest_type, uri))
                return summary
            report["manifests"].append(attempt(one))
    report["v1"] = attempt(lambda: v1_summary(state, args.id))
    print(json.dumps(report, indent=2, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("login", help="log in with your TIDAL developer app")
    p.add_argument("--client-id")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--scopes", default=DEFAULT_SCOPES)
    p.set_defaults(func=cmd_login)

    p = sub.add_parser("search", help="search tracks, to find an ID")
    p.add_argument("query")
    p.add_argument("--limit", type=int, default=10)
    p.set_defaults(func=cmd_search)

    p = sub.add_parser("track", help="a track's v2 metadata")
    p.add_argument("id")
    p.set_defaults(func=cmd_track)

    p = sub.add_parser("manifest", help="a track's v2 manifest, and whether it has DRM")
    p.add_argument("id")
    p.add_argument("--type", choices=["HLS", "MPEG_DASH"], default="HLS")
    p.add_argument("--uri", choices=["DATA", "HTTPS"], default="DATA")
    p.add_argument("--formats", default="FLAC_HIRES,FLAC")
    p.add_argument("--play", action="store_true", help="play it with ffplay if it has no DRM")
    p.set_defaults(func=cmd_manifest)

    p = sub.add_parser("v1", help="the same track through the private v1 API, with the same login")
    p.add_argument("id")
    p.set_defaults(func=cmd_v1)

    p = sub.add_parser("report", help="every check for one track, as JSON to paste back")
    p.add_argument("id")
    p.set_defaults(func=cmd_report)

    args = parser.parse_args()
    try:
        args.func(args)
    except HttpError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
