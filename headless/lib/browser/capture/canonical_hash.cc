// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/capture/canonical_hash.h"

#include <string.h>

#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "crypto/hash.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"

namespace headless::capture {

namespace {

// Visits each canonical row. The readback bitmap is N32 premultiplied, which
// is BGRA on every desktop build; rows are then taken in place. Any other
// color type goes through Skia one row at a time (no full-frame copy).
template <typename RowFn>
bool ForEachCanonicalRow(const SkBitmap& bitmap, RowFn fn) {
  const int width = bitmap.width();
  const int height = bitmap.height();
  if (width <= 0 || height <= 0 || !bitmap.getPixels()) {
    return false;
  }
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  if (IsCanonicalLayout(bitmap)) {
    for (int y = 0; y < height; ++y) {
      fn(base::span<const uint8_t>(
          static_cast<const uint8_t*>(bitmap.getAddr(0, y)), row_bytes));
    }
    return true;
  }
  std::vector<uint8_t> row(row_bytes);
  const SkImageInfo info = SkImageInfo::Make(
      width, 1, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
  for (int y = 0; y < height; ++y) {
    if (!bitmap.readPixels(info, row.data(), row_bytes, 0, y)) {
      return false;
    }
    fn(base::span<const uint8_t>(row));
  }
  return true;
}

}  // namespace

size_t CanonicalBytes(int width, int height) {
  if (width <= 0 || height <= 0) {
    return 0;
  }
  return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

bool IsCanonicalLayout(const SkBitmap& bitmap) {
  return bitmap.colorType() == kBGRA_8888_SkColorType &&
         bitmap.alphaType() == kPremul_SkAlphaType && bitmap.getPixels();
}

std::optional<Sha256Digest> Sha256OfBitmap(const SkBitmap& bitmap) {
  crypto::hash::Hasher hasher(crypto::hash::kSha256);
  const bool ok = ForEachCanonicalRow(
      bitmap, [&hasher](base::span<const uint8_t> row) { hasher.Update(row); });
  if (!ok) {
    return std::nullopt;
  }
  Sha256Digest digest;
  hasher.Finish(digest);
  return digest;
}

bool CopyCanonical(const SkBitmap& bitmap, base::span<uint8_t> dst) {
  if (dst.size() != CanonicalBytes(bitmap.width(), bitmap.height())) {
    return false;
  }
  size_t offset = 0;
  return ForEachCanonicalRow(bitmap, [&](base::span<const uint8_t> row) {
    memcpy(dst.data() + offset, row.data(), row.size());
    offset += row.size();
  });
}

std::string HexLower(const Sha256Digest& digest) {
  return base::ToLowerASCII(base::HexEncode(digest));
}

std::string Sha256Hex(base::span<const uint8_t> bytes) {
  return HexLower(crypto::hash::Sha256(bytes));
}

std::string ChainHash(const std::string& chain, const std::string& frame_hash) {
  const std::string input = chain + frame_hash;
  return HexLower(crypto::hash::Sha256(std::string_view(input)));
}

}  // namespace headless::capture
