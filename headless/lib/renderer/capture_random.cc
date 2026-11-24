// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/renderer/capture_random.h"

#include <atomic>
#include <cstring>

#include "base/containers/span.h"
#include "base/rand_util_deterministic_capture.h"

namespace headless {

namespace {

// State protected by a spin lock: RandBytes may run inside the allocator shim,
// so this path allocates nothing and depends on nothing in base.
uint64_t g_state[4];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

uint64_t SplitMix64(uint64_t& x) {
  uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

uint64_t Rotl(uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

// xoshiro256** (Blackman & Vigna), public domain.
uint64_t NextLocked() {
  const uint64_t result = Rotl(g_state[1] * 5, 7) * 9;
  const uint64_t t = g_state[1] << 17;
  g_state[2] ^= g_state[0];
  g_state[3] ^= g_state[1];
  g_state[1] ^= g_state[2];
  g_state[0] ^= g_state[3];
  g_state[2] ^= t;
  g_state[3] = Rotl(g_state[3], 45);
  return result;
}

void DeterministicRandBytes(base::span<uint8_t> output) {
  while (g_lock.test_and_set(std::memory_order_acquire)) {
  }
  size_t i = 0;
  while (i < output.size()) {
    const uint64_t word = NextLocked();
    const size_t n = output.size() - i < 8 ? output.size() - i : 8;
    memcpy(output.data() + i, &word, n);
    i += n;
  }
  g_lock.clear(std::memory_order_release);
}

}  // namespace

void InstallDeterministicRandom(uint64_t seed) {
  uint64_t x = seed ^ 0x44434150'52414E44ULL;  // "DCAPRAND"
  for (uint64_t& word : g_state) {
    word = SplitMix64(x);
  }
  base::subtle::SetRandBytesOverride(&DeterministicRandBytes);
}

}  // namespace headless
