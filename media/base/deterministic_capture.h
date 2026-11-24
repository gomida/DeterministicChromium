// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_BASE_DETERMINISTIC_CAPTURE_H_
#define MEDIA_BASE_DETERMINISTIC_CAPTURE_H_

#include "base/command_line.h"

namespace media {

// DeterministicChromium: true in processes started with --deterministic-capture
// (headless_shell forwards the switch to its renderers). Media code uses it to
// wait for decoded data instead of dropping or underflowing, so that frame
// selection does not depend on decode speed (deterministic-capture/DESIGN.md
// 4.5). The switch name is spelled out here because media/ cannot depend on
// headless/public/switches.h.
inline bool IsDeterministicCaptureMode() {
  static const bool enabled =
      base::CommandLine::ForCurrentProcess()->HasSwitch("deterministic-capture");
  return enabled;
}

}  // namespace media

#endif  // MEDIA_BASE_DETERMINISTIC_CAPTURE_H_
