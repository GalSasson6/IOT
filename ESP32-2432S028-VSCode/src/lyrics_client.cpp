#include "lyrics_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

void LyricsClient::reset() {
  lines_.clear();
  status_ = LyricsStatus::Idle;
}

String LyricsClient::urlEncode(const String& value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

String LyricsClient::cleanSongTitle(const String& rawTitle) {
  String title = rawTitle;

  // Remove " - Remaster...", " - Live...", " - Radio Edit...", " - Stereo...", " - Mono...", " - From..."
  int dash = title.indexOf(" - ");
  if (dash > 0) {
    title = title.substring(0, dash);
  }

  // Remove "(feat...", "(with...", "(Remaster...", "(Live...", "(Radio...", "(Official..."
  int paren = title.indexOf('(');
  if (paren > 0) {
    title = title.substring(0, paren);
  }

  // Remove "[...]"
  int bracket = title.indexOf('[');
  if (bracket > 0) {
    title = title.substring(0, bracket);
  }

  title.trim();
  return title.isEmpty() ? rawTitle : title;
}

String LyricsClient::fetchFromLrclibGet(const String& trackName, const String& artistName,
                                        const String& albumName, uint32_t durationSec) {
  String url = String("https://lrclib.net/api/get?track_name=") + urlEncode(trackName) +
               "&artist_name=" + urlEncode(artistName);
  if (!albumName.isEmpty()) {
    url += "&album_name=" + urlEncode(albumName);
  }
  if (durationSec > 0) {
    url += "&duration=" + String(durationSec);
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(2500);

  if (!http.begin(secureClient, url)) {
    return "";
  }

  http.addHeader("User-Agent", "ESP32-SpotifyPlayer/1.0");
  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    http.end();
    return "";
  }

  const String response = http.getString();
  http.end();

  JsonDocument filter;
  filter["syncedLyrics"] = true;

  JsonDocument doc;
  const DeserializationError jsonError =
      deserializeJson(doc, response, DeserializationOption::Filter(filter));
  if (jsonError) {
    return "";
  }

  return doc["syncedLyrics"] | "";
}

String LyricsClient::fetchFromLrclibSearch(const String& query) {
  String url = String("https://lrclib.net/api/search?q=") + urlEncode(query);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(2500);

  if (!http.begin(secureClient, url)) {
    return "";
  }

  http.addHeader("User-Agent", "ESP32-SpotifyPlayer/1.0");
  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    http.end();
    return "";
  }

  const String response = http.getString();
  http.end();

  JsonDocument filter;
  filter[0]["syncedLyrics"] = true;

  JsonDocument doc;
  const DeserializationError jsonError =
      deserializeJson(doc, response, DeserializationOption::Filter(filter));
  if (jsonError) {
    return "";
  }

  JsonArray array = doc.as<JsonArray>();
  if (array.isNull() || array.size() == 0) {
    return "";
  }

  return array[0]["syncedLyrics"] | "";
}

String LyricsClient::fetchFromNetease(const String& trackName, const String& artistName) {
  // Step 1: Search song ID
  String searchUrl = String("http://music.163.com/api/search/get?s=") +
                     urlEncode(trackName + " " + artistName) + "&type=1&limit=1";

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(2500);

  if (!http.begin(client, searchUrl)) {
    return "";
  }

  http.addHeader("User-Agent", "Mozilla/5.0");
  http.addHeader("Referer", "http://music.163.com/");

  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    http.end();
    return "";
  }

  const String response = http.getString();
  http.end();

  JsonDocument filter;
  filter["result"]["songs"][0]["id"] = true;

  JsonDocument doc;
  const DeserializationError jsonError =
      deserializeJson(doc, response, DeserializationOption::Filter(filter));
  if (jsonError) {
    return "";
  }

  const long songId = doc["result"]["songs"][0]["id"] | 0L;
  if (songId <= 0) {
    return "";
  }

  // Step 2: Fetch lyrics by song ID
  String lyricUrl = String("http://music.163.com/api/song/lyric?id=") +
                    String(songId) + "&lv=1&kv=1&tv=-1";

  if (!http.begin(client, lyricUrl)) {
    return "";
  }

  http.addHeader("User-Agent", "Mozilla/5.0");
  http.addHeader("Referer", "http://music.163.com/");

  const int lyricCode = http.GET();
  if (lyricCode != HTTP_CODE_OK) {
    http.end();
    return "";
  }

  const String lyricResponse = http.getString();
  http.end();

  JsonDocument lyricFilter;
  lyricFilter["lrc"]["lyric"] = true;

  JsonDocument lyricDoc;
  const DeserializationError lyricError =
      deserializeJson(lyricDoc, lyricResponse, DeserializationOption::Filter(lyricFilter));
  if (lyricError) {
    return "";
  }

  return lyricDoc["lrc"]["lyric"] | "";
}

