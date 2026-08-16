# ESP32-2432S028 touch menu

This is a PlatformIO project for the ESP32-2432S028 dual-USB board with a
240 x 320 ILI9341-compatible display.

## Connect and upload

1. Copy `.env.example` to `.env` and replace its placeholder values. PlatformIO
   generates an ignored C++ header from this file during each build.
2. Use a USB **data** cable in the board's small Micro-USB programming port.
   The USB-C connector on some revisions powers the board but does not expose
   the CH340 serial interface.
3. Open this folder in VS Code.
4. Wait for PlatformIO to finish installing its ESP32 toolchain and libraries.
5. Click the PlatformIO Upload arrow in the bottom status bar, or run:

   `pio run --target upload`

The screen shows a six-slot, touch-friendly mode menu. Spotify is currently the
only active mode; the other five cards are empty placeholders. Tap **Spotify**
to select it, then tap **Back** to return to the menu.

## First-time Wi-Fi setup

On first boot, the board tries its saved network and then creates a temporary
configuration network if needed:

1. Connect a phone to the setup network using the `WIFI_SETUP_NAME` and
   `WIFI_SETUP_PASSWORD` values from `.env`.
2. If the setup page does not open automatically, visit `192.168.4.1`.
3. Select the home 2.4 GHz Wi-Fi network, enter its password, and save.
4. The board stores the credentials and reconnects automatically on later boots.

Tap the **Wi-Fi** status button in the top-right corner of the menu to inspect
the connection or start the setup portal again. The menu continues in offline
mode if the five-minute setup portal times out.

## Spotify dashboard setup

The Spotify mode shows the current song, artist, album art, play/pause state,
elapsed time, total duration, and a live progress bar. The device refreshes the
Spotify API every five seconds and advances the timer locally between requests.

Spotify development-mode apps currently require the app owner to have Spotify
Premium. To authorize this personal dashboard:

1. Create an app at <https://developer.spotify.com/dashboard>.
2. In the app settings, add this exact Redirect URI:
   `http://127.0.0.1:8888/callback`
3. Run `python scripts/get_spotify_refresh_token.py` from this project folder.
   The helper reads the client ID from `.env` (or prompts if it is missing) and
   saves the new refresh token back to `.env` without printing it.
4. Build and upload the firmware again.

The helper uses Spotify's Authorization Code with PKCE flow and requests
`user-read-currently-playing`, `user-read-playback-state`, and
`user-modify-playback-state`. The final scope powers the previous, play/pause,
and next buttons. A Client Secret is not needed or stored on the ESP32. The real
`.env` file and generated configuration header are excluded by `.gitignore`;
`.env.example` is the safe template to commit.

If upload waits at `Connecting...`, hold the `BOOT` button, tap `RST`, and
release `BOOT` when writing begins.
