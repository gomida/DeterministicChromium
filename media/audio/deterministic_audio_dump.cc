// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/deterministic_audio_dump.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <limits>
#include <utility>

#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/posix/eintr_wrapper.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "media/audio/fake_audio_output_stream.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_timestamp_helper.h"

namespace media {

namespace {

constexpr char kHeadlessRawAudioDumpSwitch[] = "headless-raw-audio-dump";
constexpr size_t kWavHeaderSize = 44;
constexpr std::array<uint8_t, 4> kRiff = {'R', 'I', 'F', 'F'};
constexpr std::array<uint8_t, 8> kWaveFormat = {'W', 'A', 'V', 'E',
                                                'f', 'm', 't', ' '};
constexpr std::array<uint8_t, 4> kData = {'d', 'a', 't', 'a'};

bool WriteAllAt(int fd, base::span<const uint8_t> bytes, off_t offset) {
  while (!bytes.empty()) {
    const ssize_t written =
        HANDLE_EINTR(pwrite(fd, bytes.data(), bytes.size(), offset));
    if (written <= 0) {
      return false;
    }
    bytes = bytes.subspan(static_cast<size_t>(written));
    offset += written;
  }
  return true;
}

void WriteLittleEndian16(base::span<uint8_t> bytes,
                         size_t offset,
                         uint16_t value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >> 8) & 0xff;
}

void WriteLittleEndian32(base::span<uint8_t> bytes,
                         size_t offset,
                         uint32_t value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >> 8) & 0xff;
  bytes[offset + 2] = (value >> 16) & 0xff;
  bytes[offset + 3] = (value >> 24) & 0xff;
}

}  // namespace

DeterministicAudioPump::DeterministicAudioPump(
    scoped_refptr<base::SequencedTaskRunner> worker_task_runner)
    : worker_task_runner_(std::move(worker_task_runner)) {}

DeterministicAudioPump::~DeterministicAudioPump() = default;

void DeterministicAudioPump::Activate(FakeAudioOutputStream* stream) {
  base::AutoLock lock(stream_lock_);
  CHECK(!stream_ || stream_ == stream);
  stream_ = stream;
}

void DeterministicAudioPump::Deactivate(FakeAudioOutputStream* stream) {
  base::AutoLock lock(stream_lock_);
  if (stream_ == stream) {
    stream_ = nullptr;
  }
}

bool DeterministicAudioPump::PostPump(std::vector<int16_t>* samples,
                                      base::RepeatingClosure done) {
  return worker_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&DeterministicAudioPump::PumpOnWorker,
                                base::RetainedRef(this),
                                base::Unretained(samples), std::move(done)));
}

void DeterministicAudioPump::PumpOnWorker(std::vector<int16_t>* samples,
                                          base::RepeatingClosure done) {
  {
    base::AutoLock lock(stream_lock_);
    if (stream_) {
      stream_->PumpForDeterministicCapture(samples);
    }
  }
  done.Run();
}

// static
bool DeterministicAudioDump::IsEnabled() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      kHeadlessRawAudioDumpSwitch);
}

// static
DeterministicAudioDump* DeterministicAudioDump::GetIfEnabled() {
  if (!IsEnabled()) {
    return nullptr;
  }
  static base::NoDestructor<DeterministicAudioDump> instance;
  return instance.get();
}

DeterministicAudioDump::DeterministicAudioDump() = default;

DeterministicAudioDump::~DeterministicAudioDump() {
  if (output_fd_ >= 0) {
    IGNORE_EINTR(close(output_fd_));
  }
}

void DeterministicAudioDump::RegisterStream(
    scoped_refptr<DeterministicAudioPump> pump,
    const AudioParameters& params) {
  base::AutoLock lock(stream_lock_);
  if (params.sample_rate() <= 0 || params.channels() <= 0) {
    stream_error_ = "Deterministic audio received an invalid output format";
    return;
  }
  if (sample_rate_ == 0) {
    sample_rate_ = params.sample_rate();
    channels_ = params.channels();
  } else if (params.sample_rate() != sample_rate_ ||
             params.channels() != channels_) {
    stream_error_ = "Deterministic audio output format changed from " +
                    std::to_string(sample_rate_) + " Hz / " +
                    std::to_string(channels_) + " channels to " +
                    std::to_string(params.sample_rate()) + " Hz / " +
                    std::to_string(params.channels()) + " channels";
    return;
  }
  if (pump_ && pump_ != pump) {
    stream_error_ =
        "Deterministic audio supports one active output device at a time";
    return;
  }
  pump_ = std::move(pump);
}

