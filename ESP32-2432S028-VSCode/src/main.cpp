#include <Arduino.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <XPT2046_Touchscreen.h>

#include "generated_config.h"
#include "spotify_client.h"
#include "lyrics_client.h"
#include "glances_client.h"

namespace {

constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 240;

constexpr int TOUCH_IRQ = 36;
constexpr int TOUCH_MOSI = 32;
constexpr int TOUCH_MISO = 39;
constexpr int TOUCH_CLK = 25;
constexpr int TOUCH_CS = 33;

// Typical raw limits for the XPT2046 fitted to the ESP32-2432S028.
constexpr int TOUCH_X_MIN = 200;
constexpr int TOUCH_X_MAX = 3700;
constexpr int TOUCH_Y_MIN = 240;
constexpr int TOUCH_Y_MAX = 3800;

constexpr uint16_t COLOR_BACKGROUND = 0x1082;
constexpr uint16_t COLOR_PANEL = 0x2124;
constexpr uint16_t COLOR_PANEL_PRESSED = 0x31A6;
constexpr uint16_t COLOR_SPOTIFY = 0x1DC5;
constexpr uint16_t COLOR_MUTED = 0x9CF3;
constexpr uint16_t COLOR_OFFLINE = 0xFBA0;

constexpr unsigned long WIFI_PORTAL_TIMEOUT_SECONDS = 300;

constexpr int ALBUM_ART_X = 10;
constexpr int ALBUM_ART_Y = 46;
constexpr int ALBUM_ART_SIZE = 68;

constexpr uint16_t COLOR_SPOTIFY_SCREEN = 0x10E3;
constexpr uint16_t COLOR_SPOTIFY_HEADER = 0x18E4;
constexpr uint16_t COLOR_CORAL = 0xE3AA;
constexpr uint16_t COLOR_CONTROL = 0x2946;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  bool contains(int px, int py) const {
    return px >= x && px < x + width && py >= y && py < y + height;
  }
};

constexpr Rect MODE_BUTTONS[] = {
    {12, 58, 144, 52},
    {164, 58, 144, 52},
    {12, 117, 144, 52},
    {164, 117, 144, 52},
    {12, 176, 144, 52},
    {164, 176, 144, 52},
};
constexpr int MODE_COUNT = sizeof(MODE_BUTTONS) / sizeof(MODE_BUTTONS[0]);
constexpr Rect BACK_BUTTON = {12, 12, 72, 38};
constexpr Rect SPOTIFY_BACK_BUTTON = {8, 8, 38, 28};
constexpr Rect SPOTIFY_PREVIOUS_BUTTON = {90, 192, 38, 38};
constexpr Rect SPOTIFY_PLAY_BUTTON = {137, 188, 46, 46};
constexpr Rect SPOTIFY_NEXT_BUTTON = {192, 192, 38, 38};
constexpr Rect WIFI_STATUS_BUTTON = {238, 10, 70, 36};
constexpr Rect WIFI_SETUP_BUTTON = {40, 174, 240, 54};

enum class Screen {
  Menu,
  Spotify,
  Wifi,
  ServerStats,
};

TFT_eSPI display;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
WiFiManager wifiManager;
SpotifyClient spotifyClient;
LyricsClient lyricsClient;
GlancesClient glancesClient(GLANCES_HOST, GLANCES_PORT);

Screen currentScreen = Screen::Menu;
bool touchWasDown = false;
int pressedModeIndex = -1;
bool backButtonPressed = false;
bool wifiStatusButtonPressed = false;
bool wifiSetupButtonPressed = false;
int pressedSpotifyControl = -1;
int displayedLyricIndex = -2;
uint32_t currentTrackPlayCount = 0;
uint32_t currentArtistPlayCount = 0;
uint32_t lastGlancesUpdate = 0;
SpotifyStatus displayedSpotifyStatus = SpotifyStatus::Idle;
String displayedTrackId;
String displayedImageUrl;
String lastCountedTrackId;
int32_t displayedProgressSecond = -1;

void drawSpotifyLogo(int centerX, int centerY, int radius = 28) {
  display.fillCircle(centerX, centerY, radius, COLOR_SPOTIFY);

  const int left = centerX - radius * 55 / 100;
  const int right = centerX + radius * 55 / 100;
  const int top = centerY - radius / 3;
  const int bottom = centerY + radius / 3;

  // Three simple bands suggest the Spotify mark without needing image assets.
  display.drawLine(left, top, right, top + radius / 6, TFT_BLACK);
  display.drawLine(left, top + 1, right, top + radius / 6 + 1, TFT_BLACK);
  display.drawLine(left + 2, centerY, right - 2, centerY + radius / 8, TFT_BLACK);
  display.drawLine(left + 2, centerY + 1, right - 2, centerY + radius / 8 + 1, TFT_BLACK);
  display.drawLine(left + 4, bottom, right - 4, bottom + radius / 10, TFT_BLACK);
  display.drawLine(left + 4, bottom + 1, right - 4, bottom + radius / 10 + 1, TFT_BLACK);
}

void drawHeader(const char* title, const char* subtitle) {
  display.fillScreen(COLOR_BACKGROUND);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString(title, 12, 8, 4);

  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString(subtitle, 12, 37, 2);
}

