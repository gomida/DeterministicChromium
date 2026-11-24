// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_PLATFORM_MEDIA_WEB_CAPTURE_MEDIA_REGISTRY_H_
#define THIRD_PARTY_BLINK_PUBLIC_PLATFORM_MEDIA_WEB_CAPTURE_MEDIA_REGISTRY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "third_party/blink/public/platform/web_common.h"

namespace blink {

// DeterministicChromium: what the renderer's CaptureAgent needs to know about
// the media players of this process between two captured frames
// (deterministic-capture/DESIGN.md 4.3 step 3). Renderer main thread only.
struct WebCaptureMediaState {
  int player_id = 0;
  // idle | starting | playing | paused | seeking | transition | ended
  std::string state;
  double media_time_ms = 0;
  // No pipeline operation (start, seek, suspend, resume, track change) is in
  // flight or queued.
  bool stable = true;
  bool has_video = false;
  bool has_audio = false;
};

struct WebCaptureMediaSnapshot {
  std::vector<WebCaptureMediaState> players;
  // Count of play, pause, seek and rate changes since the process started.
  // Two equal snapshots around a media-thread round trip mean no transition
  // was started in between.
  uint64_t transitions = 0;
};

class BLINK_PLATFORM_EXPORT WebCaptureMediaRegistry {
 public:
  // False unless the process runs with --deterministic-capture.
  static bool Enabled();
  static WebCaptureMediaSnapshot Snapshot();
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_PLATFORM_MEDIA_WEB_CAPTURE_MEDIA_REGISTRY_H_
