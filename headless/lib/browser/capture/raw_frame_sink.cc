// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/capture/raw_frame_sink.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

#include "base/posix/eintr_wrapper.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "headless/lib/browser/capture/shm_ring.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace headless::capture {

namespace {

template <typename T>
T LoadAcquire(const uint8_t* base, size_t offset) {
  return __atomic_load_n(reinterpret_cast<const T*>(base + offset),
                         __ATOMIC_ACQUIRE);
}

template <typename T>
void StoreRelease(uint8_t* base, size_t offset, T value) {
  __atomic_store_n(reinterpret_cast<T*>(base + offset), value,
                   __ATOMIC_RELEASE);
}

template <typename T>
T LoadPlain(const uint8_t* base, size_t offset) {
  T value;
  memcpy(&value, base + offset, sizeof(T));
  return value;
}

template <typename T>
void StorePlain(uint8_t* base, size_t offset, T value) {
  memcpy(base + offset, &value, sizeof(T));
}

bool SignalEventFd(int fd) {
  const uint64_t one = 1;
  return HANDLE_EINTR(write(fd, &one, sizeof(one))) == static_cast<ssize_t>(sizeof(one));
}

// Waits until the eventfd is readable or the deadline passes; consumes the
// counter. Returns false on timeout or error.
bool WaitEventFd(int fd, base::TimeTicks deadline) {
  for (;;) {
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (remaining <= base::TimeDelta()) {
      return false;
    }
    pollfd pfd{fd, POLLIN, 0};
    const int rc = HANDLE_EINTR(poll(&pfd, 1, static_cast<int>(remaining.InMilliseconds()) + 1));
    if (rc < 0) {
      return false;
    }
    if (rc == 0) {
      continue;
    }
    uint64_t count = 0;
    const ssize_t n = HANDLE_EINTR(read(fd, &count, sizeof(count)));
    if (n == static_cast<ssize_t>(sizeof(count))) {
      return true;
    }
    if (n < 0 && errno != EAGAIN) {
      return false;
    }
  }
}

}  // namespace

RawFrameSink::RawFrameSink(Config config)
    : config_(std::move(config)), has_data_(&lock_), has_space_(&lock_) {}

RawFrameSink::~RawFrameSink() {
  Close(/*aborted=*/true);
  if (ring_) {
    munmap(ring_, ring_size_);
    ring_ = nullptr;
  }
}