void drawModeButton(int index, bool pressed) {
  const Rect& button = MODE_BUTTONS[index];
  const uint16_t fill = pressed ? COLOR_PANEL_PRESSED : COLOR_PANEL;
  const uint16_t border = index == 0 ? COLOR_SPOTIFY : 0x4228;

  display.fillRoundRect(button.x, button.y, button.width, button.height, 9, fill);
  display.drawRoundRect(button.x, button.y, button.width, button.height, 9,
                        pressed ? TFT_WHITE : border);

  display.setTextDatum(MC_DATUM);
  if (index == 0) {
    drawSpotifyLogo(button.x + 27, button.y + button.height / 2, 16);
    display.setTextDatum(ML_DATUM);
    display.setTextColor(TFT_WHITE, fill);
    display.drawString("Spotify", button.x + 51, button.y + button.height / 2, 2);
  } else if (index == 1) {
    display.fillRoundRect(button.x + 19, button.y + button.height / 2 - 10, 16, 20, 2, 0x4A69);
    display.fillCircle(button.x + 27, button.y + button.height / 2 - 4, 3, TFT_WHITE);
    display.setTextDatum(ML_DATUM);
    display.setTextColor(TFT_WHITE, fill);
    display.drawString("Server", button.x + 51, button.y + button.height / 2, 2);
  } else {
    display.setTextColor(COLOR_MUTED, fill);
    display.drawString("+", button.x + button.width / 2,
                       button.y + button.height / 2, 4);
  }
}

void drawWifiStatusButton(bool pressed) {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const uint16_t fill = pressed ? COLOR_PANEL_PRESSED : COLOR_PANEL;
  const uint16_t statusColor = connected ? COLOR_SPOTIFY : COLOR_OFFLINE;

  display.fillRoundRect(WIFI_STATUS_BUTTON.x, WIFI_STATUS_BUTTON.y,
                        WIFI_STATUS_BUTTON.width, WIFI_STATUS_BUTTON.height, 8, fill);
  display.drawRoundRect(WIFI_STATUS_BUTTON.x, WIFI_STATUS_BUTTON.y,
                        WIFI_STATUS_BUTTON.width, WIFI_STATUS_BUTTON.height, 8,
                        pressed ? TFT_WHITE : statusColor);
  display.fillCircle(WIFI_STATUS_BUTTON.x + 14,
                     WIFI_STATUS_BUTTON.y + WIFI_STATUS_BUTTON.height / 2, 5,
                     statusColor);
  display.setTextDatum(ML_DATUM);
  display.setTextColor(TFT_WHITE, fill);
  display.drawString("Wi-Fi", WIFI_STATUS_BUTTON.x + 25,
                     WIFI_STATUS_BUTTON.y + WIFI_STATUS_BUTTON.height / 2, 2);
}

void drawMenu() {
  drawHeader("Choose a mode", "Six slots available");
  drawWifiStatusButton(false);
  for (int index = 0; index < MODE_COUNT; ++index) {
    drawModeButton(index, false);
  }
}

void drawBackButton(bool pressed) {
  const uint16_t fill = pressed ? COLOR_PANEL_PRESSED : COLOR_PANEL;
  display.fillRoundRect(BACK_BUTTON.x, BACK_BUTTON.y,
                        BACK_BUTTON.width, BACK_BUTTON.height, 8, fill);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, fill);
  display.drawString("< Back", BACK_BUTTON.x + BACK_BUTTON.width / 2,
                     BACK_BUTTON.y + BACK_BUTTON.height / 2, 2);
}

String ellipsizeText(const String& text, int maxWidth, uint8_t font) {
  if (display.textWidth(text, font) <= maxWidth) {
    return text;
  }

  String shortened = text;
  while (shortened.length() > 1 &&
         display.textWidth(shortened + "...", font) > maxWidth) {
    shortened.remove(shortened.length() - 1);
  }
  return shortened + "...";
}

void drawTwoLineText(const String& text, int x, int y, int maxWidth,
                     uint8_t font, uint16_t color, uint16_t background) {
  String firstLine = text;
  String secondLine;
  if (display.textWidth(firstLine, font) > maxWidth) {
    int splitAt = -1;
    for (int index = 0; index < static_cast<int>(text.length()); ++index) {
      if (text[index] == ' ' &&
          display.textWidth(text.substring(0, index), font) <= maxWidth) {
        splitAt = index;
      }
      if (display.textWidth(text.substring(0, index + 1), font) > maxWidth) {
        break;
      }
    }
    if (splitAt < 1) {
      firstLine = ellipsizeText(text, maxWidth, font);
    } else {
      firstLine = text.substring(0, splitAt);
      secondLine = ellipsizeText(text.substring(splitAt + 1), maxWidth, font);
    }
  }

  display.setTextDatum(TL_DATUM);
  display.setTextColor(color, background);
  display.drawString(firstLine, x, y, font);
  if (!secondLine.isEmpty()) {
    display.drawString(secondLine, x, y + display.fontHeight(font) + 2, font);
  }
}

void drawSpotifyBackButton(bool pressed) {
  const uint16_t fill = pressed ? TFT_WHITE : COLOR_CORAL;
  display.fillRoundRect(SPOTIFY_BACK_BUTTON.x, SPOTIFY_BACK_BUTTON.y,
                        SPOTIFY_BACK_BUTTON.width, SPOTIFY_BACK_BUTTON.height,
                        7, fill);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(pressed ? COLOR_CORAL : TFT_WHITE, fill);
  display.drawString("<", SPOTIFY_BACK_BUTTON.x + SPOTIFY_BACK_BUTTON.width / 2,
                     SPOTIFY_BACK_BUTTON.y + SPOTIFY_BACK_BUTTON.height / 2, 2);
}

