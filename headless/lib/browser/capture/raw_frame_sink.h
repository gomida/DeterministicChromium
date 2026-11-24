// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_BROWSER_CAPTURE_RAW_FRAME_SINK_H_
#define HEADLESS_LIB_BROWSER_CAPTURE_RAW_FRAME_SINK_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "headless/lib/browser/capture/canonical_hash.h"

class SkBitmap;

namespace headless::capture {

struct SinkError {
  int64_t frame_index = -1;
  int error_number = 0;
  bool peer_closed = false;
  int64_t bytes_into_frame = 0;
  int64_t frames_fully_written = 0;
  std::string message;
};

// Destination of canonical frames (DESIGN.md 4.4 and 13.3).
//   shm:  a shared-memory ring created by the controller; the frame is copied
//         once into a free slot on the calling (worker) thread, which blocks
//         while the ring is full, up to stall_timeout.
//   file: packed canonical BGRA appended to a file through a writer thread
//         (debugging and tests).
//   null: frames are counted and dropped.
// Never writes more than expected_frames.
class RawFrameSink : public base::PlatformThread::Delegate {
 public:
  enum class Type { kNull, kFile, kShm };

  struct Config {
    Type type = Type::kNull;
    std::string path;      // kFile
    int ring_fd = -1;      // kShm: mapping
    int ready_fd = -1;     // kShm: eventfd signalled after publishing a slot
    int free_fd = -1;      // kShm: eventfd signalled by the consumer per release
    std::optional<int64_t> expected_frames;
    int max_queued_frames = 8;
    base::TimeDelta stall_timeout = base::Seconds(60);
  };

  struct Frame {
    int64_t frame_index = 0;
    int64_t virtual_time_us = 0;
    std::shared_ptr<const SkBitmap> bitmap;
    std::optional<Sha256Digest> hash;
  };

  explicit RawFrameSink(Config config);
  RawFrameSink(const RawFrameSink&) = delete;
  RawFrameSink& operator=(const RawFrameSink&) = delete;
  ~RawFrameSink() override;

  // Opens the destination: creates the file, or maps and validates the ring.
  std::optional<SinkError> Open();

  // Writes one frame. May block (file: queue full; shm: ring full) up to
  // stall_timeout. Returns the first error observed, if any.
  std::optional<SinkError> Write(const Frame& frame);

  // Finishes the stream: drains the file queue and closes the descriptor, or
  // marks the ring closed and wakes the consumer. |aborted| marks the stream
  // incomplete for the consumer.
  std::optional<SinkError> Close(bool aborted = false);

  int64_t frames_written() const;
  int64_t frames_queued() const;
  // Ring geometry for validation by the session (0 when not a ring).
  int ring_width() const { return ring_width_; }
  int ring_height() const { return ring_height_; }

 private:
  struct Pending {
    int64_t frame_index;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
  };

  std::optional<SinkError> OpenRing();
  std::optional<SinkError> WriteRing(const Frame& frame);
  std::optional<SinkError> WriteFile(const Frame& frame);

  // base::PlatformThread::Delegate:
  void ThreadMain() override;
  bool WriteAll(const Pending& pending, SinkError* error);

  const Config config_;
  int fd_ = -1;
  base::PlatformThreadHandle thread_;
  bool thread_started_ = false;

  // Ring state (kShm). The mapping is mmap(2) memory owned by this object,
  // not a PartitionAlloc allocation, so raw_ptr does not apply.
  RAW_PTR_EXCLUSION uint8_t* ring_ = nullptr;
  size_t ring_size_ = 0;
  uint32_t slot_count_ = 0;
  uint64_t slot_bytes_ = 0;
  uint64_t video_bytes_ = 0;
  int ring_width_ = 0;
  int ring_height_ = 0;
  bool closed_ = false;

  mutable base::Lock lock_;
  base::ConditionVariable has_data_;
  base::ConditionVariable has_space_;
  std::deque<Pending> queue_;
  bool stop_ = false;
  std::optional<SinkError> error_;
  int64_t frames_written_ = 0;
};

}  // namespace headless::capture

#endif  // HEADLESS_LIB_BROWSER_CAPTURE_RAW_FRAME_SINK_H_
