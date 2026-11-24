// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/capture/capture_session.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "headless/lib/browser/capture/canonical_hash.h"
#include "headless/lib/browser/capture/shm_ring.h"
#include "headless/lib/browser/headless_web_contents_impl.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace headless::capture {

namespace {

constexpr char kProtocolVersion[] = "2.1";

double Percentile(std::vector<double> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const size_t index = std::min(
      values.size() - 1, static_cast<size_t>(values.size() * p / 100.0));
  return values[index];
}

base::DictValue Histogram(const std::vector<double>& values) {
  base::DictValue h;
  h.Set("count", static_cast<int>(values.size()));
  h.Set("p50", Percentile(values, 50));
  h.Set("p95", Percentile(values, 95));
  h.Set("p99", Percentile(values, 99));
  h.Set("max", values.empty() ? 0.0 : *std::max_element(values.begin(), values.end()));
  return h;
}

base::DictValue SinkErrorDict(const SinkError& error) {
  base::DictValue d;
  d.Set("frameIndex", static_cast<double>(error.frame_index));
  d.Set("errno", error.error_number);
  d.Set("peerClosed", error.peer_closed);
  d.Set("bytesIntoFrame", static_cast<double>(error.bytes_into_frame));
  d.Set("framesFullyWritten", static_cast<double>(error.frames_fully_written));
  d.Set("message", error.message);
  return d;
}

}  // namespace

std::string CaptureSession::Error::Render() const {
  return kind + ": " + message;
}

// static
const char* CaptureSession::StateName(State state) {
  switch (state) {
    case State::kUnconfigured:
      return "unconfigured";
    case State::kConfigured:
      return "configured";
    case State::kCapturing:
      return "capturing";
    case State::kFinished:
      return "finished";
    case State::kFailed:
      return "failed";
    case State::kAborted:
      return "aborted";
  }
  return "unknown";
}

CaptureSession::CaptureSession(content::WebContents* web_contents)
    : content::WebContentsUserData<CaptureSession>(*web_contents),
      web_contents_(web_contents),
      worker_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

CaptureSession::~CaptureSession() = default;

int64_t CaptureSession::FrameOffsetMicros(int64_t frame_index) const {
  // T_n = round_half_down(n * den * 1e6 / num), computed from the index.
  const int64_t numerator = frame_index * frame_rate_.den * 1000000;
  const int64_t quotient = numerator / frame_rate_.num;
  const int64_t remainder = numerator % frame_rate_.num;
  return quotient + (2 * remainder > frame_rate_.num ? 1 : 0);
}

base::TimeTicks CaptureSession::FrameTicks(int64_t frame_index) const {
  // Every frame, including frame 0, is preceded by a grant of one frame
  // interval: the document cannot load or paint while virtual time is paused,
  // so T_n = base + offset(n + 1) and the grant for frame n is
  // offset(n + 1) - offset(n).
  return base::TimeTicks() + base::Milliseconds(virtual_time_ticks_base_ms_) +
         base::Microseconds(FrameOffsetMicros(frame_index + 1));
}

int64_t CaptureSession::AudioSamplesThrough(int64_t frame_index) const {
  // Mirrors dcap's FrameRate::samples_through; the products stay below 2^63
  // for any practical frame count (1e6 frames * 192000 * 1001 < 2e14).
  return frame_index * audio_rate_ * frame_rate_.den / frame_rate_.num;
}

void CaptureSession::Fail(Error error) {
  if (!last_error_) {
    last_error_ = error;
  }
  state_ = State::kFailed;
}

std::optional<CaptureSession::Error> CaptureSession::CheckState(
    const char* command) const {
  if (state_ == State::kFailed && last_error_) {
    return last_error_;
  }
  if (state_ != State::kConfigured && state_ != State::kCapturing) {
    return Error{"NotConfigured",
                 std::string(command) + " in state " + StateName(state_)};
  }
  return std::nullopt;
}

// ---- configure --------------------------------------------------------------