void drawSpotifyShell() {
  display.fillScreen(COLOR_SPOTIFY_SCREEN);
  display.fillRect(0, 0, SCREEN_WIDTH, 42, COLOR_SPOTIFY_HEADER);
  drawSpotifyBackButton(false);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_SPOTIFY_HEADER);
  display.drawString("Spotify Player", SCREEN_WIDTH / 2, 22, 2);
  display.drawFastHLine(0, 41, SCREEN_WIDTH, 0x2946);
}

void drawSpotifyMessage(const String& title, const String& detail) {
  display.fillRect(0, 42, SCREEN_WIDTH, SCREEN_HEIGHT - 42, COLOR_SPOTIFY_SCREEN);
  drawSpotifyLogo(SCREEN_WIDTH / 2, 103, 28);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_SPOTIFY_SCREEN);
  display.drawString(title, SCREEN_WIDTH / 2, 146, 4);
  display.setTextColor(COLOR_MUTED, COLOR_SPOTIFY_SCREEN);
  display.drawString(ellipsizeText(detail, 294, 2), SCREEN_WIDTH / 2, 179, 2);
}

String formatPlaybackTime(uint32_t milliseconds) {
  const uint32_t totalSeconds = milliseconds / 1000;
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%lu:%02lu",
           static_cast<unsigned long>(totalSeconds / 60),
           static_cast<unsigned long>(totalSeconds % 60));
  return String(buffer);
}

bool jpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height,
                uint16_t* bitmap) {
  if (y >= SCREEN_HEIGHT) {
    return false;
  }
  display.pushImage(x, y, width, height, bitmap);
  return true;
}

