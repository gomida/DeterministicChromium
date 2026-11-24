// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_RAND_UTIL_DETERMINISTIC_CAPTURE_H_
#define BASE_RAND_UTIL_DETERMINISTIC_CAPTURE_H_

#include <stdint.h>

#include "base/base_export.h"
#include "base/containers/span.h"

namespace base::subtle {

// DeterministicChromium: replaces the OS entropy source behind base::RandBytes
// (and so crypto::RandBytes, base::Uuid, blob URLs and crypto.getRandomValues)
// for the whole process. Installed only by renderers started with
// --deterministic-capture so that a capture's random values repeat from run to
// run (deterministic-capture/DESIGN.md 4.8). Kept out of rand_util.h so the
// hook does not recompile everything that includes it. Never set in ordinary
// builds; the function must not allocate.
using RandBytesOverrideFunction = void (*)(span<uint8_t> output);
BASE_EXPORT void SetRandBytesOverride(RandBytesOverrideFunction function);

}  // namespace base::subtle

#endif  // BASE_RAND_UTIL_DETERMINISTIC_CAPTURE_H_