void CaptureSession::Configure(base::DictValue config,
                               ResultCallback callback) {
  if (state_ == State::kCapturing) {
    std::move(callback).Run(Error{"AlreadyStarted", "capture already started"},
                            base::DictValue());
    return;
  }
  if (state_ != State::kUnconfigured && state_ != State::kConfigured) {
    std::move(callback).Run(Error{"NotConfigured", std::string("configure in state ") + StateName(state_)},
                            base::DictValue());
    return;
  }
  // Feature negotiation: everything this build implements.
  static constexpr const char* kImplemented[] = {"pipelinedAdvance", "advanceRange", "shmSink", "canonicalBgra", "markers", "prepare", "audioRender"};
  if (const base::ListValue* required = config.FindList("requiredFeatures")) {
    for (const base::Value& feature : *required) {
      const std::string* name = feature.GetIfString();
      bool found = false;
      for (const char* implemented : kImplemented) {
        if (name && *name == implemented) {
          found = true;
        }
      }
      if (!found) {
        std::move(callback).Run(
            Error{"UnknownFeature", "required feature not available: " + (name ? *name : "?")},
            base::DictValue());
        return;
      }
    }
  }
  if (const base::DictValue* rate = config.FindDict("frameRate")) {
    frame_rate_.num = rate->FindInt("num").value_or(60);
    frame_rate_.den = rate->FindInt("den").value_or(1);
    if (frame_rate_.num <= 0 || frame_rate_.den <= 0) {
      std::move(callback).Run(Error{"InvalidViewport", "frameRate must be positive"}, base::DictValue());
      return;
    }
  }
  RawFrameSink::Config sink_config;
  effective_config_ = config.Clone();  // the sections below add their resolved defaults
  if (const base::DictValue* markers = config.FindDict("markers")) {
    if (const std::string* prefix = markers->FindString("prefix")) {
      marker_prefix_ = *prefix;
    }
    collect_marks_ = true;
    if (const base::ListValue* sources = markers->FindList("sources")) {
      collect_marks_ = false;
      for (const base::Value& source : *sources) {
        if (source.is_string() && source.GetString() == "performanceMark") {
          collect_marks_ = true;
        }
      }
    }
  }
  if (const base::DictValue* audio = config.FindDict("audio")) {
    if (const base::DictValue* device = audio->FindDict("deviceFormat")) {
      audio_rate_ = device->FindInt("rate").value_or(audio_rate_);
      audio_channels_ = device->FindInt("channels").value_or(audio_channels_);
    }
    if (const std::string* fmt = audio->FindString("sampleFormat")) {
      if (*fmt != "s16" && *fmt != "f32") {
        std::move(callback).Run(Error{"AudioFormatUnsupported", "audio.sampleFormat must be s16 or f32"}, base::DictValue());
        return;
      }
      audio_s16_ = *fmt == "s16";
    }
    if (audio_rate_ < 8000 || audio_rate_ > 192000 || audio_channels_ < 1 || audio_channels_ > 8) {
      std::move(callback).Run(Error{"AudioFormatUnsupported", "audio.deviceFormat out of range"}, base::DictValue());
      return;
    }
  }
  base::DictValue effective_audio;
  base::DictValue effective_device;
  effective_device.Set("rate", audio_rate_);
  effective_device.Set("channels", audio_channels_);
  effective_audio.Set("deviceFormat", std::move(effective_device));
  effective_audio.Set("sampleFormat", audio_s16_ ? "s16" : "f32");
  effective_config_.Set("audio", std::move(effective_audio));
  if (const base::DictValue* media = config.FindDict("media")) {
    if (std::optional<double> ms = media->FindDouble("waitTimeoutMs")) {
      media_wait_timeout_ms_ = *ms;
    }
    fail_on_media_wait_timeout_ = media->FindBool("failOnWaitTimeout").value_or(true);
    fail_on_unsupported_api_ = media->FindBool("failOnUnsupportedApi").value_or(true);
  }
  base::DictValue effective_markers;
  effective_markers.Set("prefix", marker_prefix_);
  base::ListValue effective_sources;
  if (collect_marks_) {
    effective_sources.Append("performanceMark");
  }
  effective_markers.Set("sources", std::move(effective_sources));
  effective_config_.Set("markers", std::move(effective_markers));
  base::DictValue effective_media;
  effective_media.Set("waitTimeoutMs", media_wait_timeout_ms_);
  effective_media.Set("failOnWaitTimeout", fail_on_media_wait_timeout_);
  effective_media.Set("failOnUnsupportedApi", fail_on_unsupported_api_);
  effective_config_.Set("media", std::move(effective_media));
  if (const base::DictValue* sink = config.FindDict("frameSink")) {
    const std::string* type = sink->FindString("type");
    if (type && *type == "shm") {
      sink_config.type = RawFrameSink::Type::kShm;
      sink_config.ring_fd = sink->FindInt("ringFd").value_or(-1);
      sink_config.ready_fd = sink->FindInt("readyFd").value_or(-1);
      sink_config.free_fd = sink->FindInt("freeFd").value_or(-1);
    } else if (type && *type == "file") {
      sink_config.type = RawFrameSink::Type::kFile;
    } else if (type && *type != "null") {
      std::move(callback).Run(Error{"InvalidParameters", "frameSink.type must be shm, file or null"},
                              base::DictValue());
      return;
    }
    if (const std::string* path = sink->FindString("path")) {
      sink_config.path = *path;
    }
    if (std::optional<int> expected = sink->FindInt("expectedFrames")) {
      sink_config.expected_frames = *expected;
      expected_frames_ = *expected;
    }
    max_queued_frames_ = sink->FindInt("maxQueuedFrames").value_or(8);
    sink_config.max_queued_frames = max_queued_frames_;
    stall_timeout_ = base::Milliseconds(sink->FindInt("stallTimeoutMs").value_or(60000));
    sink_config.stall_timeout = stall_timeout_;
  }
  sink_wants_frames_ = sink_config.type != RawFrameSink::Type::kNull;
  sink_config.audio_rate = static_cast<uint32_t>(audio_rate_);
  sink_config.audio_channels = static_cast<uint32_t>(audio_channels_);
  sink_config.audio_format = audio_s16_ ? ring::kAudioFormatS16 : ring::kAudioFormatF32;
  sink_ = std::make_unique<RawFrameSink>(sink_config);
  effective_config_.Set("protocolVersion", kProtocolVersion);
  effective_config_.Set("pixelFormat", kCanonicalPixelFormat);
  // Opening maps the ring or creates the file; keep it off the UI thread.
  worker_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce([](RawFrameSink* sink) { return sink->Open(); }, base::Unretained(sink_.get())),
      base::BindOnce(&CaptureSession::OnSinkOpened, weak_factory_.GetWeakPtr(),
                     std::move(config), std::move(callback)));
}