bool downloadAndDrawAlbumArt(const SpotifyTrack& track) {
  display.fillRoundRect(ALBUM_ART_X, ALBUM_ART_Y, ALBUM_ART_SIZE,
                        ALBUM_ART_SIZE, 5, COLOR_PANEL);
  if (track.imageUrl.isEmpty() || WiFi.status() != WL_CONNECTED) {
    drawSpotifyLogo(ALBUM_ART_X + ALBUM_ART_SIZE / 2,
                    ALBUM_ART_Y + ALBUM_ART_SIZE / 2, 32);
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(2500);
  if (!http.begin(secureClient, track.imageUrl)) {
    drawSpotifyLogo(ALBUM_ART_X + ALBUM_ART_SIZE / 2,
                    ALBUM_ART_Y + ALBUM_ART_SIZE / 2, 32);
    return false;
  }

  const int responseCode = http.GET();
  const int imageLength = http.getSize();
  const size_t maxSafeAlloc = ESP.getMaxAllocHeap();
  if (responseCode != HTTP_CODE_OK || imageLength <= 0 || imageLength > 35000 ||
      imageLength > static_cast<int>(maxSafeAlloc > 12000 ? maxSafeAlloc - 12000 : 0)) {
    Serial.printf("Album art skipped or failed: HTTP %d, length %d, maxAlloc %u\n",
                  responseCode, imageLength, (unsigned int)maxSafeAlloc);
    http.end();
    drawSpotifyLogo(ALBUM_ART_X + ALBUM_ART_SIZE / 2,
                    ALBUM_ART_Y + ALBUM_ART_SIZE / 2, 32);
    return false;
  }

  uint8_t* image = static_cast<uint8_t*>(malloc(imageLength));
  if (image == nullptr) {
    Serial.println("Not enough memory for album art");
    http.end();
    drawSpotifyLogo(ALBUM_ART_X + ALBUM_ART_SIZE / 2,
                    ALBUM_ART_Y + ALBUM_ART_SIZE / 2, 32);
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  int bytesRead = 0;
  uint32_t lastDataAtMs = millis();
  while (http.connected() && bytesRead < imageLength) {
    const int available = stream->available();
    if (available > 0) {
      bytesRead += stream->readBytes(image + bytesRead,
                                     min(available, imageLength - bytesRead));
      lastDataAtMs = millis();
    } else if (millis() - lastDataAtMs > 2500) {
      Serial.println("Album art download timed out");
      break;
    } else {
      delay(1);
    }
  }
  http.end();

  bool drawn = false;
  if (bytesRead == imageLength) {
    uint8_t scale = 1;
    if (track.imageWidth >= 500) {
      scale = 8;
    } else if (track.imageWidth >= 200) {
      scale = 4;
    } else if (track.imageWidth >= 100) {
      scale = 2;
    }
    const int renderedSize = track.imageWidth > 0 ? track.imageWidth / scale
                                                   : ALBUM_ART_SIZE;
    const int drawX = ALBUM_ART_X + max(0, (ALBUM_ART_SIZE - renderedSize) / 2);
    const int drawY = ALBUM_ART_Y + max(0, (ALBUM_ART_SIZE - renderedSize) / 2);
    TJpgDec.setJpgScale(scale);
    drawn = TJpgDec.drawJpg(drawX, drawY, image, imageLength) == JDR_OK;
  }
  free(image);

  if (!drawn) {
    Serial.println("Album art JPEG decode failed");
    drawSpotifyLogo(ALBUM_ART_X + ALBUM_ART_SIZE / 2,
                    ALBUM_ART_Y + ALBUM_ART_SIZE / 2, 32);
  }
  return drawn;
}

void drawSpotifyProgress() {
  const SpotifyTrack& track = spotifyClient.track();
  if (!track.available || track.durationMs == 0) {
    return;
  }

  const uint32_t progressMs = spotifyClient.estimatedProgressMs();
  const int32_t progressSecond = progressMs / 1000;
  if (progressSecond == displayedProgressSecond) {
    return;
  }
  displayedProgressSecond = progressSecond;

  // Clear progress line and time text without clearing large rectangles
  display.fillRect(10, 157, 300, 11, COLOR_SPOTIFY_SCREEN);
  display.drawFastHLine(10, 162, 300, COLOR_PANEL_PRESSED);
  const int progressWidth = static_cast<uint64_t>(progressMs) * 300 /
                            track.durationMs;
  if (progressWidth > 0) {
    display.drawFastHLine(10, 162, progressWidth, COLOR_SPOTIFY);
  }
  display.fillCircle(10 + progressWidth, 162, 4, 0xBDF7);

  display.fillRect(10, 168, 65, 18, COLOR_SPOTIFY_SCREEN);
  display.fillRect(250, 168, 65, 18, COLOR_SPOTIFY_SCREEN);

  display.setTextDatum(ML_DATUM);
  display.setTextColor(COLOR_MUTED, COLOR_SPOTIFY_SCREEN);
  display.drawString(formatPlaybackTime(progressMs), 10, 175, 2);
  display.setTextDatum(MR_DATUM);
  display.drawString(formatPlaybackTime(track.durationMs), 310, 175, 2);
}


void drawConnectionBars() {
  for (int index = 0; index < 3; ++index) {
    const int height = 5 + index * 4;
    display.fillRoundRect(287 + index * 7, 69 - height, 4, height, 2,
                          COLOR_SPOTIFY);
  }
}

void drawPlaybackControls(bool isPlaying, int pressedControl = -1) {
  constexpr int buttonY = 210;
  display.fillRoundRect(90, buttonY - 18, 38, 38, 8,
                        pressedControl == 0 ? COLOR_PANEL_PRESSED : COLOR_CONTROL);
  display.drawRoundRect(90, buttonY - 18, 38, 38, 8, COLOR_MUTED);
  display.fillRect(104, buttonY - 7, 3, 14, TFT_WHITE);
  display.fillTriangle(116, buttonY - 8, 116, buttonY + 8,
                       106, buttonY, TFT_WHITE);

  display.fillRoundRect(137, buttonY - 22, 46, 46, 9,
                        pressedControl == 1 ? TFT_WHITE : 0xBDF7);
  if (isPlaying) {
    display.fillRect(151, buttonY - 9, 5, 18, COLOR_SPOTIFY_SCREEN);
    display.fillRect(164, buttonY - 9, 5, 18, COLOR_SPOTIFY_SCREEN);
  } else {
    display.fillTriangle(153, buttonY - 11, 153, buttonY + 11,
                         171, buttonY, COLOR_SPOTIFY_SCREEN);
  }

  display.fillRoundRect(192, buttonY - 18, 38, 38, 8,
                        pressedControl == 2 ? COLOR_PANEL_PRESSED : COLOR_CONTROL);
  display.drawRoundRect(192, buttonY - 18, 38, 38, 8, COLOR_MUTED);
  display.fillTriangle(204, buttonY - 8, 204, buttonY + 8,
                       214, buttonY, TFT_WHITE);
  display.fillRect(214, buttonY - 7, 3, 14, TFT_WHITE);
}

constexpr int LYRICS_CARD_X = 86;
constexpr int LYRICS_CARD_Y = 94;
constexpr int LYRICS_CARD_W = 224;
constexpr int LYRICS_CARD_H = 56;
constexpr uint16_t COLOR_LYRICS_BG = 0x1A05;
constexpr uint16_t COLOR_LYRICS_BORDER = 0x2A69;

void drawWrappedCardText(const String& text, int x, int y, int maxWidth, int maxHeight,
                         uint8_t font, uint16_t color, uint16_t background) {
  String firstLine = text;
  String secondLine = "";

  if (display.textWidth(firstLine, font) > maxWidth) {
    int splitAt = -1;
    for (int i = 0; i < static_cast<int>(text.length()); ++i) {
      if (text[i] == ' ' && display.textWidth(text.substring(0, i), font) <= maxWidth) {
        splitAt = i;
      }
      if (display.textWidth(text.substring(0, i + 1), font) > maxWidth) {
        break;
      }
    }
    if (splitAt < 1) {
      firstLine = ellipsizeText(text, maxWidth, font);
    } else {
      firstLine = text.substring(0, splitAt);
      secondLine = ellipsizeText(text.substring(splitAt + 1), maxWidth, font);
    }
  }

  display.setTextColor(color, background);
  const int fontH = display.fontHeight(font);
  if (secondLine.isEmpty()) {
    display.setTextDatum(MC_DATUM);
    display.drawString(firstLine, x + maxWidth / 2, y + maxHeight / 2, font);
  } else {
    display.setTextDatum(MC_DATUM);
    display.drawString(firstLine, x + maxWidth / 2, y + maxHeight / 2 - fontH / 2 + 1, font);
    display.drawString(secondLine, x + maxWidth / 2, y + maxHeight / 2 + fontH / 2 - 1, font);
  }
}

void drawLyricsCard(uint32_t progressMs, bool forceRedraw = false) {
  const int currentLineIndex = lyricsClient.isReady() ? lyricsClient.getCurrentLineIndex(progressMs) : -1;
  if (!forceRedraw && currentLineIndex == displayedLyricIndex) {
    return;
  }
  displayedLyricIndex = currentLineIndex;

  display.fillRoundRect(LYRICS_CARD_X, LYRICS_CARD_Y, LYRICS_CARD_W, LYRICS_CARD_H, 8, COLOR_LYRICS_BG);
  display.drawRoundRect(LYRICS_CARD_X, LYRICS_CARD_Y, LYRICS_CARD_W, LYRICS_CARD_H, 8, COLOR_LYRICS_BORDER);

  if (lyricsClient.status() == LyricsStatus::Fetching) {
    display.setTextDatum(MC_DATUM);
    display.setTextColor(COLOR_MUTED, COLOR_LYRICS_BG);
    display.drawString("Searching lyrics...", LYRICS_CARD_X + LYRICS_CARD_W / 2,
                       LYRICS_CARD_Y + LYRICS_CARD_H / 2, 2);
  } else if (!lyricsClient.isReady()) {
    display.setTextDatum(MC_DATUM);
    display.setTextColor(COLOR_MUTED, COLOR_LYRICS_BG);
    display.drawString("♪ Instrumental ♪", LYRICS_CARD_X + LYRICS_CARD_W / 2,
                       LYRICS_CARD_Y + LYRICS_CARD_H / 2, 2);
  } else if (currentLineIndex >= 0 && currentLineIndex < static_cast<int>(lyricsClient.lines().size())) {
    const String& lineText = lyricsClient.lines()[currentLineIndex].text;
    drawWrappedCardText(lineText, LYRICS_CARD_X + 6, LYRICS_CARD_Y + 4,
                        LYRICS_CARD_W - 12, LYRICS_CARD_H - 8, 2, TFT_WHITE, COLOR_LYRICS_BG);
  }
}

String makePrefKey(const char* prefix, const String& id) {
  uint32_t hash = 5381;
  for (size_t i = 0; i < id.length(); ++i) {
    hash = ((hash << 5) + hash) + static_cast<uint8_t>(id[i]);
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%08X", prefix, hash);
  return String(buf);
}

void updatePlayCounts(const SpotifyTrack& track) {
  if (track.id.isEmpty()) return;

  Preferences pref;
  pref.begin("plays", false);

  const String tKey = makePrefKey("t_", track.id);
  const String aKey = makePrefKey("a_", track.artist);

  currentTrackPlayCount = pref.getUInt(tKey.c_str(), 0) + 1;
  currentArtistPlayCount = pref.getUInt(aKey.c_str(), 0) + 1;

  pref.putUInt(tKey.c_str(), currentTrackPlayCount);
  pref.putUInt(aKey.c_str(), currentArtistPlayCount);

  pref.end();
}

void drawPlayCounts() {
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COLOR_SPOTIFY, COLOR_SPOTIFY_SCREEN);
  display.drawString("Song: " + String(currentTrackPlayCount) + "x", 44, 119, 2);

  display.setTextColor(COLOR_MUTED, COLOR_SPOTIFY_SCREEN);
  display.drawString("Artist: " + String(currentArtistPlayCount) + "x", 44, 136, 2);
}

void drawSpotifyTrack(const SpotifyTrack& track) {
  display.fillRect(0, 42, SCREEN_WIDTH, SCREEN_HEIGHT - 42, COLOR_SPOTIFY_SCREEN);

  // Single-line track info to prevent overlapping
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_SPOTIFY_SCREEN);
  display.drawString(ellipsizeText(track.name, 190, 2), 86, 46, 2);

  display.setTextColor(COLOR_SPOTIFY, COLOR_SPOTIFY_SCREEN);
  display.drawString(ellipsizeText(track.artist, 190, 2), 86, 62, 2);

  display.setTextColor(COLOR_MUTED, COLOR_SPOTIFY_SCREEN);
  display.drawString(ellipsizeText(track.album, 190, 2), 86, 77, 2);

  drawConnectionBars();

  // Draw play counts right below album art
  drawPlayCounts();

  displayedLyricIndex = -2;
  drawLyricsCard(spotifyClient.estimatedProgressMs(), true);

  drawPlaybackControls(track.isPlaying);
  displayedProgressSecond = -1;
  drawSpotifyProgress();

  downloadAndDrawAlbumArt(track);
}

void drawSpotifyScreen() {
  drawSpotifyShell();
  displayedSpotifyStatus = SpotifyStatus::Idle;
  displayedTrackId = "";
  displayedImageUrl = "";
  lastCountedTrackId = "";
  displayedProgressSecond = -1;
  pressedSpotifyControl = -1;
  displayedLyricIndex = -2;
  spotifyClient.requestImmediateUpdate();
  drawSpotifyMessage("Loading Spotify", "Checking your current playback...");
}

void updateSpotifyDashboard() {
  spotifyClient.update();
  const SpotifyStatus status = spotifyClient.status();

  if (status == SpotifyStatus::Ready && spotifyClient.track().available) {
    const SpotifyTrack& track = spotifyClient.track();
    const bool trackChanged = track.id != displayedTrackId ||
                              track.imageUrl != displayedImageUrl ||
                              displayedSpotifyStatus != SpotifyStatus::Ready;
    if (trackChanged) {
      displayedTrackId = track.id;
      displayedImageUrl = track.imageUrl;
      displayedLyricIndex = -2;

      // Update and save persistent play counts only when track ID actually changed
      if (track.id != lastCountedTrackId && !track.id.isEmpty()) {
        lastCountedTrackId = track.id;
        updatePlayCounts(track);
      }

      drawSpotifyTrack(track);

      // Fetch lyrics from LRCLIB
      lyricsClient.fetchLyrics(track.name, track.artist, track.album,
                               track.durationMs / 1000);

      drawLyricsCard(spotifyClient.estimatedProgressMs(), true);
    } else {
      const uint32_t progressMs = spotifyClient.estimatedProgressMs();
      drawSpotifyProgress();
      drawLyricsCard(progressMs);
    }
    displayedSpotifyStatus = status;
    return;
  }

  // If a track is active and status is momentary retrying or authorizing, don't wipe the screen
  if (spotifyClient.track().available && status != SpotifyStatus::Offline &&
      status != SpotifyStatus::MissingCredentials && status != SpotifyStatus::AuthError &&
      status != SpotifyStatus::NothingPlaying) {
    const uint32_t progressMs = spotifyClient.estimatedProgressMs();
    drawSpotifyProgress();
    drawLyricsCard(progressMs);
    return;
  }

  if (status == displayedSpotifyStatus) {
    return;
  }
  displayedSpotifyStatus = status;

  switch (status) {
    case SpotifyStatus::MissingCredentials:
      drawSpotifyMessage("Setup required", "Fill the project .env file");
      break;
    case SpotifyStatus::Offline:
      drawSpotifyMessage("Wi-Fi offline", "Connect Wi-Fi from the main menu");
      break;
    case SpotifyStatus::Authorizing:
      drawSpotifyMessage("Connecting", "Refreshing Spotify authorization...");
      break;
    case SpotifyStatus::NothingPlaying:
      drawSpotifyMessage("Nothing playing", "Start a song in Spotify");
      break;
    case SpotifyStatus::AuthError:
      drawSpotifyMessage("Login required", spotifyClient.errorMessage());
      break;
    case SpotifyStatus::RateLimited:
      drawSpotifyMessage("Please wait", spotifyClient.errorMessage());
      break;
    case SpotifyStatus::ApiError:
      drawSpotifyMessage("Spotify error", spotifyClient.errorMessage());
      break;
    case SpotifyStatus::Idle:
      drawSpotifyMessage("Loading Spotify", "Checking your current playback...");
      break;
    case SpotifyStatus::Ready:
      break;
  }
}


void drawServerScreen() {
  display.fillScreen(COLOR_BACKGROUND);
  display.fillRect(0, 0, SCREEN_WIDTH, 42, 0x2945);
  drawBackButton(false);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, 0x2945);
  display.drawString("Server Stats", SCREEN_WIDTH / 2, 22, 2);
  display.drawFastHLine(0, 41, SCREEN_WIDTH, 0x4228);
  
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString("Loading stats from Glances...", SCREEN_WIDTH / 2, 120, 2);
  lastGlancesUpdate = 0; // Force immediate update
}