void DeterministicAudioDump::UnregisterStream(DeterministicAudioPump* pump) {
  base::AutoLock lock(stream_lock_);
  if (pump_.get() != pump) {
    return;
  }
  pump_.reset();
}

int64_t DeterministicAudioDump::FrameAt(base::TimeTicks target) const {
  CHECK(origin_.has_value());
  if (target <= *origin_) {
    return 0;
  }
  return AudioTimestampHelper::TimeToFrames(target - *origin_, sample_rate_);
}

std::optional<std::string> DeterministicAudioDump::AdvanceTo(
    base::TimeTicks target) {
  {
    base::AutoLock lock(stream_lock_);
    if (stream_error_.has_value()) {
      return stream_error_;
    }
    if (sample_rate_ == 0 || channels_ == 0) {
      return "Deterministic audio requires an active output stream before "
             "the first beginFrame";
    }
  }
  if (!origin_.has_value()) {
    origin_ = target;
  }
  const int64_t target_frame = FrameAt(target);
  std::optional<std::string> error = EnsureGeneratedThrough(target_frame);
  if (error.has_value()) {
    return error;
  }
  if (!recording_started_ && target_frame > consumed_frames_) {
    ConsumeFrames(target_frame - consumed_frames_, nullptr);
  }
  return std::nullopt;
}

std::optional<std::string> DeterministicAudioDump::CaptureInterval(
    base::TimeTicks start,
    base::TimeDelta interval) {
  if (interval <= base::TimeDelta()) {
    return "Deterministic audio capture interval must be positive";
  }
  if (!origin_.has_value()) {
    origin_ = start;
  }

  int64_t start_frame = FrameAt(start);
  int64_t end_frame = FrameAt(start + interval);
  if (end_frame <= start_frame) {
    return "Deterministic audio capture interval contains no samples";
  }

  if (!recording_started_) {
    std::optional<std::string> error = EnsureGeneratedThrough(start_frame);
    if (error.has_value()) {
      return error;
    }
    if (start_frame > consumed_frames_) {
      ConsumeFrames(start_frame - consumed_frames_, nullptr);
    }
    recording_started_ = true;
    capture_end_frame_ = start_frame;
  } else {
    const int64_t discontinuity = start_frame - capture_end_frame_;
    if (discontinuity < -1 || discontinuity > 1) {
      return "Deterministic audio capture timestamps are not contiguous: " +
             std::to_string(discontinuity) + " sample frames";
    }
    start_frame = capture_end_frame_;
  }

  end_frame =
      start_frame + AudioTimestampHelper::TimeToFrames(interval, sample_rate_);
  std::optional<std::string> error = EnsureGeneratedThrough(end_frame);
  if (error.has_value()) {
    return error;
  }

  std::vector<int16_t> captured_samples;
  ConsumeFrames(end_frame - consumed_frames_, &captured_samples);
  error = WriteCapturedFrames(captured_samples);
  if (error.has_value()) {
    return error;
  }
  capture_end_frame_ = end_frame;
  return std::nullopt;
}

std::optional<std::string> DeterministicAudioDump::EnsureGeneratedThrough(
    int64_t target_frame) {
  if (target_frame < 0) {
    return "Deterministic audio target precedes its timeline origin";
  }
  while (generated_frames_ < target_frame) {
    bool has_stream = false;
    {
      base::AutoLock lock(stream_lock_);
      if (stream_error_.has_value()) {
        return stream_error_;
      }
      has_stream = pump_ != nullptr;
    }
    if (!has_stream) {
      AppendSilence(target_frame - generated_frames_);
      break;
    }
    std::optional<std::string> error = PumpOneBuffer();
    if (error.has_value()) {
      return error;
    }
  }
  return std::nullopt;
}

