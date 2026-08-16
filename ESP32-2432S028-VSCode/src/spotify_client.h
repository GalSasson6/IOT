#pragma once

#include <Arduino.h>

enum class SpotifyStatus {
  Idle,
  Ready,
  MissingCredentials,
  Offline,
  Authorizing,
  NothingPlaying,
  AuthError,
  ApiError,
  RateLimited,
};

struct SpotifyTrack {
  String id;
  String name;
  String artist;
  String album;
  String imageUrl;
  uint16_t imageWidth = 0;
  uint32_t durationMs = 0;
  uint32_t progressMs = 0;
  uint32_t fetchedAtMs = 0;
  bool isPlaying = false;
  bool available = false;
};

class SpotifyClient {
 public:
  void begin();
  void update();
  void requestImmediateUpdate();
  bool previousTrack();
  bool togglePlayPause();
  bool nextTrack();

  bool credentialsConfigured() const;
  SpotifyStatus status() const;
  const SpotifyTrack& track() const;
  const String& errorMessage() const;
  uint32_t estimatedProgressMs() const;

 private:
  static constexpr uint32_t POLL_INTERVAL_MS = 5000;

  bool refreshAccessToken(bool isSilent = false);
  bool sendPlayerCommand(const char* url, const char* method);
  void fetchCurrentlyPlaying();
  void clearTrack();
  void setError(SpotifyStatus status, const String& message);

  SpotifyTrack currentTrack_;
  SpotifyStatus status_ = SpotifyStatus::Idle;
  String errorMessage_;
  String accessToken_;
  String refreshToken_;
  uint32_t accessTokenExpiresAtMs_ = 0;
  uint32_t nextPollAtMs_ = 0;
  uint8_t consecutiveErrors_ = 0;
};

