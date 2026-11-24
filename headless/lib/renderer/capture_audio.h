// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_RENDERER_CAPTURE_AUDIO_H_
#define HEADLESS_LIB_RENDERER_CAPTURE_AUDIO_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/output_device_info.h"
#include "third_party/blink/public/platform/audio/web_audio_device_source_type.h"
#include "third_party/blink/public/web/modules/media/audio/audio_device_factory.h"

namespace media {
class AudioBus;
class ChannelMixer;
class MultiChannelResampler;
}  // namespace media

namespace headless {

class CaptureAudioRendererSink;

// Device format of every capture sink, from --deterministic-capture-audio.
struct CaptureAudioFormat {
  int rate = 48000;
  int channels = 2;
  int frames_per_buffer = 128;  // one Web Audio render quantum

  static CaptureAudioFormat FromCommandLine();
  media::AudioParameters Params() const;
};

// Renderer-side audio for deterministic capture (DESIGN.md 4.6): one capture
// audio thread, the sinks of this process in creation order, and the mix. The
// browser's advance renders exactly the interval's sample frames through
// CaptureAgent::Prepare; nothing pulls audio in real time.
class CaptureAudioEngine {
 public:
  // Created on first use on the renderer main thread; lives for the process.
  static CaptureAudioEngine* Get();

  const CaptureAudioFormat& format() const { return format_; }
  scoped_refptr<base::SingleThreadTaskRunner> task_runner() const;

  // Sink registration (any thread).
  void AddSink(CaptureAudioRendererSink* sink);
  void RemoveSink(CaptureAudioRendererSink* sink);
  void NoteUnsupported(const std::string& api);
  std::vector<std::string> TakeUnsupported();

  // Renders every sink up to `frames_through` device frames since the start
  // and mixes the interval since the previous call. Runs on the audio thread;
  // `callback` runs on the calling sequence with interleaved float PCM.
  using RenderCallback =
      base::OnceCallback<void(std::vector<float> pcm, uint32_t frames, int sinks)>;
  void RenderThrough(int64_t frames_through, RenderCallback callback);

 private:
  CaptureAudioEngine();
  ~CaptureAudioEngine() = delete;

  void RenderOnAudioThread(int64_t frames_through,
                           RenderCallback callback,
                           scoped_refptr<base::SequencedTaskRunner> reply_runner);

  CaptureAudioFormat format_;
  base::Thread thread_{"DcapAudio"};
  base::Lock lock_;
  // Owned references: a sink stays mixable (as silence once stopped) until
  // Stop() removes it, so the audio thread never dereferences a freed sink.
  std::vector<scoped_refptr<CaptureAudioRendererSink>> sinks_;  // creation order
  std::vector<std::string> unsupported_;
  int64_t rendered_through_ = 0;  // audio thread only
};

// A sink that renders its source only when the engine asks, in whole source
// buffers, keeping the excess in a FIFO so the source's clock reads exactly
// the capture time (DESIGN.md 4.6).
class CaptureAudioRendererSink : public media::SwitchableAudioRendererSink {
 public:
  explicit CaptureAudioRendererSink(blink::WebAudioDeviceSourceType source_type);

  // media::AudioRendererSink:
  void Initialize(const media::AudioParameters& params, RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause(PauseReason reason) override;
  void Play() override;
  void Flush() override;
  bool SetVolume(double volume) override;
  media::OutputDeviceInfo GetOutputDeviceInfo() override;
  void GetOutputDeviceInfoAsync(OutputDeviceInfoCB info_cb) override;
  bool IsOptimizedForHardwareParameters() override;
  bool CurrentThreadIsRenderingThread() override;
  // media::SwitchableAudioRendererSink:
  void SwitchOutputDevice(const std::string& device_id, media::OutputDeviceStatusCB callback) override;

  // Audio thread: adds this sink's next `frames` device frames into `mix`
  // (silence when not playing). `now` is the capture virtual time.
  void MixInto(media::AudioBus* mix, int frames, base::TimeTicks now);

 private:
  ~CaptureAudioRendererSink() override;
  void InitializeLocked(const media::AudioParameters& params, RenderCallback* callback);
  void RenderOneBuffer_Locked(base::TimeTicks now);

  const blink::WebAudioDeviceSourceType source_type_;
  raw_ptr<CaptureAudioEngine> engine_;
  base::Lock lock_;
  media::AudioParameters params_;
  raw_ptr<RenderCallback> callback_ = nullptr;
  bool initialized_ = false;
  bool started_ = false;
  bool playing_ = false;
  std::atomic<double> volume_{1.0};  // set from the media thread while a mix may be running
  std::unique_ptr<media::AudioBus> render_bus_;     // source format
  std::unique_ptr<media::AudioBus> resampled_bus_;  // device rate, source channels
  std::unique_ptr<media::MultiChannelResampler> resampler_;
  std::unique_ptr<media::ChannelMixer> mixer_;
  std::unique_ptr<media::AudioBus> mixed_bus_;      // device channels
  std::unique_ptr<media::AudioBus> exact_bus_;        // media elements: one request per interval
  std::unique_ptr<media::AudioBus> exact_mixed_bus_;
  // FIFO of device-format frames not yet consumed, one vector per channel.
  std::vector<std::vector<float>> fifo_;
  size_t fifo_read_ = 0;
  int64_t frames_rendered_ = 0;
};

// Replaces Blink's AudioDeviceFactory in --deterministic-capture renderers so
// every media element and AudioContext gets a CaptureAudioRendererSink.
class CaptureAudioDeviceFactory : public blink::AudioDeviceFactory {
 public:
  CaptureAudioDeviceFactory();
  ~CaptureAudioDeviceFactory() override;

  scoped_refptr<media::AudioRendererSink> NewAudioRendererSink(
      blink::WebAudioDeviceSourceType source_type,
      const blink::LocalFrameToken& frame_token,
      const media::AudioSinkParameters& params) override;
  scoped_refptr<media::SwitchableAudioRendererSink> NewMixableSink(
      blink::WebAudioDeviceSourceType source_type,
      const blink::LocalFrameToken& frame_token,
      const blink::FrameToken& main_frame_token,
      const media::AudioSinkParameters& params) override;
  media::OutputDeviceInfo GetOutputDeviceInfo(const blink::LocalFrameToken& frame_token,
                                              const std::string& device_id) override;
};

}  // namespace headless

#endif  // HEADLESS_LIB_RENDERER_CAPTURE_AUDIO_H_
