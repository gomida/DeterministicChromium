// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/media/capture_media_registry.h"

#include "media/base/deterministic_capture.h"
#include "third_party/blink/renderer/platform/media/web_media_player_impl.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"

namespace blink {

// static
CaptureMediaRegistry* CaptureMediaRegistry::GetIfEnabled() {
  if (!media::IsDeterministicCaptureMode()) {
    return nullptr;
  }
  DCHECK(IsMainThread());
  DEFINE_STATIC_LOCAL(CaptureMediaRegistry, registry, ());
  return &registry;
}

void CaptureMediaRegistry::Register(WebMediaPlayerImpl* player) {
  players_.push_back(player);
}

void CaptureMediaRegistry::Unregister(WebMediaPlayerImpl* player) {
  wtf_size_t index = players_.Find(player);
  if (index != kNotFound) {
    players_.EraseAt(index);
  }
}

WebCaptureMediaSnapshot CaptureMediaRegistry::Snapshot() const {
  WebCaptureMediaSnapshot snapshot;
  snapshot.transitions = transitions_;
  for (WebMediaPlayerImpl* player : players_) {
    WebCaptureMediaState state;
    player->CaptureState(&state);
    snapshot.players.push_back(std::move(state));
  }
  return snapshot;
}

// static
bool WebCaptureMediaRegistry::Enabled() {
  return CaptureMediaRegistry::GetIfEnabled() != nullptr;
}

// static
WebCaptureMediaSnapshot WebCaptureMediaRegistry::Snapshot() {
  CaptureMediaRegistry* registry = CaptureMediaRegistry::GetIfEnabled();
  return registry ? registry->Snapshot() : WebCaptureMediaSnapshot();
}

}  // namespace blink
