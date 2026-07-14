// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_DETERMINISTIC_AUDIO_DUMP_H_
#define MEDIA_AUDIO_DETERMINISTIC_AUDIO_DUMP_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "media/base/media_export.h"

namespace base {
class SequencedTaskRunner;
template <typename T>
class NoDestructor;
}  // namespace base

namespace media {

class AudioParameters;
class FakeAudioOutputStream;

// Ref-counted bridge which keeps posted worker tasks independent from output
// stream registration changes on the audio-manager sequence.
class MEDIA_EXPORT DeterministicAudioPump
    : public base::RefCountedThreadSafe<DeterministicAudioPump> {
 public:
  explicit DeterministicAudioPump(
      scoped_refptr<base::SequencedTaskRunner> worker_task_runner);

  void Activate(FakeAudioOutputStream* stream);
  void Deactivate(FakeAudioOutputStream* stream);
  bool PostPump(std::vector<int16_t>* samples, base::RepeatingClosure done);

 private:
  friend class base::RefCountedThreadSafe<DeterministicAudioPump>;

  ~DeterministicAudioPump();
  void PumpOnWorker(std::vector<int16_t>* samples, base::RepeatingClosure done);

  const scoped_refptr<base::SequencedTaskRunner> worker_task_runner_;
  base::Lock stream_lock_;
  raw_ptr<FakeAudioOutputStream> stream_ GUARDED_BY(stream_lock_) = nullptr;
};

// Pulls the final fake audio output from HeadlessExperimental.beginFrame and
// writes the captured intervals to a PCM WAV file. The switch is intentionally
// headless-specific, but this class lives beside FakeAudioOutputStream so the
// browser process and in-process audio service share one controller.
class MEDIA_EXPORT DeterministicAudioDump {
 public:
  static bool IsEnabled();
  static DeterministicAudioDump* GetIfEnabled();

  DeterministicAudioDump(const DeterministicAudioDump&) = delete;
  DeterministicAudioDump& operator=(const DeterministicAudioDump&) = delete;

  void RegisterStream(scoped_refptr<DeterministicAudioPump> pump,
                      const AudioParameters& params);
  void UnregisterStream(DeterministicAudioPump* pump);

  // Advances native audio playback to |target| without recording. Generated
  // samples are discarded at their exact timeline boundary, retaining any
  // buffer overshoot for the next call.
  std::optional<std::string> AdvanceTo(base::TimeTicks target);

  // Records [start, start + interval). Calls must be contiguous after the
  // first captured interval. One call corresponds to one captured video frame.
  std::optional<std::string> CaptureInterval(base::TimeTicks start,
                                             base::TimeDelta interval);

 private:
  friend class base::NoDestructor<DeterministicAudioDump>;

  DeterministicAudioDump();
  ~DeterministicAudioDump();

  int64_t FrameAt(base::TimeTicks target) const;
  std::optional<std::string> EnsureGeneratedThrough(int64_t target_frame);
  std::optional<std::string> PumpOneBuffer();
  void AppendSilence(int64_t frames);
  void ConsumeFrames(int64_t frames, std::vector<int16_t>* output);
  std::optional<std::string> OpenOutputIfNeeded();
  std::optional<std::string> WriteCapturedFrames(
      const std::vector<int16_t>& samples);
  std::optional<std::string> RewriteWavHeader();

  base::Lock stream_lock_;
  scoped_refptr<DeterministicAudioPump> pump_ GUARDED_BY(stream_lock_);
  std::optional<std::string> stream_error_ GUARDED_BY(stream_lock_);
  int sample_rate_ = 0;
  int channels_ = 0;

  std::optional<base::TimeTicks> origin_;
  int64_t generated_frames_ = 0;
  int64_t consumed_frames_ = 0;
  std::deque<int16_t> pending_samples_;
  bool recording_started_ = false;
  int64_t capture_end_frame_ = 0;

  int output_fd_ = -1;
  uint32_t output_data_bytes_ = 0;
};

}  // namespace media

#endif  // MEDIA_AUDIO_DETERMINISTIC_AUDIO_DUMP_H_
