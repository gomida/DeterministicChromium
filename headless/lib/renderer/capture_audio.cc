// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/renderer/capture_audio.h"

#include <algorithm>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "media/base/audio_sample_types.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "headless/public/switches.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_timestamp_helper.h"
#include "media/base/channel_layout.h"
#include "media/base/channel_mixer.h"
#include "media/base/multi_channel_resampler.h"

namespace headless {

// ---- CaptureAudioFormat -------------------------------------------------------

// static
CaptureAudioFormat CaptureAudioFormat::FromCommandLine() {
  CaptureAudioFormat format;
  const std::string value = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
      switches::kDeterministicCaptureAudio);
  const std::vector<std::string> parts =
      base::SplitString(value, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  int rate = 0;
  int channels = 0;
  int frames = 0;
  if (parts.size() >= 1 && base::StringToInt(parts[0], &rate) && rate >= 8000 && rate <= 192000) {
    format.rate = rate;
  }
  if (parts.size() >= 2 && base::StringToInt(parts[1], &channels) && channels >= 1 && channels <= 8) {
    format.channels = channels;
  }
  if (parts.size() >= 3 && base::StringToInt(parts[2], &frames) && frames >= 64 && frames <= 8192) {
    format.frames_per_buffer = frames;
  }
  return format;
}

media::AudioParameters CaptureAudioFormat::Params() const {
  return media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                media::ChannelLayoutConfig::Guess(channels), rate,
                                frames_per_buffer);
}

// ---- CaptureAudioEngine -------------------------------------------------------

// static
CaptureAudioEngine* CaptureAudioEngine::Get() {
  static CaptureAudioEngine* const engine = new CaptureAudioEngine();  // leaked on purpose
  return engine;
}

CaptureAudioEngine::CaptureAudioEngine() : format_(CaptureAudioFormat::FromCommandLine()) {
  base::Thread::Options options;
  options.thread_type = base::ThreadType::kRealtimeAudio;
  CHECK(thread_.StartWithOptions(std::move(options)));
}

scoped_refptr<base::SingleThreadTaskRunner> CaptureAudioEngine::task_runner() const {
  return thread_.task_runner();
}

void CaptureAudioEngine::AddSink(CaptureAudioRendererSink* sink) {
  base::AutoLock lock(lock_);
  for (const auto& existing : sinks_) {
    if (existing.get() == sink) {
      return;
    }
  }
  sinks_.push_back(base::WrapRefCounted(sink));
}

void CaptureAudioEngine::RemoveSink(CaptureAudioRendererSink* sink) {
  scoped_refptr<CaptureAudioRendererSink> dropped;  // released outside the lock
  base::AutoLock lock(lock_);
  for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
    if (it->get() == sink) {
      dropped = std::move(*it);
      sinks_.erase(it);
      break;
    }
  }
}

void CaptureAudioEngine::NoteUnsupported(const std::string& api) {
  base::AutoLock lock(lock_);
  unsupported_.push_back(api);
}

std::vector<std::string> CaptureAudioEngine::TakeUnsupported() {
  base::AutoLock lock(lock_);
  return std::move(unsupported_);
}

void CaptureAudioEngine::RenderThrough(int64_t frames_through, RenderCallback callback) {
  thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&CaptureAudioEngine::RenderOnAudioThread, base::Unretained(this),
                                frames_through, std::move(callback),
                                base::SequencedTaskRunner::GetCurrentDefault()));
}

