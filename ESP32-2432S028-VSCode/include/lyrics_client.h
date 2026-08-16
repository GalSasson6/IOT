#pragma once

#include <Arduino.h>
#include <vector>

enum class LyricsStatus {
  Idle,
  Fetching,
  Ready,
  NotFound,
  Error,
};

struct LyricLine {
  uint32_t timestampMs = 0;
  String text;
};

class LyricsClient {
 public:
  void reset();
  bool fetchLyrics(const String& trackName, const String& artistName,
                   const String& albumName, uint32_t durationSec);

  LyricsStatus status() const { return status_; }
  bool isReady() const { return status_ == LyricsStatus::Ready && !lines_.empty(); }
  const std::vector<LyricLine>& lines() const { return lines_; }
  int getCurrentLineIndex(uint32_t progressMs) const;

 private:
  void parseLrc(const String& lrcContent);
  static String urlEncode(const String& value);
  static String cleanSongTitle(const String& rawTitle);
  static String fetchFromLrclibGet(const String& trackName, const String& artistName,
                                   const String& albumName, uint32_t durationSec);
  static String fetchFromLrclibSearch(const String& query);
  static String fetchFromNetease(const String& trackName, const String& artistName);

  LyricsStatus status_ = LyricsStatus::Idle;
  std::vector<LyricLine> lines_;
};

