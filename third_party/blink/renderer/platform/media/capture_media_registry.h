// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_MEDIA_CAPTURE_MEDIA_REGISTRY_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_MEDIA_CAPTURE_MEDIA_REGISTRY_H_

#include <cstdint>

#include "third_party/blink/public/platform/media/web_capture_media_registry.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

class WebMediaPlayerImpl;

// DeterministicChromium: the players of this renderer in creation order, and a
// transition counter, for WebCaptureMediaRegistry. Exists only under
// --deterministic-capture; everything runs on the renderer main thread.
class PLATFORM_EXPORT CaptureMediaRegistry {
 public:
  // nullptr when the process is not in capture mode.
  static CaptureMediaRegistry* GetIfEnabled();

  CaptureMediaRegistry() = default;  // for DEFINE_STATIC_LOCAL

  void Register(WebMediaPlayerImpl* player);
  void Unregister(WebMediaPlayerImpl* player);
  void NoteTransition() { ++transitions_; }
  WebCaptureMediaSnapshot Snapshot() const;

 private:
  Vector<WebMediaPlayerImpl*> players_;
  uint64_t transitions_ = 0;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_MEDIA_CAPTURE_MEDIA_REGISTRY_H_