void drawRingGaugeSprite(TFT_eSprite* spr, int r, int thickness, float percent, uint16_t color, const char* label) {
  int width = r * 2 + 10;
  int centerX = width / 2;
  int centerY = r + 5;
  
  spr->fillSprite(COLOR_BACKGROUND);
  
  int totalSweep = 270;
  int fillSweep = (int)((percent / 100.0f) * totalSweep);
  if (fillSweep > totalSweep) fillSweep = totalSweep;
  if (fillSweep < 0) fillSweep = 0;
  
  for (int a = 0; a <= totalSweep; a++) {
    float rad = (135 + a) * 0.0174533f;
    int px = centerX + (r * cos(rad));
    int py = centerY + (r * sin(rad));
    uint16_t c = (a <= fillSweep) ? color : 0x2124;
    spr->fillCircle(px, py, thickness / 2, c);
  }
  
  char valBuf[16];
  snprintf(valBuf, sizeof(valBuf), "%.0f%%", percent);
  spr->setTextDatum(MC_DATUM);
  spr->setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  spr->drawString(valBuf, centerX, centerY, 2);
  
  spr->setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  spr->drawString(label, centerX, centerY + r + 15, 2);
}

void updateServerDashboard() {
  if (millis() - lastGlancesUpdate > 5000 || lastGlancesUpdate == 0) {
    bool isFirstLoad = (lastGlancesUpdate == 0);
    lastGlancesUpdate = millis();
    bool ok = glancesClient.update();
    
    if (isFirstLoad) {
      display.fillRect(0, 42, SCREEN_WIDTH, SCREEN_HEIGHT - 42, COLOR_BACKGROUND);
    }
    
    if (ok) {
      ServerStats stats = glancesClient.getStats();
      
      TFT_eSprite spr = TFT_eSprite(&display);
      int r = 36;
      int width = r * 2 + 10;
      int height = r * 2 + 40;
      spr.createSprite(width, height);
      
      drawRingGaugeSprite(&spr, r, 10, stats.cpu_percent, 0xFBA0, "CPU");
      spr.pushSprite(55 - width/2, 110 - (r+5));
      
      drawRingGaugeSprite(&spr, r, 10, stats.mem_percent, 0x1DC5, "RAM");
      spr.pushSprite(160 - width/2, 110 - (r+5));
      
      drawRingGaugeSprite(&spr, r, 10, stats.disk_percent, 0xBDF7, "DISK");
      spr.pushSprite(265 - width/2, 110 - (r+5));
      
      spr.deleteSprite();
      
      display.setTextDatum(MC_DATUM);
      display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
      char tempBuf[32];
      snprintf(tempBuf, sizeof(tempBuf), "CPU Temp: %.1f C", stats.temp_c);
      
      display.fillRect(0, 190, SCREEN_WIDTH, 30, COLOR_BACKGROUND);
      display.drawString(tempBuf, SCREEN_WIDTH / 2, 200, 2);
    } else {
      display.fillRect(0, 42, SCREEN_WIDTH, SCREEN_HEIGHT - 42, COLOR_BACKGROUND);
      display.setTextDatum(MC_DATUM);
      display.setTextColor(COLOR_OFFLINE, COLOR_BACKGROUND);
      display.drawString("Failed to connect to Glances", SCREEN_WIDTH / 2, 120, 2);
    }
  }
}