std::optional<SinkError> RawFrameSink::Open() {
  switch (config_.type) {
    case Type::kNull:
      return std::nullopt;
    case Type::kFile:
      fd_ = HANDLE_EINTR(
          open(config_.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
      if (fd_ < 0) {
        return SinkError{-1, errno, false, 0, 0, "SinkNotConnected: open failed"};
      }
      return std::nullopt;
    case Type::kShm:
      return OpenRing();
  }
  return std::nullopt;
}

std::optional<SinkError> RawFrameSink::OpenRing() {
  if (config_.ring_fd < 0 || config_.ready_fd < 0 || config_.free_fd < 0) {
    return SinkError{-1, 0, false, 0, 0, "SinkNotConnected: shm sink needs ringFd, readyFd and freeFd"};
  }
  struct stat st;
  if (fstat(config_.ring_fd, &st) != 0 || st.st_size < static_cast<off_t>(ring::kHeaderBytes)) {
    return SinkError{-1, errno, false, 0, 0, "SinkNotConnected: ring descriptor is not a mapping"};
  }
  ring_size_ = static_cast<size_t>(st.st_size);
  void* mapped = mmap(nullptr, ring_size_, PROT_READ | PROT_WRITE, MAP_SHARED, config_.ring_fd, 0);
  if (mapped == MAP_FAILED) {
    return SinkError{-1, errno, false, 0, 0, "SinkNotConnected: mmap of the ring failed"};
  }
  ring_ = static_cast<uint8_t*>(mapped);
  if (LoadPlain<uint64_t>(ring_, ring::kOffMagic) != ring::kMagic ||
      LoadPlain<uint32_t>(ring_, ring::kOffVersion) != ring::kVersion) {
    return SinkError{-1, 0, false, 0, 0, "SinkNotConnected: ring magic or version mismatch"};
  }
  if (LoadPlain<uint32_t>(ring_, ring::kOffPixelFormat) != ring::kPixelFormatBgra8Premul) {
    return SinkError{-1, 0, false, 0, 0, "InvalidParameters: ring pixel format is not bgra8-premul"};
  }
  const uint32_t ring_audio_format = LoadPlain<uint32_t>(ring_, ring::kOffAudioFormat);
  if (ring_audio_format != ring::kAudioFormatNone) {
    if (ring_audio_format != config_.audio_format ||
        LoadPlain<uint32_t>(ring_, ring::kOffAudioRate) != config_.audio_rate ||
        LoadPlain<uint32_t>(ring_, ring::kOffAudioChannels) != config_.audio_channels) {
      return SinkError{-1, 0, false, 0, 0, "AudioFormatUnsupported: ring audio format differs from the capture configuration"};
    }
    ring_audio_bytes_ = LoadPlain<uint64_t>(ring_, ring::kOffAudioBytes);
  }
  slot_count_ = LoadPlain<uint32_t>(ring_, ring::kOffSlotCount);
  slot_bytes_ = LoadPlain<uint64_t>(ring_, ring::kOffSlotBytes);
  video_bytes_ = LoadPlain<uint64_t>(ring_, ring::kOffVideoBytes);
  ring_width_ = static_cast<int>(LoadPlain<uint32_t>(ring_, ring::kOffWidth));
  ring_height_ = static_cast<int>(LoadPlain<uint32_t>(ring_, ring::kOffHeight));
  const uint64_t needed = ring::kHeaderBytes + static_cast<uint64_t>(slot_count_) * slot_bytes_;
  if (slot_count_ == 0 || slot_bytes_ < ring::kSlotHeaderBytes + video_bytes_ || needed > ring_size_ ||
      video_bytes_ != CanonicalBytes(ring_width_, ring_height_)) {
    return SinkError{-1, 0, false, 0, 0, "InvalidParameters: ring geometry is inconsistent"};
  }
  return std::nullopt;
}

std::optional<SinkError> RawFrameSink::Write(const Frame& frame) {
  switch (config_.type) {
    case Type::kNull: {
      base::AutoLock lock(lock_);
      ++frames_written_;
      return std::nullopt;
    }
    case Type::kFile:
      return WriteFile(frame);
    case Type::kShm:
      return WriteRing(frame);
  }
  return std::nullopt;
}

std::optional<SinkError> RawFrameSink::WriteRing(const Frame& frame) {
  base::AutoLock lock(lock_);
  if (error_) {
    return error_;
  }
  if (config_.expected_frames && frames_written_ >= *config_.expected_frames) {
    error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                       "IncompleteOutput: more frames than expectedFrames"};
    return error_;
  }
  if (!frame.bitmap || frame.bitmap->width() != ring_width_ || frame.bitmap->height() != ring_height_) {
    error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                       "InvalidViewport: frame size differs from the ring geometry"};
    return error_;
  }
  const base::TimeTicks deadline = base::TimeTicks::Now() + config_.stall_timeout;
  const uint64_t produced = LoadAcquire<uint64_t>(ring_, ring::kOffProduced);
  for (;;) {
    const uint64_t consumed = LoadAcquire<uint64_t>(ring_, ring::kOffConsumed);
    if (produced - consumed < slot_count_) {
      break;
    }
    if (!WaitEventFd(config_.free_fd, deadline)) {
      error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                         "SinkStalled: consumer did not release a ring slot"};
      return error_;
    }
  }
  uint8_t* slot = ring_ + ring::kHeaderBytes + (produced % slot_count_) * slot_bytes_;
  StorePlain<int64_t>(slot, ring::kSlotFrameIndex, frame.frame_index);
  StorePlain<int64_t>(slot, ring::kSlotVirtualTimeUs, frame.virtual_time_us);
  uint32_t flags = ring::kFlagVideo;
  uint32_t audio_frames = 0;
  if (frame.audio && frame.audio_frames > 0 && ring_audio_bytes_ > 0) {
    if (frame.audio->size() > ring_audio_bytes_) {
      error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                         "AudioFormatUnsupported: interval PCM exceeds the ring's audio capacity"};
      return error_;
    }
    memcpy(slot + ring::kSlotVideoData + video_bytes_, frame.audio->data(), frame.audio->size());
    flags |= ring::kFlagAudio;
    audio_frames = frame.audio_frames;
  }
  StorePlain<uint32_t>(slot, ring::kSlotFlags, flags);
  StorePlain<uint32_t>(slot, ring::kSlotAudioFrames, audio_frames);
  StorePlain<uint64_t>(slot, ring::kSlotVideoBytes, video_bytes_);
  if (frame.hash) {
    memcpy(slot + ring::kSlotHash, frame.hash->data(), frame.hash->size());
    StorePlain<uint32_t>(slot, ring::kSlotHashPresent, 1);
  } else {
    memset(slot + ring::kSlotHash, 0, 32);
    StorePlain<uint32_t>(slot, ring::kSlotHashPresent, 0);
  }
  if (!CopyCanonical(*frame.bitmap, base::span<uint8_t>(slot + ring::kSlotVideoData, video_bytes_))) {
    error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                       "NoSurface: readback bitmap could not be copied"};
    return error_;
  }
  StoreRelease<uint64_t>(ring_, ring::kOffProduced, produced + 1);
  ++frames_written_;
  if (!SignalEventFd(config_.ready_fd)) {
    error_ = SinkError{frame.frame_index, errno, errno == EPIPE, 0, frames_written_,
                       "SinkClosed: consumer eventfd write failed"};
    return error_;
  }
  return std::nullopt;
}