void CaptureAudioEngine::RenderOnAudioThread(int64_t frames_through,
                                             RenderCallback callback,
                                             scoped_refptr<base::SequencedTaskRunner> reply_runner) {
  const int64_t need64 = frames_through - rendered_through_;
  const int frames = static_cast<int>(std::clamp<int64_t>(need64, 0, 1 << 24));
  std::vector<scoped_refptr<CaptureAudioRendererSink>> sinks;
  {
    base::AutoLock lock(lock_);
    sinks = sinks_;
  }
  std::vector<float> pcm(static_cast<size_t>(frames) * format_.channels, 0.0f);
  if (frames > 0) {
    std::unique_ptr<media::AudioBus> mix = media::AudioBus::Create(format_.channels, frames);
    mix->Zero();
    // Virtual time is paused at T_n while this runs; the sinks report it as
    // the delay timestamp so AudioClock reads capture time.
    const base::TimeTicks now = base::TimeTicks::Now();
    for (const auto& sink : sinks) {
      sink->MixInto(mix.get(), frames, now);
    }
    mix->ToInterleaved<media::Float32SampleTypeTraitsNoClip>(base::span<float>(pcm));
    rendered_through_ = frames_through;
  }
  reply_runner->PostTask(FROM_HERE, base::BindOnce(std::move(callback), std::move(pcm),
                                                   static_cast<uint32_t>(frames),
                                                   static_cast<int>(sinks.size())));
}

// ---- CaptureAudioRendererSink ---------------------------------------------------

CaptureAudioRendererSink::CaptureAudioRendererSink(blink::WebAudioDeviceSourceType source_type)
    : source_type_(source_type), engine_(CaptureAudioEngine::Get()) {}

CaptureAudioRendererSink::~CaptureAudioRendererSink() = default;

void CaptureAudioRendererSink::Initialize(const media::AudioParameters& params,
                                          RenderCallback* callback) {
  {
    base::AutoLock lock(lock_);
    InitializeLocked(params, callback);
  }
  engine_->AddSink(this);  // engine lock is never taken under the sink lock
}

void CaptureAudioRendererSink::InitializeLocked(const media::AudioParameters& params,
                                                RenderCallback* callback) {
  lock_.AssertAcquired();
  params_ = params;
  callback_ = callback;
  const CaptureAudioFormat& device = engine_->format();
  render_bus_ = media::AudioBus::Create(params.channels(), params.frames_per_buffer());
  if (params.sample_rate() != device.rate) {
    // Sources that insist on their own rate (an AudioContext created with a
    // sampleRate) are resampled to the device rate in whole source buffers.
    const double ratio = static_cast<double>(params.sample_rate()) / device.rate;
    const int out_frames = static_cast<int>(params.frames_per_buffer() / ratio) + 1;
    resampled_bus_ = media::AudioBus::Create(params.channels(), out_frames);
    resampler_ = std::make_unique<media::MultiChannelResampler>(
        params.channels(), ratio, params.frames_per_buffer(),
        base::BindRepeating(
            [](CaptureAudioRendererSink* self, int frame_delay, media::AudioBus* dest) {
              self->lock_.AssertAcquired();
              const int got = self->callback_->Render(base::TimeDelta(), base::TimeTicks::Now(),
                                                      media::AudioGlitchInfo(), dest);
              if (got < dest->frames()) {
                dest->ZeroFramesPartial(got, dest->frames() - got);
              }
            },
            base::Unretained(this)));
  }
  if (params.channels() != device.channels) {
    mixer_ = std::make_unique<media::ChannelMixer>(
        media::ChannelLayoutConfig::Guess(params.channels()),
        media::ChannelLayoutConfig::Guess(device.channels));
  }
  const int mixed_frames = resampled_bus_ ? resampled_bus_->frames() : params.frames_per_buffer();
  mixed_bus_ = media::AudioBus::Create(device.channels, mixed_frames);
  fifo_.assign(device.channels, {});
  fifo_read_ = 0;
  initialized_ = true;
}

void CaptureAudioRendererSink::Start() {
  base::AutoLock lock(lock_);
  started_ = true;
}

void CaptureAudioRendererSink::Stop() {
  {
    base::AutoLock lock(lock_);
    started_ = false;
    playing_ = false;
    callback_ = nullptr;
    initialized_ = false;
    for (auto& channel : fifo_) {
      channel.clear();
    }
    fifo_read_ = 0;
  }
  engine_->RemoveSink(this);
}