void CaptureSession::OnSinkOpened(base::DictValue config,
                                  ResultCallback callback,
                                  std::optional<SinkError> error) {
  if (error) {
    std::move(callback).Run(Error{"SinkNotConnected", error->message, SinkErrorDict(*error)},
                            base::DictValue());
    return;
  }
  double initial_virtual_time_s = 1700000000.0;
  if (const base::DictValue* vt = config.FindDict("virtualTime")) {
    if (std::optional<double> ms = vt->FindDouble("initialVirtualTime")) {
      initial_virtual_time_s = *ms / 1000.0;
    }
  }
  if (!virtual_time_) {
    virtual_time_ = std::make_unique<VirtualTimeOwner>(web_contents_);
  }
  virtual_time_->Enable(
      initial_virtual_time_s,
      base::BindOnce(&CaptureSession::OnVirtualTimeEnabled, weak_factory_.GetWeakPtr(),
                     effective_config_.Clone(), std::move(callback)));
}

void CaptureSession::OnVirtualTimeEnabled(base::DictValue effective,
                                          ResultCallback callback,
                                          std::optional<std::string> error,
                                          base::DictValue result) {
  if (error) {
    std::move(callback).Run(Error{"VirtualTimeOwned", *error}, base::DictValue());
    return;
  }
  virtual_time_ticks_base_ms_ = result.FindDouble("virtualTimeTicksBase").value_or(0.0);
  effective.Set("virtualTimeTicksBase", virtual_time_ticks_base_ms_);
  effective_config_ = effective.Clone();
  state_ = State::kConfigured;
  next_frame_ = 0;
  base::DictValue response;
  response.Set("effective", std::move(effective));
  response.Set("virtualTimeTicksBase", virtual_time_ticks_base_ms_);
  std::move(callback).Run(std::nullopt, std::move(response));
}

// ---- advance ------------------------------------------------------------------

void CaptureSession::Advance(base::DictValue params, ResultCallback callback) {
  if (std::optional<Error> error = CheckState("advance")) {
    std::move(callback).Run(error, base::DictValue());
    return;
  }
  const int64_t frame_index = params.FindInt("frameIndex").value_or(-1);
  if (frame_index == last_response_frame_ && last_response_frame_ >= 0) {
    // The controller lost our response; replay it (DESIGN.md 4.3).
    std::move(callback).Run(std::nullopt, last_response_.Clone());
    return;
  }
  if (frame_index != next_frame_) {
    base::DictValue details;
    details.Set("expected", static_cast<double>(next_frame_));
    std::move(callback).Run(
        Error{"FrameIndexMismatch", "expected frame " + base::NumberToString(next_frame_), std::move(details)},
        base::DictValue());
    return;
  }
  if (deferred_) {
    std::move(callback).Run(Error{"FrameIndexMismatch", "previous advance is still in flight"}, base::DictValue());
    return;
  }
  auto pending = std::make_unique<PendingAdvance>();
  pending->frame_index = frame_index;
  pending->capture_video = params.FindBool("captureVideo").value_or(true);
  pending->capture_audio = params.FindBool("captureAudio").value_or(true);
  pending->hash = params.FindBool("hash").value_or(true);
  pending->started = base::TimeTicks::Now();
  pending->callback = std::move(callback);
  RunAdvance(std::move(pending));
}

void CaptureSession::RunAdvance(std::unique_ptr<PendingAdvance> pending) {
  state_ = State::kCapturing;
  const int64_t budget_us = FrameOffsetMicros(pending->frame_index + 1) -
                            FrameOffsetMicros(pending->frame_index);
  virtual_time_->Grant(
      budget_us / 1000.0,
      base::BindOnce(&CaptureSession::OnGranted, weak_factory_.GetWeakPtr(),
                     std::move(pending)));
}