std::optional<SinkError> RawFrameSink::WriteFile(const Frame& frame) {
  if (!frame.bitmap) {
    return SinkError{frame.frame_index, 0, false, 0, frames_written(), "NoSurface: no bitmap"};
  }
  auto bytes = std::make_shared<std::vector<uint8_t>>(
      CanonicalBytes(frame.bitmap->width(), frame.bitmap->height()));
  if (!CopyCanonical(*frame.bitmap, base::span<uint8_t>(*bytes))) {
    return SinkError{frame.frame_index, 0, false, 0, frames_written(), "NoSurface: readback bitmap could not be copied"};
  }
  base::AutoLock lock(lock_);
  if (error_) {
    return error_;
  }
  if (config_.expected_frames &&
      frames_written_ + static_cast<int64_t>(queue_.size()) >= *config_.expected_frames) {
    error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                       "IncompleteOutput: more frames than expectedFrames"};
    return error_;
  }
  if (!thread_started_) {
    thread_started_ = base::PlatformThread::Create(0, this, &thread_);
    if (!thread_started_) {
      error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                         "SinkStalled: writer thread did not start"};
      return error_;
    }
  }
  const base::TimeTicks deadline = base::TimeTicks::Now() + config_.stall_timeout;
  while (!error_ && static_cast<int>(queue_.size()) >= config_.max_queued_frames) {
    const base::TimeDelta remaining = deadline - base::TimeTicks::Now();
    if (remaining <= base::TimeDelta()) {
      error_ = SinkError{frame.frame_index, 0, false, 0, frames_written_,
                         "SinkStalled: file writer did not drain the queue"};
      return error_;
    }
    has_space_.TimedWait(remaining);
  }
  if (error_) {
    return error_;
  }
  queue_.push_back(Pending{frame.frame_index, std::move(bytes)});
  has_data_.Signal();
  return std::nullopt;
}

std::optional<SinkError> RawFrameSink::Close(bool aborted) {
  {
    base::AutoLock lock(lock_);
    stop_ = true;
    has_data_.Broadcast();
    has_space_.Broadcast();
  }
  if (thread_started_) {
    base::PlatformThread::Join(thread_);
    thread_started_ = false;
  }
  if (fd_ >= 0) {
    IGNORE_EINTR(close(fd_));
    fd_ = -1;
  }
  if (ring_ && !closed_) {
    closed_ = true;
    if (aborted) {
      StoreRelease<uint32_t>(ring_, ring::kOffProducerError, 1);
    }
    StoreRelease<uint32_t>(ring_, ring::kOffClosed, 1);
    SignalEventFd(config_.ready_fd);
  }
  base::AutoLock lock(lock_);
  return error_;
}

int64_t RawFrameSink::frames_written() const {
  base::AutoLock lock(lock_);
  return frames_written_;
}

int64_t RawFrameSink::frames_queued() const {
  base::AutoLock lock(lock_);
  if (config_.type == Type::kShm && ring_) {
    return static_cast<int64_t>(LoadAcquire<uint64_t>(ring_, ring::kOffProduced) -
                                LoadAcquire<uint64_t>(ring_, ring::kOffConsumed));
  }
  return static_cast<int64_t>(queue_.size());
}

void RawFrameSink::ThreadMain() {
  base::PlatformThread::SetName("DeterministicCaptureSink");
  for (;;) {
    Pending pending;
    {
      base::AutoLock lock(lock_);
      while (queue_.empty() && !stop_) {
        has_data_.Wait();
      }
      if (queue_.empty()) {
        return;  // stop_ and drained
      }
      pending = std::move(queue_.front());
      queue_.pop_front();
    }
    SinkError error;
    const bool ok = WriteAll(pending, &error);
    base::AutoLock lock(lock_);
    if (!ok) {
      if (!error_) {
        error_ = error;
      }
      queue_.clear();
      has_space_.Broadcast();
      return;
    }
    ++frames_written_;
    has_space_.Signal();
  }
}

bool RawFrameSink::WriteAll(const Pending& pending, SinkError* error) {
  const uint8_t* data = pending.bytes->data();
  size_t remaining = pending.bytes->size();
  int64_t written_total = 0;
  while (remaining > 0) {
    const ssize_t written = HANDLE_EINTR(write(fd_, data, remaining));
    if (written < 0) {
      *error = SinkError{pending.frame_index, errno, errno == EPIPE,
                         written_total, frames_written_,
                         errno == EPIPE ? "SinkClosed: consumer closed the file"
                                        : "SinkClosed: write failed"};
      return false;
    }
    data += written;
    remaining -= static_cast<size_t>(written);
    written_total += written;
  }
  return true;
}

}  // namespace headless::capture
