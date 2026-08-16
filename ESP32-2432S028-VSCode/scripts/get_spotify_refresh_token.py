"""Get a Spotify PKCE refresh token for the ESP32 dashboard.

Before running, add http://127.0.0.1:8888/callback as a Redirect URI in the
Spotify Developer Dashboard. This script intentionally never asks for or uses
the app's Client Secret.
"""

from __future__ import annotations

import base64
import argparse
import hashlib
import json
import secrets
import threading
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPE = (
    "user-read-currently-playing "
    "user-read-playback-state "
    "user-modify-playback-state"
)


def read_env_value(path: Path, key: str) -> str:
    if not path.exists():
        return ""
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        candidate, value = line.split("=", 1)
        if candidate.strip() == key:
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            return value
    return ""


def update_env(path: Path, updates: dict[str, str]) -> None:
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    remaining = dict(updates)
    updated_lines: list[str] = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in remaining:
                updated_lines.append(f"{key}={remaining.pop(key)}")
                continue
        updated_lines.append(line)
    if updated_lines and remaining:
        updated_lines.append("")
    updated_lines.extend(f"{key}={value}" for key, value in remaining.items())
    path.write_text("\n".join(updated_lines) + "\n", encoding="utf-8")


def base64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--client-id", help="Spotify application Client ID")
    parser.add_argument(
        "--env-file",
        type=Path,
        default=Path(__file__).resolve().parents[1] / ".env",
        help="Environment file to read and update (default: project .env)",
    )
    arguments = parser.parse_args()

    saved_client_id = read_env_value(arguments.env_file, "SPOTIFY_CLIENT_ID")
    if saved_client_id.startswith("YOUR_"):
        saved_client_id = ""
    client_id = (arguments.client_id or saved_client_id or input("Spotify Client ID: ")).strip()
    if not client_id:
        raise SystemExit("A Client ID is required.")

    state = secrets.token_urlsafe(24)
    verifier = secrets.token_urlsafe(64)
    challenge = base64url(hashlib.sha256(verifier.encode("ascii")).digest())
    result: dict[str, str] = {}
    finished = threading.Event()

    class CallbackHandler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if query.get("state", [""])[0] != state:
                result["error"] = "OAuth state mismatch"
            elif "error" in query:
                result["error"] = query["error"][0]
            else:
                result["code"] = query.get("code", [""])[0]

            message = (
                "Spotify authorization received. You may close this tab."
                if "code" in result
                else "Spotify authorization failed. Return to the terminal."
            )
            body = message.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            finished.set()

        def log_message(self, _format: str, *_args: object) -> None:
            return

    server = HTTPServer(("127.0.0.1", 8888), CallbackHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    authorize_query = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPE,
            "state": state,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
        }
    )
    authorize_url = "https://accounts.spotify.com/authorize?" + authorize_query
    print("Opening Spotify authorization in your browser...")
    if not webbrowser.open(authorize_url):
        print("Open this URL manually:\n" + authorize_url)

    if not finished.wait(timeout=300):
        server.shutdown()
        raise SystemExit("Authorization timed out after five minutes.")
    server.shutdown()

    if "error" in result:
        raise SystemExit("Authorization failed: " + result["error"])

    token_body = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": REDIRECT_URI,
            "code_verifier": verifier,
        }
    ).encode("ascii")
    request = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=token_body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        token_response = json.load(response)

    refresh_token = token_response.get("refresh_token")
    if not refresh_token:
        raise SystemExit("Spotify did not return a refresh token.")

    update_env(
        arguments.env_file,
        {
            "SPOTIFY_CLIENT_ID": client_id,
            "SPOTIFY_REFRESH_TOKEN": refresh_token,
        },
    )
    print(f"Authorization succeeded. Updated {arguments.env_file}.")


if __name__ == "__main__":
    main()