bool LyricsClient::fetchLyrics(const String& trackName, const String& artistName,
                                const String& albumName, uint32_t durationSec) {
  reset();
  status_ = LyricsStatus::Fetching;

  if (WiFi.status() != WL_CONNECTED || trackName.isEmpty() || artistName.isEmpty()) {
    status_ = LyricsStatus::Error;
    return false;
  }

  Serial.printf("Searching lyrics for: %s - %s\n", trackName.c_str(), artistName.c_str());

  // 1. Primary: Exact match from LRCLIB
  String syncedLyrics = fetchFromLrclibGet(trackName, artistName, albumName, durationSec);

  // 2. Fallback: Cleaned title query without duration/album restrictions
  const String cleaned = cleanSongTitle(trackName);
  if (syncedLyrics.isEmpty() && (cleaned != trackName || !albumName.isEmpty() || durationSec > 0)) {
    Serial.printf("LRCLIB exact match miss. Retrying with cleaned title: %s\n", cleaned.c_str());
    syncedLyrics = fetchFromLrclibGet(cleaned, artistName, "", 0);
  }

  // 3. Fallback: LRCLIB fuzzy search
  if (syncedLyrics.isEmpty()) {
    Serial.println("LRCLIB get missed. Trying LRCLIB search...");
    syncedLyrics = fetchFromLrclibSearch(cleaned + " " + artistName);
  }

  // 4. Fallback: Secondary source (NetEase Cloud Music)
  if (syncedLyrics.isEmpty()) {
    Serial.println("LRCLIB missed. Trying NetEase Cloud Music...");
    syncedLyrics = fetchFromNetease(cleaned, artistName);
  }

  if (syncedLyrics.isEmpty()) {
    Serial.println("No synchronized lyrics found across all providers");
    status_ = LyricsStatus::NotFound;
    return false;
  }

  parseLrc(syncedLyrics);

  if (lines_.empty()) {
    Serial.println("No valid timestamped lines after parsing LRC");
    status_ = LyricsStatus::NotFound;
    return false;
  }

  Serial.printf("Loaded %u synchronized lyric lines\n", lines_.size());
  status_ = LyricsStatus::Ready;
  return true;
}

void LyricsClient::parseLrc(const String& lrcContent) {
  lines_.clear();
  lines_.reserve(48);
  int start = 0;
  const int len = lrcContent.length();

  while (start < len) {
    int lineEnd = lrcContent.indexOf('\n', start);
    if (lineEnd == -1) {
      lineEnd = len;
    }

    String line = lrcContent.substring(start, lineEnd);
    line.trim();
    if (line.endsWith("\r")) {
      line.remove(line.length() - 1);
      line.trim();
    }

    start = lineEnd + 1;

    if (line.startsWith("[")) {
      int bracketClose = line.indexOf(']');
      if (bracketClose > 1) {
        String timeStr = line.substring(1, bracketClose);
        String text = line.substring(bracketClose + 1);
        text.trim();

        // Must start with a digit to filter out [ti:...], [ar:...], [offset:...]
        if (timeStr.length() > 0 && isdigit(timeStr[0])) {
          int colon = timeStr.indexOf(':');
          if (colon > 0) {
            int min = timeStr.substring(0, colon).toInt();
            String secStr = timeStr.substring(colon + 1);

            int dot = secStr.indexOf('.');
            if (dot == -1) {
              dot = secStr.indexOf(',');
            }

            int sec = 0;
            int ms = 0;
            if (dot > 0) {
              sec = secStr.substring(0, dot).toInt();
              String msStr = secStr.substring(dot + 1);
              if (msStr.length() == 2) {
                ms = msStr.toInt() * 10;
              } else if (msStr.length() >= 3) {
                ms = msStr.substring(0, 3).toInt();
              } else if (msStr.length() == 1) {
                ms = msStr.toInt() * 100;
              }
            } else {
              sec = secStr.toInt();
            }

            uint32_t timestampMs = (static_cast<uint32_t>(min) * 60 + sec) * 1000 + ms;
            LyricLine lyricLine;
            lyricLine.timestampMs = timestampMs;
            lyricLine.text = text;
            lines_.push_back(lyricLine);
          }
        }
      }
    }
  }
}

int LyricsClient::getCurrentLineIndex(uint32_t progressMs) const {
  if (lines_.empty()) {
    return -1;
  }
  int index = -1;
  for (size_t i = 0; i < lines_.size(); ++i) {
    if (lines_[i].timestampMs <= progressMs) {
      index = static_cast<int>(i);
    } else {
      break;
    }
  }
  return index;
}