void CaptureSession::OnGranted(std::unique_ptr<PendingAdvance> pending,
                               std::optional<std::string> error) {
  pending->after_grant = base::TimeTicks::Now();
  grant_ms_.push_back((pending->after_grant - pending->started).InMillisecondsF());
  if (error) {
    Error err{"VirtualTimeStall", *error};
    Fail(err);
    std::move(pending->callback).Run(err, base::DictValue());
    return;
  }
  PrepareFrame(std::move(pending));
}

void CaptureSession::PrepareFrame(std::unique_ptr<PendingAdvance> pending) {
  content::RenderFrameHost* rfh = web_contents_->GetPrimaryMainFrame();
  if (!rfh || !rfh->IsRenderFrameLive()) {
    Error err{"RendererCrashed", "no live main frame for prepare"};
    Fail(err);
    std::move(pending->callback).Run(err, base::DictValue());
    return;
  }
  if (!agent_.is_bound() || rfh->GetGlobalId() != agent_frame_id_) {
    agent_.reset();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(&agent_);
    agent_.set_disconnect_handler(
        base::BindOnce(&CaptureSession::OnAgentDisconnected, weak_factory_.GetWeakPtr()));
    agent_frame_id_ = rfh->GetGlobalId();
  }
  const int64_t index = pending->frame_index;
  auto params = mojom::PrepareParams::New();
  params->frame_index = index;
  params->frame_interval_ms =
      (FrameOffsetMicros(index + 1) - FrameOffsetMicros(index)) / 1000.0;
  params->marker_prefix = marker_prefix_;
  params->collect_marks = collect_marks_;
  params->wait_timeout_ms = media_wait_timeout_ms_;
  // Audio is always rendered so media clocks advance; it is written to the
  // sink only when captureAudio is set (DESIGN.md 4.3).
  params->render_audio = true;
  params->audio_frames_through = AudioSamplesThrough(index + 1);
  prepare_pending_ = std::move(pending);
  // The renderer may spend the quiescence wait plus one 10 s decode wait each
  // for video and audio before it answers.
  prepare_timer_.Start(FROM_HERE, base::Milliseconds(media_wait_timeout_ms_ + 25000),
                       base::BindOnce(&CaptureSession::OnPrepareTimeout, weak_factory_.GetWeakPtr()));
  agent_->Prepare(std::move(params),
                  base::BindOnce(&CaptureSession::OnPrepared, weak_factory_.GetWeakPtr(), index));
}

