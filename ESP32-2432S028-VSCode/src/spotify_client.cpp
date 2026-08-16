#include "spotify_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "generated_config.h"

namespace {

constexpr char TOKEN_URL[] = "https://accounts.spotify.com/api/token";
constexpr char CURRENTLY_PLAYING_URL[] =
    "https://api.spotify.com/v1/me/player/currently-playing";
constexpr char PREVIOUS_TRACK_URL[] =
    "https://api.spotify.com/v1/me/player/previous";
constexpr char NEXT_TRACK_URL[] = "https://api.spotify.com/v1/me/player/next";
constexpr char PLAY_URL[] = "https://api.spotify.com/v1/me/player/play";
constexpr char PAUSE_URL[] = "https://api.spotify.com/v1/me/player/pause";

String urlEncode(const String& value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '_' || character == '.' || character == '~') {
      encoded += static_cast<char>(character);
    } else {
      encoded += '%';
      encoded += hex[character >> 4];
      encoded += hex[character & 0x0F];
    }
  }
  return encoded;
}

bool timeReached(uint32_t targetMs) {
  return static_cast<int32_t>(millis() - targetMs) >= 0;
}

}  // namespace

void SpotifyClient::begin() {
  if (!credentialsConfigured()) {
    setError(SpotifyStatus::MissingCredentials, "Add Spotify credentials");
    return;
  }

  Preferences preferences;
  preferences.begin("spotify", false);
  const String configuredToken = SPOTIFY_REFRESH_TOKEN;
  const String storedSource = preferences.getString("source", "");
  if (storedSource == configuredToken) {
    refreshToken_ = preferences.getString("refresh", configuredToken);
  } else {
    refreshToken_ = configuredToken;
    preferences.putString("source", configuredToken);
    preferences.putString("refresh", configuredToken);
  }
  preferences.end();
  status_ = SpotifyStatus::Idle;
  consecutiveErrors_ = 0;
  nextPollAtMs_ = 0;
}

bool SpotifyClient::credentialsConfigured() const {
  return strlen(SPOTIFY_CLIENT_ID) > 10 && strlen(SPOTIFY_REFRESH_TOKEN) > 10 &&
         strncmp(SPOTIFY_CLIENT_ID, "YOUR_", 5) != 0 &&
         strncmp(SPOTIFY_REFRESH_TOKEN, "YOUR_", 5) != 0;
}

SpotifyStatus SpotifyClient::status() const {
  return status_;
}

const SpotifyTrack& SpotifyClient::track() const {
  return currentTrack_;
}

const String& SpotifyClient::errorMessage() const {
  return errorMessage_;
}

uint32_t SpotifyClient::estimatedProgressMs() const {
  if (!currentTrack_.available) {
    return 0;
  }

  uint32_t progress = currentTrack_.progressMs;
  if (currentTrack_.isPlaying) {
    progress += millis() - currentTrack_.fetchedAtMs;
  }
  return min(progress, currentTrack_.durationMs);
}

void SpotifyClient::requestImmediateUpdate() {
  nextPollAtMs_ = 0;
}

bool SpotifyClient::previousTrack() {
  return sendPlayerCommand(PREVIOUS_TRACK_URL, "POST");
}

bool SpotifyClient::togglePlayPause() {
  const bool wasPlaying = currentTrack_.isPlaying;
  if (!sendPlayerCommand(wasPlaying ? PAUSE_URL : PLAY_URL, "PUT")) {
    return false;
  }
  currentTrack_.isPlaying = !wasPlaying;
  currentTrack_.progressMs = estimatedProgressMs();
  currentTrack_.fetchedAtMs = millis();
  return true;
}

bool SpotifyClient::nextTrack() {
  return sendPlayerCommand(NEXT_TRACK_URL, "POST");
}