void handleServerTouch(bool isDown, int x, int y) {
  const bool isOverBack = isDown && BACK_BUTTON.contains(x, y);
  const bool backWasPressed = backButtonPressed;

  if (isOverBack != backButtonPressed) {
    backButtonPressed = isOverBack;
    drawBackButton(backButtonPressed);
  }

  if (!isDown && touchWasDown && backWasPressed) {
    backButtonPressed = false;
    currentScreen = Screen::Menu;
    drawMenu();
  }
}

void drawWifiSetupInstructions() {
  display.fillScreen(COLOR_BACKGROUND);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString("Wi-Fi setup", SCREEN_WIDTH / 2, 25, 4);

  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString("On your phone, connect to:", SCREEN_WIDTH / 2, 66, 2);
  display.setTextColor(COLOR_SPOTIFY, COLOR_BACKGROUND);
  display.drawString(WIFI_SETUP_NAME, SCREEN_WIDTH / 2, 94, 4);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString(String("Password: ") + WIFI_SETUP_PASSWORD,
                     SCREEN_WIDTH / 2, 128, 2);
  display.drawString("Then open 192.168.4.1", SCREEN_WIDTH / 2, 157, 2);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString("Setup portal closes after 5 minutes", SCREEN_WIDTH / 2, 207, 2);
}