void CaptureSession::OnPrepared(int64_t frame_index, mojom::PrepareResultPtr result) {
  if (!prepare_pending_ || prepare_pending_->frame_index != frame_index) {
    return;  // answered already by a timeout or a disconnect
  }
  prepare_timer_.Stop();
  std::unique_ptr<PendingAdvance> pending = std::move(prepare_pending_);
  pending->after_prepare = base::TimeTicks::Now();
  prepare_ms_.push_back((pending->after_prepare - pending->after_grant).InMillisecondsF());
  wake_ms_.push_back(result->wake_ms);
  quiesce_ms_.push_back(result->quiesce_ms);
  pending->wake_ms = result->wake_ms;
  pending->quiesce_ms = result->quiesce_ms;
  pending->audio_ms = result->audio_ms;
  const double interval_ms =
      (FrameOffsetMicros(frame_index + 1) - FrameOffsetMicros(frame_index)) / 1000.0;
  for (const mojom::CaptureMarkerPtr& marker : result->markers) {
    // A mark made during the grant for frame n (age < one interval) belongs
    // to n; one made in frame n-1's rAF (age == one interval, +-0.1 ms of
    // TimeClamper rounding) belongs to n-1. The quarter-millisecond slack
    // keeps the rounding on the right side of the boundary.
    int64_t frames_back = 0;
    if (interval_ms > 0) {
      frames_back = static_cast<int64_t>((marker->age_ms + 0.25) / interval_ms);
    }
    base::DictValue m;
    m.Set("name", marker->name);
    m.Set("frameIndex", static_cast<double>(std::max<int64_t>(0, frame_index - frames_back)));
    m.Set("source", marker->source);
    m.Set("ageMs", marker->age_ms);
    pending->markers.Append(std::move(m));
  }
  for (const mojom::CaptureMediaStatePtr& media : result->media) {
    base::DictValue m;
    m.Set("playerId", media->player_id);
    m.Set("state", media->state);
    m.Set("mediaTimeMs", media->media_time_ms);
    m.Set("stable", media->stable);
    m.Set("hasVideo", media->has_video);
    m.Set("hasAudio", media->has_audio);
    pending->media.Append(std::move(m));
  }
  static constexpr std::string_view kUnsupportedPrefix = "UnsupportedApiUsed: ";
  for (const std::string& warning : result->warnings) {
    pending->warnings.Append(warning);
    if (fail_on_unsupported_api_ && warning.starts_with(kUnsupportedPrefix)) {
      base::DictValue details;
      details.Set("api", warning.substr(kUnsupportedPrefix.size()));
      details.Set("frameIndex", static_cast<double>(frame_index));
      Error err{"UnsupportedApiUsed", warning, std::move(details)};
      Fail(err);
      std::move(pending->callback).Run(err, base::DictValue());
      return;
    }
  }
  if (result->audio_frames > 0 || !result->pcm.empty()) {
    const int64_t expected = AudioSamplesThrough(frame_index + 1) - AudioSamplesThrough(frame_index);
    if (static_cast<int64_t>(result->audio_frames) != expected ||
        result->pcm.size() != static_cast<size_t>(result->audio_frames) * audio_channels_ ||
        result->audio_channels != audio_channels_ || result->audio_rate != audio_rate_) {
      base::DictValue details;
      details.Set("frameIndex", static_cast<double>(frame_index));
      details.Set("expectedFrames", static_cast<double>(expected));
      details.Set("frames", static_cast<double>(result->audio_frames));
      details.Set("rendererRate", result->audio_rate);
      details.Set("rendererChannels", result->audio_channels);
      Error err{"AudioFormatUnsupported", "renderer audio does not match the configured device format", std::move(details)};
      Fail(err);
      std::move(pending->callback).Run(err, base::DictValue());
      return;
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    if (audio_s16_) {
      bytes->resize(result->pcm.size() * 2);
      for (size_t i = 0; i < result->pcm.size(); ++i) {
        const float v = std::clamp(result->pcm[i], -1.0f, 1.0f);
        const int16_t s = static_cast<int16_t>(std::lrintf(v * 32767.0f));
        (*bytes)[2 * i] = static_cast<uint8_t>(s & 0xff);
        (*bytes)[2 * i + 1] = static_cast<uint8_t>((s >> 8) & 0xff);
      }
    } else {
      bytes->resize(result->pcm.size() * 4);
      memcpy(bytes->data(), result->pcm.data(), bytes->size());
    }
    pending->pcm_frame_hash = Sha256Hex(base::span<const uint8_t>(*bytes));
    pcm_chain_hash_ = ChainHash(pcm_chain_hash_, pending->pcm_frame_hash);
    pending->audio_frames = result->audio_frames;
    if (pending->capture_audio) {
      pending->audio = std::move(bytes);  // counted when the slot is written
    }
  }
  if (!result->timed_out_players.empty()) {
    base::DictValue details;
    base::ListValue players;
    for (int32_t id : result->timed_out_players) {
      players.Append(id);
    }
    details.Set("players", std::move(players));
    details.Set("media", pending->media.Clone());
    details.Set("frameIndex", static_cast<double>(frame_index));
    if (fail_on_media_wait_timeout_) {
      Error err{"MediaWaitTimeout", "media pipelines did not settle within the wait timeout", std::move(details)};
      Fail(err);
      std::move(pending->callback).Run(err, base::DictValue());
      return;
    }
    pending->warnings.Append("MediaWaitTimeout: " + base::NumberToString(result->timed_out_players.size()) +
                             " player(s) still unstable; continuing (media.failOnWaitTimeout=false)");
  }
  IssueBeginFrame(std::move(pending));
}

void CaptureSession::OnPrepareTimeout() {
  if (!prepare_pending_) {
    return;
  }
  std::unique_ptr<PendingAdvance> pending = std::move(prepare_pending_);
  base::DictValue details;
  details.Set("frameIndex", static_cast<double>(pending->frame_index));
  Error err{"PrepareTimeout", "renderer did not answer CaptureAgent.Prepare", std::move(details)};
  Fail(err);
  std::move(pending->callback).Run(err, base::DictValue());
}

void CaptureSession::OnAgentDisconnected() {
  agent_.reset();
  if (!prepare_pending_) {
    return;
  }
  prepare_timer_.Stop();
  std::unique_ptr<PendingAdvance> pending = std::move(prepare_pending_);
  Error err{"RendererCrashed", "CaptureAgent disconnected during prepare"};
  Fail(err);
  std::move(pending->callback).Run(err, base::DictValue());
}

void CaptureSession::IssueBeginFrame(std::unique_ptr<PendingAdvance> pending) {
  HeadlessWebContentsImpl* headless_contents = HeadlessWebContentsImpl::From(web_contents_);
  if (!headless_contents || !headless_contents->begin_frame_control_enabled()) {
    Error err{"NotConfigured", "BeginFrameControl is not enabled on this target"};
    Fail(err);
    std::move(pending->callback).Run(err, base::DictValue());
    return;
  }
  const int64_t index = pending->frame_index;
  const base::TimeTicks frame_time = FrameTicks(index);
  const base::TimeDelta interval = base::Microseconds(FrameOffsetMicros(index + 1) - FrameOffsetMicros(index));
  const bool wants_readback = pending->capture_video || pending->hash;
  PendingAdvance* raw = pending.get();
  headless_contents->BeginFrame(
      frame_time, frame_time + interval, interval, /*animate_only=*/false,
      wants_readback,
      base::BindOnce(&CaptureSession::OnFrameFinished, weak_factory_.GetWeakPtr(),
                     std::move(pending)));
  (void)raw;
}

void CaptureSession::OnFrameFinished(std::unique_ptr<PendingAdvance> pending,
                                     bool has_damage,
                                     std::unique_ptr<SkBitmap> bitmap,
                                     std::string error) {
  pending->after_begin_frame = base::TimeTicks::Now();
  begin_frame_ms_.push_back((pending->after_begin_frame - pending->after_prepare).InMillisecondsF());
  if (!error.empty()) {
    Error err{"RendererCrashed", "BeginFrame failed: " + error};
    Fail(err);
    std::move(pending->callback).Run(err, base::DictValue());
    return;
  }
  const bool wants_readback = pending->capture_video || pending->hash;
  const int64_t index = pending->frame_index;
  bool reused = false;
  bool has_surface = true;
  std::shared_ptr<const SkBitmap> frame_bitmap;
  if (wants_readback && (!bitmap || bitmap->drawsNothing()) &&
      !pending->retried_for_surface && index == next_frame_) {
    // The copy request is issued before the BeginFrame and needs an existing
    // surface; the first frame after navigation has none. Now that this
    // BeginFrame produced one, repeat the BeginFrame at the same frame time
    // with the copy request so frame 0 is captured too.
    content::RenderWidgetHostView* view = web_contents_->GetRenderWidgetHostView();
    if (view && view->IsSurfaceAvailableForCopy()) {
      pending->retried_for_surface = true;
      // This callback runs inside HeadlessWebContentsImpl's PendingFrame
      // teardown, which still counts as a pending frame; re-enter later.
      content::GetUIThreadTaskRunner({})->PostTask(
          FROM_HERE, base::BindOnce(&CaptureSession::IssueBeginFrame,
                                    weak_factory_.GetWeakPtr(), std::move(pending)));
      return;
    }
  }
  if (wants_readback) {
    if (bitmap && !bitmap->drawsNothing()) {
      // The readback bitmap is kept as-is (shared memory from viz, N32
      // premultiplied); hashing and the sink copy run on the worker directly
      // from it. No canonical copy on the UI thread (DESIGN.md 13.2).
      width_ = bitmap->width();
      height_ = bitmap->height();
      frame_bitmap = std::shared_ptr<const SkBitmap>(std::move(bitmap));
      cached_bitmap_ = frame_bitmap;
      cached_bitmap_frame_ = index;
    } else if (!has_damage && cached_bitmap_ && cached_bitmap_frame_ == index - 1) {
      frame_bitmap = cached_bitmap_;
      cached_bitmap_frame_ = index;
      reused = true;
      ++reused_frames_;
    } else {
      has_surface = false;
      if (pending->capture_video) {
        Error err{"NoSurface", "no readback for frame " + base::NumberToString(index)};
        Fail(err);
        std::move(pending->callback).Run(err, base::DictValue());
        return;
      }
    }
  }
  if (frame_bitmap) {
    ++inflight_;
    const bool write = pending->capture_video && sink_wants_frames_;
    const bool hash = pending->hash;
    const int64_t virtual_time_us = FrameOffsetMicros(index + 1);
    worker_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](std::shared_ptr<const SkBitmap> frame, int64_t index, int64_t virtual_time_us,
               bool hash, bool write, std::shared_ptr<const std::vector<uint8_t>> audio,
               uint32_t audio_frames, RawFrameSink* sink) {
              Processed out{index};
              const base::TimeTicks t0 = base::TimeTicks::Now();
              std::optional<Sha256Digest> digest;
              if (hash) {
                digest = Sha256OfBitmap(*frame);
                if (digest) {
                  out.frame_hash = HexLower(*digest);
                } else {
                  out.sink_error = SinkError{index, 0, false, 0, 0, "NoSurface: readback bitmap could not be hashed"};
                }
              }
              const base::TimeTicks t1 = base::TimeTicks::Now();
              out.hash = t1 - t0;
              if (write && !out.sink_error) {
                RawFrameSink::Frame slot;
                slot.frame_index = index;
                slot.virtual_time_us = virtual_time_us;
                slot.bitmap = std::move(frame);
                slot.hash = digest;
                slot.audio = std::move(audio);
                slot.audio_frames = audio_frames;
                out.sink_error = sink->Write(slot);
                if (!out.sink_error) {
                  out.audio_frames_written = audio_frames;
                }
                out.sink = base::TimeTicks::Now() - t1;
              }
              return out;
            },
            frame_bitmap, index, virtual_time_us, hash, write, pending->audio, pending->audio_frames,
            base::Unretained(sink_.get())),
        base::BindOnce(&CaptureSession::OnFrameProcessed, weak_factory_.GetWeakPtr()));
    if (pending->capture_video) {
      ++frames_written_;
    }
  }
  next_frame_ = index + 1;
  if (inflight_ > max_queued_frames_) {
    // Backpressure: hold the response until the worker catches up.
    deferred_ = std::move(pending);
    deferred_has_damage_ = has_damage;
    deferred_has_surface_ = has_surface;
    deferred_reused_ = reused;
    return;
  }
  Respond(std::move(pending), has_damage, has_surface, reused);
}

