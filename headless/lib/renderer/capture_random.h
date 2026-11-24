// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_RENDERER_CAPTURE_RANDOM_H_
#define HEADLESS_LIB_RENDERER_CAPTURE_RANDOM_H_

#include <cstdint>

namespace headless {

// Replaces this process's entropy source (base::RandBytes and everything on
// top of it: crypto::RandBytes, base::Uuid, blob URLs, crypto.getRandomValues)
// with a seeded xoshiro256** stream, so a capture repeats its random values
// run to run (deterministic-capture/DESIGN.md 4.8). Call once, early, in a
// --deterministic-capture renderer. Not a cryptographic generator.
void InstallDeterministicRandom(uint64_t seed);

}  // namespace headless

#endif  // HEADLESS_LIB_RENDERER_CAPTURE_RANDOM_H_