std::optional<std::string> DeterministicAudioDump::PumpOneBuffer() {
  std::vector<int16_t> samples;
  base::RunLoop run_loop(base::RunLoop::Type::kNestableTasksAllowed);
  scoped_refptr<DeterministicAudioPump> pump;
  {
    base::AutoLock lock(stream_lock_);
    if (stream_error_.has_value()) {
      return stream_error_;
    }
    if (!pump_) {
      return "Deterministic audio output stream disappeared while pumping";
    }
    pump = pump_;
  }
  if (!pump->PostPump(&samples, run_loop.QuitClosure())) {
    return "Failed to post deterministic audio pump task";
  }
  run_loop.Run();

  if (samples.empty() || samples.size() % channels_ != 0) {
    return "Deterministic audio pump returned an invalid buffer";
  }
  const int64_t frames = samples.size() / channels_;
  pending_samples_.insert(pending_samples_.end(), samples.begin(),
                          samples.end());
  generated_frames_ += frames;
  return std::nullopt;
}

void DeterministicAudioDump::AppendSilence(int64_t frames) {
  CHECK_GE(frames, 0);
  pending_samples_.insert(pending_samples_.end(),
                          static_cast<size_t>(frames * channels_), 0);
  generated_frames_ += frames;
}

void DeterministicAudioDump::ConsumeFrames(int64_t frames,
                                           std::vector<int16_t>* output) {
  CHECK_GE(frames, 0);
  const int64_t sample_count = frames * channels_;
  CHECK_LE(sample_count, static_cast<int64_t>(pending_samples_.size()));
  if (output) {
    output->reserve(static_cast<size_t>(sample_count));
  }
  for (int64_t i = 0; i < sample_count; ++i) {
    if (output) {
      output->push_back(pending_samples_.front());
    }
    pending_samples_.pop_front();
  }
  consumed_frames_ += frames;
}

std::optional<std::string> DeterministicAudioDump::OpenOutputIfNeeded() {
  if (output_fd_ >= 0) {
    return std::nullopt;
  }
  const std::string path =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kHeadlessRawAudioDumpSwitch);
  if (path.empty()) {
    return "Deterministic audio dump path is empty";
  }
  output_fd_ =
      HANDLE_EINTR(open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666));
  if (output_fd_ < 0) {
    return "Failed to open deterministic audio WAV output";
  }
  return RewriteWavHeader();
}

std::optional<std::string> DeterministicAudioDump::WriteCapturedFrames(
    const std::vector<int16_t>& samples) {
  std::optional<std::string> error = OpenOutputIfNeeded();
  if (error.has_value()) {
    return error;
  }
  const base::span<const uint8_t> bytes = base::as_bytes(base::span(samples));
  if (bytes.size() >
      std::numeric_limits<uint32_t>::max() - output_data_bytes_) {
    return "Deterministic audio WAV output exceeds the RIFF size limit";
  }
  if (!WriteAllAt(output_fd_, bytes, kWavHeaderSize + output_data_bytes_)) {
    return "Failed to write deterministic audio samples";
  }
  output_data_bytes_ += static_cast<uint32_t>(bytes.size());
  return RewriteWavHeader();
}

std::optional<std::string> DeterministicAudioDump::RewriteWavHeader() {
  std::array<uint8_t, kWavHeaderSize> header{};
  base::span(header).subspan<0, 4>().copy_from(kRiff);
  WriteLittleEndian32(header, 4, 36 + output_data_bytes_);
  base::span(header).subspan<8, 8>().copy_from(kWaveFormat);
  WriteLittleEndian32(header, 16, 16);
  WriteLittleEndian16(header, 20, 1);
  WriteLittleEndian16(header, 22, channels_);
  WriteLittleEndian32(header, 24, sample_rate_);
  WriteLittleEndian32(header, 28, sample_rate_ * channels_ * sizeof(int16_t));
  WriteLittleEndian16(header, 32, channels_ * sizeof(int16_t));
  WriteLittleEndian16(header, 34, 16);
  base::span(header).subspan<36, 4>().copy_from(kData);
  WriteLittleEndian32(header, 40, output_data_bytes_);
  if (!WriteAllAt(output_fd_, header, 0)) {
    return "Failed to update deterministic audio WAV header";
  }
  return std::nullopt;
}

}  // namespace media