void CaptureSession::OnFrameProcessed(Processed processed) {
  --inflight_;
  audio_sample_frames_ += processed.audio_frames_written;
  hash_ms_.push_back(processed.hash.InMillisecondsF());
  sink_ms_.push_back(processed.sink.InMillisecondsF());
  if (!processed.frame_hash.empty()) {
    chain_hash_ = ChainHash(chain_hash_, processed.frame_hash);
    processed.chain_hash = chain_hash_;
  }
  if (processed.sink_error && !first_sink_error_) {
    first_sink_error_ = processed.sink_error;
    Fail(Error{"SinkClosed", processed.sink_error->message, SinkErrorDict(*processed.sink_error)});
  }
  completed_.push_back(std::move(processed));
  MaybeRespondDeferred();
}

void CaptureSession::MaybeRespondDeferred() {
  if (deferred_ && inflight_ <= max_queued_frames_) {
    std::unique_ptr<PendingAdvance> pending = std::move(deferred_);
    Respond(std::move(pending), deferred_has_damage_, deferred_has_surface_, deferred_reused_);
  }
}

base::ListValue CaptureSession::TakeCompleted() {
  base::ListValue list;
  for (const Processed& p : completed_) {
    base::DictValue d;
    d.Set("frameIndex", static_cast<double>(p.frame_index));
    if (!p.frame_hash.empty()) {
      d.Set("frameHash", p.frame_hash);
      d.Set("chainHash", p.chain_hash);
    }
    d.Set("hashMs", p.hash.InMillisecondsF());
    d.Set("sinkMs", p.sink.InMillisecondsF());
    if (p.sink_error) {
      d.Set("sinkError", SinkErrorDict(*p.sink_error));
    }
    list.Append(std::move(d));
  }
  completed_.clear();
  return list;
}

