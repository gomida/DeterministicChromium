// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_BROWSER_CAPTURE_SHM_RING_H_
#define HEADLESS_LIB_BROWSER_CAPTURE_SHM_RING_H_

#include <cstddef>
#include <cstdint>

// Layout of the shared-memory frame ring between the engine (producer) and
// the controller's encoder (consumer). The controller creates the mapping,
// fills the header and passes three descriptors at launch: the ring, a
// "ready" eventfd the producer signals after publishing a slot and a "free"
// eventfd the consumer signals after releasing one. All integers are
// little-endian. The Rust side (deterministic-capture/dcap/src/shm.rs)
// mirrors these constants; change both together and bump kVersion.
namespace headless::capture::ring {

inline constexpr uint64_t kMagic = 0x31474E5250414344ULL;  // "DCAPRNG1"
inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kHeaderBytes = 4096;
inline constexpr size_t kSlotHeaderBytes = 256;

// Header fields (byte offsets).
inline constexpr size_t kOffMagic = 0;               // u64
inline constexpr size_t kOffVersion = 8;             // u32
inline constexpr size_t kOffSlotCount = 12;          // u32
inline constexpr size_t kOffSlotBytes = 16;          // u64, multiple of 4096
inline constexpr size_t kOffWidth = 24;              // u32
inline constexpr size_t kOffHeight = 28;             // u32
inline constexpr size_t kOffStride = 32;             // u32, == width * 4
inline constexpr size_t kOffPixelFormat = 36;        // u32, kPixelFormatBgra8Premul
inline constexpr size_t kOffAudioRate = 40;          // u32
inline constexpr size_t kOffAudioChannels = 44;      // u32
inline constexpr size_t kOffAudioFormat = 48;        // u32, 0 none, 1 s16, 2 f32
inline constexpr size_t kOffAudioFramesPerSlot = 52; // u32
inline constexpr size_t kOffVideoBytes = 56;         // u64, == stride * height
inline constexpr size_t kOffAudioBytes = 64;         // u64, audio capacity per slot
inline constexpr size_t kOffProduced = 128;          // u64 atomic, slots published
inline constexpr size_t kOffConsumed = 192;          // u64 atomic, slots released
inline constexpr size_t kOffClosed = 256;            // u32 atomic, producer finished
inline constexpr size_t kOffProducerError = 260;     // u32 atomic, 1 = aborted/incomplete

// Slot fields (byte offsets from the slot start; slot i starts at
// kHeaderBytes + i * slot_bytes).
inline constexpr size_t kSlotFrameIndex = 0;         // i64
inline constexpr size_t kSlotVirtualTimeUs = 8;      // i64, frame time offset
inline constexpr size_t kSlotFlags = 16;             // u32
inline constexpr size_t kSlotAudioFrames = 20;       // u32, sample frames present
inline constexpr size_t kSlotVideoBytes = 24;        // u64, bytes written
inline constexpr size_t kSlotHash = 32;              // 32 bytes, SHA-256 of video
inline constexpr size_t kSlotHashPresent = 64;       // u32
inline constexpr size_t kSlotVideoData = kSlotHeaderBytes;  // then audio

inline constexpr uint32_t kFlagVideo = 1u << 0;
inline constexpr uint32_t kFlagAudio = 1u << 1;

inline constexpr uint32_t kPixelFormatBgra8Premul = 1;
inline constexpr uint32_t kAudioFormatNone = 0;
inline constexpr uint32_t kAudioFormatS16 = 1;
inline constexpr uint32_t kAudioFormatF32 = 2;

}  // namespace headless::capture::ring

#endif  // HEADLESS_LIB_BROWSER_CAPTURE_SHM_RING_H_