void CaptureAudioRendererSink::Pause(PauseReason) {
  base::AutoLock lock(lock_);
  playing_ = false;
}

void CaptureAudioRendererSink::Play() {
  base::AutoLock lock(lock_);
  playing_ = true;
}

void CaptureAudioRendererSink::Flush() {
  base::AutoLock lock(lock_);
  for (auto& channel : fifo_) {
    channel.clear();
  }
  fifo_read_ = 0;
}

bool CaptureAudioRendererSink::SetVolume(double volume) {
  // Lock-free: the media thread may set it while a mix is rendering.
  volume_.store(volume, std::memory_order_relaxed);
  return true;
}

media::OutputDeviceInfo CaptureAudioRendererSink::GetOutputDeviceInfo() {
  return media::OutputDeviceInfo(std::string(), media::OUTPUT_DEVICE_STATUS_OK,
                                 engine_->format().Params());
}

void CaptureAudioRendererSink::GetOutputDeviceInfoAsync(OutputDeviceInfoCB info_cb) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(info_cb), GetOutputDeviceInfo()));
}

bool CaptureAudioRendererSink::IsOptimizedForHardwareParameters() {
  return true;
}

bool CaptureAudioRendererSink::CurrentThreadIsRenderingThread() {
  return engine_->task_runner()->BelongsToCurrentThread();
}

void CaptureAudioRendererSink::SwitchOutputDevice(const std::string& device_id,
                                                  media::OutputDeviceStatusCB callback) {
  if (!device_id.empty()) {
    engine_->NoteUnsupported("setSinkId");
  }
  std::move(callback).Run(media::OUTPUT_DEVICE_STATUS_OK);
}

void CaptureAudioRendererSink::RenderOneBuffer_Locked(base::TimeTicks now) {
  lock_.AssertAcquired();
  const size_t fifo_frames = fifo_.empty() ? 0 : fifo_[0].size() - fifo_read_;
  const CaptureAudioFormat& device = engine_->format();
  media::AudioBus* source = render_bus_.get();
  if (resampler_) {
    resampler_->Resample(resampled_bus_->frames(), resampled_bus_.get());
    source = resampled_bus_.get();
  } else {
    // The frames still in the FIFO are what the source would hear as output
    // latency; the timestamp is the capture time.
    const base::TimeDelta delay =
        media::AudioTimestampHelper::FramesToTime(static_cast<int64_t>(fifo_frames), device.rate);
    const int got = callback_->Render(delay, now, media::AudioGlitchInfo(), render_bus_.get());
    if (got < render_bus_->frames()) {
      render_bus_->ZeroFramesPartial(got, render_bus_->frames() - got);
    }
  }
  const double volume = volume_.load(std::memory_order_relaxed);
  if (volume != 1.0) {
    source->Scale(static_cast<float>(volume));
  }
  media::AudioBus* out = source;
  if (mixer_) {
    mixer_->Transform(source, mixed_bus_.get());
    out = mixed_bus_.get();
  }
  for (int c = 0; c < out->channels(); ++c) {
    const auto& span = out->AllChannels()[c];
    fifo_[c].insert(fifo_[c].end(), span.begin(), span.end());
  }
  frames_rendered_ += out->frames();
}