void CaptureSession::Respond(std::unique_ptr<PendingAdvance> pending,
                             bool has_damage,
                             bool has_surface,
                             bool reused) {
  if (first_sink_error_ && last_error_) {
    std::move(pending->callback).Run(last_error_, base::DictValue());
    return;
  }
  const int64_t index = pending->frame_index;
  base::DictValue r;
  r.Set("frameIndex", static_cast<double>(index));
  r.Set("virtualTimeMs", FrameOffsetMicros(index + 1) / 1000.0);
  r.Set("hasSurface", has_surface);
  r.Set("hasDamage", has_damage);
  r.Set("reusedReadback", reused);
  r.Set("surfaceWarmed", pending->retried_for_surface);
  r.Set("audioSampleFrames", static_cast<double>(pending->audio_frames));
  if (!pending->pcm_frame_hash.empty()) {
    r.Set("pcmFrameHash", pending->pcm_frame_hash);
  }
  r.Set("markers", std::move(pending->markers));
  r.Set("media", std::move(pending->media));
  base::DictValue timings;
  timings.Set("grantMs", (pending->after_grant - pending->started).InMillisecondsF());
  timings.Set("prepareMs", (pending->after_prepare - pending->after_grant).InMillisecondsF());
  timings.Set("wakeMs", pending->wake_ms);
  timings.Set("quiesceMs", pending->quiesce_ms);
  timings.Set("audioMs", pending->audio_ms);
  timings.Set("beginFrameMs", (pending->after_begin_frame - pending->after_prepare).InMillisecondsF());
  timings.Set("queueDepth", inflight_);
  timings.Set("sinkQueued", sink_ ? static_cast<double>(sink_->frames_queued()) : 0.0);
  r.Set("timings", std::move(timings));
  r.Set("completed", TakeCompleted());
  r.Set("warnings", std::move(pending->warnings));
  last_response_ = r.Clone();
  last_response_frame_ = index;
  std::move(pending->callback).Run(std::nullopt, std::move(r));
}

// ---- advanceRange -------------------------------------------------------------

void CaptureSession::AdvanceRange(base::DictValue params, ResultCallback callback) {
  if (std::optional<Error> error = CheckState("advanceRange")) {
    std::move(callback).Run(error, base::DictValue());
    return;
  }
  const int64_t from = params.FindInt("fromFrameIndex").value_or(next_frame_);
  const int64_t to = params.FindInt("toFrameIndex").value_or(next_frame_);
  if (from != next_frame_) {
    std::move(callback).Run(Error{"FrameIndexMismatch", "expected frame " + base::NumberToString(next_frame_)},
                            base::DictValue());
    return;
  }
  RangeStep(std::move(params), to, base::ListValue(), std::move(callback), std::nullopt, base::DictValue());
}

