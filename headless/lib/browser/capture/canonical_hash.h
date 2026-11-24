// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_BROWSER_CAPTURE_CANONICAL_HASH_H_
#define HEADLESS_LIB_BROWSER_CAPTURE_CANONICAL_HASH_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "base/containers/span.h"

class SkBitmap;

namespace headless::capture {

// Canonical pixel form of a captured frame (DESIGN.md 4.3): 8-bit
// premultiplied BGRA in memory byte order B,G,R,A (Skia N32 on desktop
// builds), rows packed without padding, sRGB. Hashes and sinks use this form
// directly from the readback bitmap; the browser converts nothing.
inline constexpr char kCanonicalPixelFormat[] = "bgra8-premul";
inline constexpr char kCanonicalHashName[] = "sha256-bgra8-premul";

using Sha256Digest = std::array<uint8_t, 32>;

size_t CanonicalBytes(int width, int height);

// True when the bitmap's memory already is the canonical layout, so rows can
// be hashed and copied in place.
bool IsCanonicalLayout(const SkBitmap& bitmap);

// SHA-256 over the canonical bytes, row by row, without an intermediate copy
// when the layout already matches. nullopt when the bitmap has no pixels or
// an unsupported color type.
std::optional<Sha256Digest> Sha256OfBitmap(const SkBitmap& bitmap);

// Copies the canonical bytes into |dst| (exactly CanonicalBytes()). Returns
// false on a size or format mismatch.
bool CopyCanonical(const SkBitmap& bitmap, base::span<uint8_t> dst);

std::string HexLower(const Sha256Digest& digest);
std::string Sha256Hex(base::span<const uint8_t> bytes);
std::string ChainHash(const std::string& chain, const std::string& frame_hash);

}  // namespace headless::capture

#endif  // HEADLESS_LIB_BROWSER_CAPTURE_CANONICAL_HASH_H_