void drawWifiSetupButton(bool pressed) {
  const uint16_t fill = pressed ? COLOR_PANEL_PRESSED : COLOR_PANEL;
  display.fillRoundRect(WIFI_SETUP_BUTTON.x, WIFI_SETUP_BUTTON.y,
                        WIFI_SETUP_BUTTON.width, WIFI_SETUP_BUTTON.height, 10, fill);
  display.drawRoundRect(WIFI_SETUP_BUTTON.x, WIFI_SETUP_BUTTON.y,
                        WIFI_SETUP_BUTTON.width, WIFI_SETUP_BUTTON.height, 10,
                        pressed ? TFT_WHITE : COLOR_SPOTIFY);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, fill);
  display.drawString(WiFi.status() == WL_CONNECTED ? "Change Wi-Fi" : "Connect Wi-Fi",
                     WIFI_SETUP_BUTTON.x + WIFI_SETUP_BUTTON.width / 2,
                     WIFI_SETUP_BUTTON.y + WIFI_SETUP_BUTTON.height / 2, 2);
}

void drawWifiScreen() {
  const bool connected = WiFi.status() == WL_CONNECTED;

  display.fillScreen(COLOR_BACKGROUND);
  drawBackButton(false);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString("Wi-Fi", 205, 31, 4);

  display.fillCircle(SCREEN_WIDTH / 2, 78, 8,
                     connected ? COLOR_SPOTIFY : COLOR_OFFLINE);
  display.setTextColor(connected ? COLOR_SPOTIFY : COLOR_OFFLINE, COLOR_BACKGROUND);
  display.drawString(connected ? "Connected" : "Offline", SCREEN_WIDTH / 2, 101, 2);

  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  if (connected) {
    display.drawString(WiFi.SSID(), SCREEN_WIDTH / 2, 123, 2);
    display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
    display.drawString(WiFi.localIP().toString(), SCREEN_WIDTH / 2, 141, 2);
  } else {
    display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
    display.drawString("No network connection", SCREEN_WIDTH / 2, 128, 2);
  }

  drawWifiSetupButton(false);
}

void startWifiPortal() {
  drawWifiSetupInstructions();
  wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SECONDS);
  const bool connected = wifiManager.startConfigPortal(WIFI_SETUP_NAME,
                                                        WIFI_SETUP_PASSWORD);
  WiFi.setAutoReconnect(true);
  Serial.println(connected ? "Wi-Fi configuration completed"
                           : "Wi-Fi setup portal timed out");
}

void onWifiPortalStarted(WiFiManager*) {
  Serial.println("Wi-Fi setup portal started");
  drawWifiSetupInstructions();
}

void connectWifiAtBoot() {
  display.fillScreen(COLOR_BACKGROUND);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_WHITE, COLOR_BACKGROUND);
  display.drawString("Connecting to Wi-Fi", SCREEN_WIDTH / 2, 103, 4);
  display.setTextColor(COLOR_MUTED, COLOR_BACKGROUND);
  display.drawString("Please wait...", SCREEN_WIDTH / 2, 137, 2);

  wifiManager.setAPCallback(onWifiPortalStarted);
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SECONDS);
  const bool connected = wifiManager.autoConnect(WIFI_SETUP_NAME,
                                                  WIFI_SETUP_PASSWORD);
  WiFi.setAutoReconnect(true);

  if (connected) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Starting without Wi-Fi after setup timeout");
  }
}

bool readTouch(int& screenX, int& screenY) {
  if (!touch.touched()) {
    return false;
  }

  TS_Point point = touch.getPoint();
  screenX = constrain(map(point.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCREEN_WIDTH - 1),
                      0, SCREEN_WIDTH - 1);
  screenY = constrain(map(point.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_HEIGHT - 1),
                      0, SCREEN_HEIGHT - 1);
  return true;
}