void CaptureSession::RangeStep(base::DictValue params,
                               int64_t to_frame,
                               base::ListValue frames,
                               ResultCallback callback,
                               std::optional<Error> error,
                               base::DictValue frame_result) {
  if (!frame_result.empty()) {
    frames.Append(std::move(frame_result));
  }
  if (error) {
    base::DictValue details = error->details.Clone();
    details.Set("lastCompletedFrameIndex", static_cast<double>(next_frame_ - 1));
    std::move(callback).Run(Error{error->kind, error->message, std::move(details)}, base::DictValue());
    return;
  }
  if (next_frame_ >= to_frame) {
    base::DictValue r;
    r.Set("frames", std::move(frames));
    r.Set("nextFrameIndex", static_cast<double>(next_frame_));
    std::move(callback).Run(std::nullopt, std::move(r));
    return;
  }
  auto pending = std::make_unique<PendingAdvance>();
  pending->frame_index = next_frame_;
  pending->capture_video = false;
  pending->capture_audio = params.FindBool("captureAudio").value_or(false);
  pending->hash = params.FindBool("hash").value_or(false);
  pending->started = base::TimeTicks::Now();
  pending->callback = base::BindOnce(&CaptureSession::RangeStep, weak_factory_.GetWeakPtr(),
                                     params.Clone(), to_frame, std::move(frames), std::move(callback));
  // RangeStep may run from a frame-finished callback; start the next frame
  // from a fresh task so the previous PendingFrame is gone.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&CaptureSession::RunAdvance, weak_factory_.GetWeakPtr(),
                                std::move(pending)));
}

// ---- finish / abort / state ---------------------------------------------------

void CaptureSession::Finish(ResultCallback callback) {
  if (state_ == State::kUnconfigured) {
    std::move(callback).Run(Error{"NotConfigured", "finish before configure"}, base::DictValue());
    return;
  }
  // Drain the conversion sequence, then close the sink on it.
  worker_->PostTaskAndReply(
      FROM_HERE, base::DoNothing(),
      base::BindOnce(&CaptureSession::OnDrained, weak_factory_.GetWeakPtr(), std::move(callback)));
}

void CaptureSession::OnDrained(ResultCallback callback) {
  std::optional<SinkError> close_error = sink_ ? sink_->Close() : std::nullopt;
  if (close_error && !first_sink_error_) {
    first_sink_error_ = close_error;
  }
  const int64_t written = sink_ ? sink_->frames_written() : frames_written_;
  const bool complete = state_ != State::kFailed && state_ != State::kAborted &&
                        (expected_frames_ < 0 || written == expected_frames_);
  if (!complete && state_ != State::kFailed && state_ != State::kAborted) {
    Fail(Error{"IncompleteOutput", "engine wrote " + base::NumberToString(written) + " of " +
                                       base::NumberToString(expected_frames_) + " frames"});
  }
  base::DictValue r;
  r.Set("status", complete ? "complete" : "failed");
  r.Set("framesWritten", static_cast<double>(written));
  if (expected_frames_ >= 0) {
    r.Set("expectedFrames", static_cast<double>(expected_frames_));
  }
  r.Set("reusedFrames", static_cast<double>(reused_frames_));
  r.Set("audioSampleFrames", static_cast<double>(audio_sample_frames_));
  if (!chain_hash_.empty()) {
    r.Set("chainHash", chain_hash_);
  }
  if (!pcm_chain_hash_.empty()) {
    r.Set("pcmHash", pcm_chain_hash_);
  }
  base::DictValue histograms;
  histograms.Set("grantMs", Histogram(grant_ms_));
  histograms.Set("prepareMs", Histogram(prepare_ms_));
  histograms.Set("wakeMs", Histogram(wake_ms_));
  histograms.Set("quiesceMs", Histogram(quiesce_ms_));
  histograms.Set("beginFrameMs", Histogram(begin_frame_ms_));
  histograms.Set("hashMs", Histogram(hash_ms_));
  histograms.Set("sinkMs", Histogram(sink_ms_));
  r.Set("histograms", std::move(histograms));
  r.Set("gpuProcessCrashes", gpu_process_crashes_);
  if (first_sink_error_) {
    r.Set("firstSinkError", SinkErrorDict(*first_sink_error_));
  }
  r.Set("completed", TakeCompleted());
  if (complete) {
    state_ = State::kFinished;
  }
  std::move(callback).Run(std::nullopt, std::move(r));
}

void CaptureSession::Abort() {
  if (sink_) {
    sink_->Close(/*aborted=*/true);
  }
  state_ = State::kAborted;
}

base::DictValue CaptureSession::GetState() const {
  base::DictValue d;
  d.Set("state", StateName(state_));
  d.Set("nextFrameIndex", static_cast<double>(next_frame_));
  if (last_error_) {
    base::DictValue err;
    err.Set("kind", last_error_->kind);
    err.Set("message", last_error_->message);
    err.Set("details", last_error_->details.Clone());
    d.Set("lastError", std::move(err));
  }
  return d;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(CaptureSession);

}  // namespace headless::capture