void SpotifyClient::update() {
  if (!credentialsConfigured()) {
    if (status_ != SpotifyStatus::MissingCredentials) {
      setError(SpotifyStatus::MissingCredentials, "Add Spotify credentials");
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setError(SpotifyStatus::Offline, "Wi-Fi is offline");
    return;
  }
  if (nextPollAtMs_ != 0 && !timeReached(nextPollAtMs_)) {
    return;
  }

  if (accessToken_.isEmpty() || timeReached(accessTokenExpiresAtMs_)) {
    if (!refreshAccessToken(currentTrack_.available)) {
      nextPollAtMs_ = millis() + 8000;
      return;
    }
  }
  fetchCurrentlyPlaying();
}

bool SpotifyClient::sendPlayerCommand(const char* url, const char* method) {
  if (WiFi.status() != WL_CONNECTED) {
    setError(SpotifyStatus::Offline, "Wi-Fi disconnected");
    return false;
  }
  if (accessToken_.isEmpty() || timeReached(accessTokenExpiresAtMs_)) {
    if (!refreshAccessToken(true)) {
      return false;
    }
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(2000);
  if (!http.begin(secureClient, url)) {
    if (currentTrack_.available) {
      nextPollAtMs_ = millis() + 1000;
    } else {
      setError(SpotifyStatus::ApiError, "Could not open Spotify control");
    }
    return false;
  }
  http.addHeader("Authorization", "Bearer " + accessToken_);
  http.addHeader("Content-Length", "0");
  const int responseCode = http.sendRequest(method);
  http.end();

  if (responseCode >= 200 && responseCode < 300) {
    Serial.printf("Spotify control %s succeeded (HTTP %d)\n", method, responseCode);
    nextPollAtMs_ = millis() + 1000;
    consecutiveErrors_ = 0;
    return true;
  }

  Serial.printf("Spotify control error %d (%s)\n", responseCode,
                HTTPClient::errorToString(responseCode).c_str());
  if (responseCode == HTTP_CODE_UNAUTHORIZED) {
    accessToken_ = "";
    refreshAccessToken(true);
  }
  nextPollAtMs_ = millis() + (responseCode < 0 ? 1000 : 2500);
  return false;
}

bool SpotifyClient::refreshAccessToken(bool isSilent) {
  if (!isSilent) {
    status_ = SpotifyStatus::Authorizing;
    errorMessage_ = "Refreshing Spotify login";
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(secureClient, TOKEN_URL)) {
    if (!isSilent) {
      setError(SpotifyStatus::AuthError, "Could not reach token service");
    }
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String body = "grant_type=refresh_token&refresh_token=" +
                      urlEncode(refreshToken_) + "&client_id=" +
                      urlEncode(SPOTIFY_CLIENT_ID);
  const int responseCode = http.POST(body);
  const String response = http.getString();
  http.end();

  if (responseCode != HTTP_CODE_OK) {
    Serial.printf("Spotify token error %d: %s\n", responseCode, response.c_str());
    if (responseCode == HTTP_CODE_BAD_REQUEST) {
      setError(SpotifyStatus::AuthError, "Spotify login expired");
    } else if (!isSilent) {
      setError(SpotifyStatus::AuthError, "Spotify token error " + String(responseCode));
    }
    accessToken_ = "";
    return false;
  }

  JsonDocument filter;
  filter["access_token"] = true;
  filter["expires_in"] = true;
  filter["refresh_token"] = true;

  JsonDocument document;
  const DeserializationError jsonError =
      deserializeJson(document, response, DeserializationOption::Filter(filter));
  if (jsonError) {
    if (!isSilent) {
      setError(SpotifyStatus::AuthError, "Invalid token response");
    }
    return false;
  }

  accessToken_ = document["access_token"] | "";
  const uint32_t expiresInSeconds = document["expires_in"] | 3600;
  const uint32_t safeMargin = (expiresInSeconds > 120) ? (expiresInSeconds - 60) : expiresInSeconds;
  accessTokenExpiresAtMs_ = millis() + safeMargin * 1000UL;

  const String rotatedRefreshToken = document["refresh_token"] | "";
  if (!rotatedRefreshToken.isEmpty()) {
    refreshToken_ = rotatedRefreshToken;
    Preferences preferences;
    preferences.begin("spotify", false);
    preferences.putString("refresh", refreshToken_);
    preferences.end();
  }

  if (accessToken_.isEmpty()) {
    if (!isSilent) {
      setError(SpotifyStatus::AuthError, "Token response was empty");
    }
    return false;
  }

  consecutiveErrors_ = 0;
  return true;
}

void SpotifyClient::fetchCurrentlyPlaying() {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(secureClient, CURRENTLY_PLAYING_URL)) {
    consecutiveErrors_++;
    if (!currentTrack_.available || consecutiveErrors_ >= 4) {
      setError(SpotifyStatus::ApiError, "Could not reach Spotify");
    }
    nextPollAtMs_ = millis() + POLL_INTERVAL_MS;
    return;
  }

  const char* headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("Authorization", "Bearer " + accessToken_);
  const int responseCode = http.GET();

  if (responseCode == HTTP_CODE_NO_CONTENT) {
    http.end();
    clearTrack();
    status_ = SpotifyStatus::NothingPlaying;
    errorMessage_ = "Nothing is playing";
    consecutiveErrors_ = 0;
    nextPollAtMs_ = millis() + POLL_INTERVAL_MS;
    return;
  }

  if (responseCode == HTTP_CODE_UNAUTHORIZED) {
    http.end();
    Serial.println("Spotify API 401 unauthorized - refreshing token");
    accessToken_ = "";
    refreshAccessToken(true);
    nextPollAtMs_ = millis() + 500;
    return;
  }

  if (responseCode == HTTP_CODE_TOO_MANY_REQUESTS) {
    const uint32_t retrySeconds = max(5L, http.header("Retry-After").toInt());
    http.end();
    Serial.printf("Spotify rate limited - waiting %lu s\n", (unsigned long)retrySeconds);
    if (!currentTrack_.available) {
      setError(SpotifyStatus::RateLimited, "Spotify rate limit; waiting");
    }
    nextPollAtMs_ = millis() + retrySeconds * 1000UL;
    return;
  }

  if (responseCode != HTTP_CODE_OK) {
    http.end();
    Serial.printf("Spotify API error %d (%s)\n", responseCode,
                  HTTPClient::errorToString(responseCode).c_str());
    consecutiveErrors_++;
    if (currentTrack_.available && consecutiveErrors_ < 4) {
      // Keep track visible during transient glitches
      nextPollAtMs_ = millis() + 3000;
      return;
    }
    setError(SpotifyStatus::ApiError,
             responseCode < 0 ? "Spotify connection timeout" : ("Spotify error " + String(responseCode)));
    nextPollAtMs_ = millis() + 6000;
    return;
  }

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["show"]["name"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["album"]["images"] = true;
  filter["item"]["images"] = true;

  JsonDocument document;
  const String payload = http.getString();
  http.end();

  const DeserializationError jsonError =
      deserializeJson(document, payload, DeserializationOption::Filter(filter));
  if (jsonError) {
    Serial.printf("Spotify JSON decode error: %s\n", jsonError.c_str());
    consecutiveErrors_++;
    if (!currentTrack_.available || consecutiveErrors_ >= 4) {
      setError(SpotifyStatus::ApiError, "Invalid Spotify response");
    }
    nextPollAtMs_ = millis() + POLL_INTERVAL_MS;
    return;
  }

  JsonObject item = document["item"].as<JsonObject>();
  if (item.isNull()) {
    clearTrack();
    status_ = SpotifyStatus::NothingPlaying;
    errorMessage_ = "Nothing is playing";
    consecutiveErrors_ = 0;
    nextPollAtMs_ = millis() + POLL_INTERVAL_MS;
    return;
  }

  currentTrack_.id = item["id"] | "";
  currentTrack_.name = item["name"] | "Unknown title";
  currentTrack_.artist = item["artists"][0]["name"] | "";
  if (currentTrack_.artist.isEmpty()) {
    currentTrack_.artist = item["show"]["name"] | "Unknown artist";
  }
  currentTrack_.album = item["album"]["name"] | "";
  currentTrack_.durationMs = item["duration_ms"] | 0;
  currentTrack_.progressMs = document["progress_ms"] | 0;
  currentTrack_.fetchedAtMs = millis();
  currentTrack_.isPlaying = document["is_playing"] | false;
  currentTrack_.available = true;

  currentTrack_.imageUrl = "";
  currentTrack_.imageWidth = 0;
  JsonArray images = item["album"]["images"].as<JsonArray>();
  if (images.isNull() || images.size() == 0) {
    images = item["images"].as<JsonArray>();
  }
  uint16_t bestDifference = UINT16_MAX;
  for (JsonObject image : images) {
    const uint16_t width = image["width"] | 0;
    const char* url = image["url"] | "";
    if (url[0] != '\0') {
      const uint16_t difference = width > 64 ? width - 64 : 64 - width;
      if (difference < bestDifference) {
        bestDifference = difference;
        currentTrack_.imageUrl = url;
        currentTrack_.imageWidth = width;
      }
    }
  }

  status_ = SpotifyStatus::Ready;
  errorMessage_ = "";
  consecutiveErrors_ = 0;
  nextPollAtMs_ = millis() + POLL_INTERVAL_MS;
}

void SpotifyClient::clearTrack() {
  currentTrack_ = SpotifyTrack();
}

void SpotifyClient::setError(SpotifyStatus status, const String& message) {
  status_ = status;
  errorMessage_ = message;
}