void handleMenuTouch(bool isDown, int x, int y) {
  int touchedModeIndex = -1;
  const bool isOverWifiButton = isDown && WIFI_STATUS_BUTTON.contains(x, y);
  if (isDown) {
    for (int index = 0; index < MODE_COUNT; ++index) {
      if (MODE_BUTTONS[index].contains(x, y)) {
        touchedModeIndex = index;
        break;
      }
    }
  }

  const int previouslyPressedIndex = pressedModeIndex;
  const bool wifiButtonWasPressed = wifiStatusButtonPressed;
  if (touchedModeIndex != pressedModeIndex) {
    if (pressedModeIndex >= 0) {
      drawModeButton(pressedModeIndex, false);
    }
    pressedModeIndex = touchedModeIndex;
    if (pressedModeIndex >= 0) {
      drawModeButton(pressedModeIndex, true);
    }
  }

  if (isOverWifiButton != wifiStatusButtonPressed) {
    wifiStatusButtonPressed = isOverWifiButton;
    drawWifiStatusButton(wifiStatusButtonPressed);
  }

  if (!isDown && touchWasDown && wifiButtonWasPressed) {
    wifiStatusButtonPressed = false;
    pressedModeIndex = -1;
    currentScreen = Screen::Wifi;
    drawWifiScreen();
    return;
  }

  if (!isDown && touchWasDown && previouslyPressedIndex == 0) {
    pressedModeIndex = -1;
    currentScreen = Screen::Spotify;
    Serial.println("Spotify Mode selected");
    drawSpotifyScreen();
  }

  if (!isDown && touchWasDown && previouslyPressedIndex == 1) {
    pressedModeIndex = -1;
    currentScreen = Screen::ServerStats;
    Serial.println("Server Stats Mode selected");
    drawServerScreen();
  }
}

void handleWifiTouch(bool isDown, int x, int y) {
  const bool isOverBack = isDown && BACK_BUTTON.contains(x, y);
  const bool isOverSetup = isDown && WIFI_SETUP_BUTTON.contains(x, y);
  const bool backWasPressed = backButtonPressed;
  const bool setupWasPressed = wifiSetupButtonPressed;

  if (isOverBack != backButtonPressed) {
    backButtonPressed = isOverBack;
    drawBackButton(backButtonPressed);
  }
  if (isOverSetup != wifiSetupButtonPressed) {
    wifiSetupButtonPressed = isOverSetup;
    drawWifiSetupButton(wifiSetupButtonPressed);
  }

  if (!isDown && touchWasDown && backWasPressed) {
    backButtonPressed = false;
    wifiSetupButtonPressed = false;
    currentScreen = Screen::Menu;
    drawMenu();
    return;
  }

  if (!isDown && touchWasDown && setupWasPressed) {
    backButtonPressed = false;
    wifiSetupButtonPressed = false;
    startWifiPortal();
    drawWifiScreen();
  }
}

void handleSpotifyTouch(bool isDown, int x, int y) {
  const bool isOverBack = isDown && SPOTIFY_BACK_BUTTON.contains(x, y);
  int touchedControl = -1;

  if (isDown && spotifyClient.track().available) {
    if (SPOTIFY_PREVIOUS_BUTTON.contains(x, y)) {
      touchedControl = 0;
    } else if (SPOTIFY_PLAY_BUTTON.contains(x, y)) {
      touchedControl = 1;
    } else if (SPOTIFY_NEXT_BUTTON.contains(x, y)) {
      touchedControl = 2;
    }
  }

  const bool wasBackPressed = backButtonPressed;
  const int previouslyPressedControl = pressedSpotifyControl;

  if (isOverBack != backButtonPressed) {
    backButtonPressed = isOverBack;
    drawSpotifyBackButton(backButtonPressed);
  }

  if (touchedControl != pressedSpotifyControl) {
    pressedSpotifyControl = touchedControl;
    if (spotifyClient.track().available) {
      drawPlaybackControls(spotifyClient.track().isPlaying,
                           pressedSpotifyControl);
    }
  }

  if (!isDown && touchWasDown && wasBackPressed) {
    backButtonPressed = false;
    pressedSpotifyControl = -1;
    currentScreen = Screen::Menu;
    drawMenu();
    return;
  }

  if (!isDown && touchWasDown && previouslyPressedControl >= 0) {
    pressedSpotifyControl = -1;
    bool commandSucceeded = false;
    if (previouslyPressedControl == 0) {
      commandSucceeded = spotifyClient.previousTrack();
    } else if (previouslyPressedControl == 1) {
      commandSucceeded = spotifyClient.togglePlayPause();
    } else {
      commandSucceeded = spotifyClient.nextTrack();
    }
    if (commandSucceeded && spotifyClient.track().available) {
      drawPlaybackControls(spotifyClient.track().isPlaying);
      displayedProgressSecond = -1;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("Starting touch menu...");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  display.init();
  display.setRotation(1);

  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);

  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpegOutput);

  connectWifiAtBoot();
  spotifyClient.begin();
  drawMenu();
  Serial.println("Touch menu ready");
}

void loop() {
  int touchX = 0;
  int touchY = 0;
  const bool isTouchDown = readTouch(touchX, touchY);

  switch (currentScreen) {
    case Screen::Menu:
      handleMenuTouch(isTouchDown, touchX, touchY);
      break;
    case Screen::Spotify:
      handleSpotifyTouch(isTouchDown, touchX, touchY);
      if (currentScreen == Screen::Spotify) {
        updateSpotifyDashboard();
      }
      break;
    case Screen::ServerStats:
      handleServerTouch(isTouchDown, touchX, touchY);
      if (currentScreen == Screen::ServerStats) {
        updateServerDashboard();
      }
      break;
    case Screen::Wifi:
      handleWifiTouch(isTouchDown, touchX, touchY);
      break;
  }

  touchWasDown = isTouchDown;
  delay(15);
}