void CaptureAudioRendererSink::MixInto(media::AudioBus* mix, int frames, base::TimeTicks now) {
  base::AutoLock lock(lock_);
  if (!initialized_ || !playing_ || !callback_) {
    return;
  }
  if (source_type_ == blink::WebAudioDeviceSourceType::kMediaElement && !resampler_) {
    // A media element's renderer fills any request size, so the interval is
    // one exact request with no output delay: AudioClock then reads exactly
    // S(n+1), the capture time, and video frames are selected against it.
    if (!exact_bus_ || exact_bus_->frames() != frames) {
      exact_bus_ = media::AudioBus::Create(params_.channels(), frames);
    }
    const int got = callback_->Render(base::TimeDelta(), now, media::AudioGlitchInfo(), exact_bus_.get());
    if (got < frames) {
      exact_bus_->ZeroFramesPartial(got, frames - got);
    }
    const double volume = volume_.load(std::memory_order_relaxed);
    if (volume != 1.0) {
      exact_bus_->Scale(static_cast<float>(volume));
    }
    media::AudioBus* out = exact_bus_.get();
    if (mixer_) {
      if (!exact_mixed_bus_ || exact_mixed_bus_->frames() != frames) {
        exact_mixed_bus_ = media::AudioBus::Create(mix->channels(), frames);
      }
      mixer_->Transform(out, exact_mixed_bus_.get());
      out = exact_mixed_bus_.get();
    }
    for (int c = 0; c < mix->channels(); ++c) {
      const auto& dest = mix->AllChannels()[c];
      const auto& src = out->AllChannels()[c];
      for (int i = 0; i < frames; ++i) {
        dest[i] += src[i];
      }
    }
    frames_rendered_ += frames;
    return;
  }
  // Other sources (AudioContext destinations) render whole buffers of their
  // own size; the excess waits in the FIFO and is reported as output delay.
  while (static_cast<int>(fifo_[0].size() - fifo_read_) < frames) {
    RenderOneBuffer_Locked(now);
  }
  for (int c = 0; c < mix->channels(); ++c) {
    const auto& dest = mix->AllChannels()[c];
    const std::vector<float>& src = fifo_[c];
    for (int i = 0; i < frames; ++i) {
      dest[i] += src[fifo_read_ + i];
    }
  }
  fifo_read_ += frames;
  // Keep the FIFO small: drop consumed frames once they dominate.
  if (fifo_read_ > 4096 && fifo_read_ * 2 > fifo_[0].size()) {
    for (auto& channel : fifo_) {
      channel.erase(channel.begin(), channel.begin() + fifo_read_);
    }
    fifo_read_ = 0;
  }
}

// ---- CaptureAudioDeviceFactory --------------------------------------------------

CaptureAudioDeviceFactory::CaptureAudioDeviceFactory() = default;
CaptureAudioDeviceFactory::~CaptureAudioDeviceFactory() = default;

scoped_refptr<media::AudioRendererSink> CaptureAudioDeviceFactory::NewAudioRendererSink(
    blink::WebAudioDeviceSourceType source_type,
    const blink::LocalFrameToken& frame_token,
    const media::AudioSinkParameters& params) {
  if (source_type == blink::WebAudioDeviceSourceType::kWebRtc ||
      source_type == blink::WebAudioDeviceSourceType::kNonRtcAudioTrack) {
    CaptureAudioEngine::Get()->NoteUnsupported(
        source_type == blink::WebAudioDeviceSourceType::kWebRtc ? "WebRTC audio" : "MediaStreamTrack audio");
  }
  return base::MakeRefCounted<CaptureAudioRendererSink>(source_type);
}

scoped_refptr<media::SwitchableAudioRendererSink> CaptureAudioDeviceFactory::NewMixableSink(
    blink::WebAudioDeviceSourceType source_type,
    const blink::LocalFrameToken& frame_token,
    const blink::FrameToken& main_frame_token,
    const media::AudioSinkParameters& params) {
  return base::MakeRefCounted<CaptureAudioRendererSink>(source_type);
}

media::OutputDeviceInfo CaptureAudioDeviceFactory::GetOutputDeviceInfo(
    const blink::LocalFrameToken& frame_token,
    const std::string& device_id) {
  return media::OutputDeviceInfo(device_id, media::OUTPUT_DEVICE_STATUS_OK,
                                 CaptureAudioEngine::Get()->format().Params());
}

}  // namespace headless
