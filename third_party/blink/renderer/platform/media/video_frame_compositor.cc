// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/media/video_frame_compositor.h"

#include <cinttypes>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "media/base/media_switches.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_frame.h"
#include "net/base/filename_util.h"
#include "third_party/blink/renderer/platform/allow_discouraged_type.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_base.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "url/gurl.h"

namespace blink {

using RenderingMode = ::media::VideoRendererSink::RenderCallback::RenderingMode;

// Amount of time to wait between UpdateCurrentFrame() callbacks before starting
// background rendering to keep the Render() callbacks moving.
const int kBackgroundRenderingTimeoutMs = 250;
const int kForceBeginFramesTimeoutMs = 1000;
constexpr char kDeterministicVideoFfmpegSwitch[] =
    "deterministic-video-ffmpeg";
constexpr char kDeterministicVideoFpsSwitch[] = "deterministic-video-fps";

[[noreturn]] void DeterministicVideoFatal(const std::string& message) {
  LOG(FATAL) << "Deterministic video substitution failed: " << message;
}

bool IsDeterministicVideoEnabled() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      kDeterministicVideoFfmpegSwitch);
}

int DeterministicVideoFps() {
  std::string value = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
      kDeterministicVideoFpsSwitch);
  int fps = 0;
  if (!base::StringToInt(value, &fps) || fps <= 0) {
    DeterministicVideoFatal(
        "--deterministic-video-fps must be a positive integer");
  }
  return fps;
}

base::FilePath DeterministicVideoFfmpegPath() {
  base::FilePath ffmpeg =
      base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
          kDeterministicVideoFfmpegSwitch);
  if (ffmpeg.empty()) {
    DeterministicVideoFatal(
        "--deterministic-video-ffmpeg must point to an ffmpeg binary");
  }
  if (!base::PathExists(ffmpeg)) {
    DeterministicVideoFatal("ffmpeg binary does not exist: " +
                            ffmpeg.AsUTF8Unsafe());
  }
  return ffmpeg;
}

struct DeterministicVideoCacheEntry {
  base::FilePath raw_path;
  gfx::Size size;
  int fps = 0;
  int64_t frame_count = 0;
  int64_t frame_bytes = 0;
};

std::string DeterministicVideoCacheKey(const std::string& source_url,
                                       const gfx::Size& size,
                                       int fps) {
  return base::StringPrintf("%s|%dx%d|%d", source_url.c_str(), size.width(),
                            size.height(), fps);
}

class DeterministicVideoCache {
 public:
  DeterministicVideoCacheEntry GetOrCreate(const std::string& source_url,
                                           const gfx::Size& size,
                                           int fps) {
    const std::string key = DeterministicVideoCacheKey(source_url, size, fps);
    base::AutoLock lock(lock_);
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
      return existing->second;
    }

    if (size.IsEmpty() || size.width() % 2 != 0 || size.height() % 2 != 0) {
      DeterministicVideoFatal("NV12 substitution requires non-empty even video "
                              "dimensions: " +
                              size.ToString());
    }

    if (!temp_dir_.IsValid() &&
        !temp_dir_.CreateUniqueTempDir(
            FILE_PATH_LITERAL("deterministic-video"))) {
      DeterministicVideoFatal("failed to create deterministic video temp dir");
    }

    GURL url(source_url);
    base::FilePath input_path;
    if (!net::FileURLToFilePath(url, &input_path)) {
      DeterministicVideoFatal("only file:// video URLs are supported: " +
                              source_url);
    }
    if (!base::PathExists(input_path)) {
      DeterministicVideoFatal("video source does not exist: " +
                              input_path.AsUTF8Unsafe());
    }

    base::FilePath raw_path = temp_dir_.GetPath().AppendASCII(
        base::StringPrintf("video_%d_%dx%d_%dfps.nv12", next_cache_id_++,
                           size.width(), size.height(), fps));

    base::CommandLine ffmpeg(DeterministicVideoFfmpegPath());
    ffmpeg.AppendArg("-v");
    ffmpeg.AppendArg("error");
    ffmpeg.AppendArg("-y");
    ffmpeg.AppendArg("-i");
    ffmpeg.AppendArgPath(input_path);
    ffmpeg.AppendArg("-vf");
    ffmpeg.AppendArg(base::StringPrintf("fps=%d,scale=%d:%d,format=nv12", fps,
                                        size.width(), size.height()));
    ffmpeg.AppendArg("-f");
    ffmpeg.AppendArg("rawvideo");
    ffmpeg.AppendArgPath(raw_path);

    std::string output;
    if (!base::GetAppOutputAndError(ffmpeg, &output)) {
      DeterministicVideoFatal("ffmpeg raw decode failed for " +
                              input_path.AsUTF8Unsafe() + ": " + output);
    }

    std::optional<int64_t> raw_size = base::GetFileSize(raw_path);
    if (!raw_size || *raw_size <= 0) {
      DeterministicVideoFatal("ffmpeg produced empty raw output: " +
                              raw_path.AsUTF8Unsafe());
    }

    const int64_t frame_bytes =
        static_cast<int64_t>(size.width()) * size.height() * 3 / 2;
    if (*raw_size % frame_bytes != 0) {
      DeterministicVideoFatal(base::StringPrintf(
          "raw NV12 size is not frame-aligned: path=%s size=%" PRId64
          " frame_bytes=%" PRId64,
          raw_path.AsUTF8Unsafe().c_str(), *raw_size, frame_bytes));
    }

    DeterministicVideoCacheEntry entry;
    entry.raw_path = raw_path;
    entry.size = size;
    entry.fps = fps;
    entry.frame_bytes = frame_bytes;
    entry.frame_count = *raw_size / frame_bytes;
    if (entry.frame_count <= 0) {
      DeterministicVideoFatal("decoded video contains no deterministic frames");
    }
    entries_.emplace(key, entry);
    return entry;
  }

 private:
  base::Lock lock_;
  base::ScopedTempDir temp_dir_;
  int next_cache_id_ = 0;
  std::map<std::string, DeterministicVideoCacheEntry> entries_
      ALLOW_DISCOURAGED_TYPE("Experimental deterministic-video cache");
};

DeterministicVideoCache& GetDeterministicVideoCache() {
  static base::NoDestructor<DeterministicVideoCache> cache;
  return *cache;
}

scoped_refptr<media::VideoFrame> ReadDeterministicVideoFrame(
    const DeterministicVideoCacheEntry& entry,
    int64_t frame_index,
    base::TimeDelta timestamp,
    const media::VideoFrame& source_frame) {
  if (frame_index < 0 || frame_index >= entry.frame_count) {
    DeterministicVideoFatal(base::StringPrintf(
        "deterministic frame index out of range: index=%" PRId64
        " frame_count=%" PRId64,
        frame_index, entry.frame_count));
  }

  std::vector<uint8_t> raw_frame(entry.frame_bytes);
  base::File raw_file(entry.raw_path,
                      base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!raw_file.IsValid()) {
    DeterministicVideoFatal("failed to open deterministic raw cache: " +
                            entry.raw_path.AsUTF8Unsafe());
  }
  std::optional<size_t> read =
      raw_file.Read(frame_index * entry.frame_bytes, base::span(raw_frame));
  if (read != raw_frame.size()) {
    DeterministicVideoFatal(base::StringPrintf(
        "failed to read deterministic frame: index=%" PRId64 " read=%zu "
        "expected=%zu",
        frame_index, read.value_or(0), raw_frame.size()));
  }

  scoped_refptr<media::VideoFrame> frame = media::VideoFrame::CreateFrame(
      media::PIXEL_FORMAT_NV12, entry.size, gfx::Rect(entry.size), entry.size,
      timestamp);
  if (!frame) {
    DeterministicVideoFatal("failed to allocate deterministic NV12 VideoFrame");
  }
  frame->set_color_space(source_frame.ColorSpace());
  frame->metadata().transformation = source_frame.metadata().transformation;
  frame->metadata().frame_duration = base::Seconds(1.0 / entry.fps);
  frame->metadata().frame_rate = entry.fps;

  const uint8_t* src_y = raw_frame.data();
  const uint8_t* src_uv =
      raw_frame.data() + static_cast<size_t>(entry.size.width()) *
                             entry.size.height();
  uint8_t* dst_y = frame->writable_data(media::VideoFrame::Plane::kY);
  uint8_t* dst_uv = frame->writable_data(media::VideoFrame::Plane::kUV);
  const size_t dst_y_stride =
      frame->stride(media::VideoFrame::Plane::kY);
  const size_t dst_uv_stride =
      frame->stride(media::VideoFrame::Plane::kUV);
  const size_t row_bytes = static_cast<size_t>(entry.size.width());
  for (int y = 0; y < entry.size.height(); ++y) {
    std::memcpy(dst_y + y * dst_y_stride, src_y + y * row_bytes, row_bytes);
  }
  for (int y = 0; y < entry.size.height() / 2; ++y) {
    std::memcpy(dst_uv + y * dst_uv_stride, src_uv + y * row_bytes, row_bytes);
  }
  return frame;
}

// static
constexpr const char VideoFrameCompositor::kTracingCategory[];

VideoFrameCompositor::VideoFrameCompositor(
    const scoped_refptr<base::SingleThreadTaskRunner>& task_runner,
    std::unique_ptr<WebVideoFrameSubmitter> submitter)
    : task_runner_(task_runner),
      tick_clock_(base::DefaultTickClock::GetInstance()),
      background_rendering_timer_(
          FROM_HERE,
          base::Milliseconds(kBackgroundRenderingTimeoutMs),
          ConvertToBaseRepeatingCallback(
              CrossThreadBindRepeating(&VideoFrameCompositor::BackgroundRender,
                                       CrossThreadUnretained(this),
                                       RenderingMode::kBackground))),
      force_begin_frames_timer_(
          FROM_HERE,
          base::Milliseconds(kForceBeginFramesTimeoutMs),
          ConvertToBaseRepeatingCallback(CrossThreadBindRepeating(
              &VideoFrameCompositor::StopForceBeginFrames,
              CrossThreadUnretained(this)))),
      submitter_(std::move(submitter)) {
  if (submitter_) {
    PostCrossThreadTask(
        *task_runner_, FROM_HERE,
        CrossThreadBindOnce(&VideoFrameCompositor::InitializeSubmitter,
                            weak_ptr_factory_.GetWeakPtr()));
    update_submission_state_callback_ = base::BindPostTask(
        task_runner_, ConvertToBaseRepeatingCallback(CrossThreadBindRepeating(
                          &VideoFrameCompositor::SetIsSurfaceVisible,
                          weak_ptr_factory_.GetWeakPtr())));
  }
}

cc::UpdateSubmissionStateCB
VideoFrameCompositor::GetUpdateSubmissionStateCallback() {
  return update_submission_state_callback_;
}

void VideoFrameCompositor::SetIsSurfaceVisible(
    bool is_visible,
    base::WaitableEvent* done_event) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  submitter_->SetIsSurfaceVisible(is_visible);
  if (done_event)
    done_event->Signal();
}

void VideoFrameCompositor::InitializeSubmitter() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  submitter_->Initialize(this, /* is_media_stream = */ false);
}

VideoFrameCompositor::~VideoFrameCompositor() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(!callback_);
  DCHECK(!rendering_);
  if (client_)
    client_->StopUsingProvider();
}

void VideoFrameCompositor::EnableSubmission(
    const viz::SurfaceId& id,
    media::VideoTransformation transform,
    bool force_submit) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  // If we're switching to |submitter_| from some other client, then tell it.
  if (client_ && client_ != submitter_.get())
    client_->StopUsingProvider();

  submitter_->SetTransform(transform);
  submitter_->SetForceSubmit(force_submit);
  submitter_->EnableSubmission(id);
  client_ = submitter_.get();
  if (rendering_)
    client_->StartRendering();
}

bool VideoFrameCompositor::IsClientSinkAvailable() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  return client_;
}

void VideoFrameCompositor::OnRendererStateUpdate(bool new_state) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK_NE(rendering_, new_state);
  rendering_ = new_state;

  auto track = perfetto::NamedTrack::ThreadScoped("VideoPlayback", this);
  if (rendering_) {
    TRACE_EVENT_BEGIN(kTracingCategory, "Rendering", track);
  } else {
    new_processed_frame_cb_.Reset();
    TRACE_EVENT_END(kTracingCategory, track);
  }

  if (rendering_) {
    // Always start playback in background rendering mode, if |client_| kicks
    // in right away it's okay.
    BackgroundRender(RenderingMode::kStartup);
  } else if (background_rendering_enabled_) {
    background_rendering_timer_.Stop();
  } else {
    DCHECK(!background_rendering_timer_.IsRunning());
  }

  if (!IsClientSinkAvailable())
    return;

  if (rendering_)
    client_->StartRendering();
  else
    client_->StopRendering();
}

void VideoFrameCompositor::SetVideoFrameProviderClient(
    cc::VideoFrameProvider::Client* client) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (client_)
    client_->StopUsingProvider();
  client_ = client;

  // |client_| may now be null, so verify before calling it.
  if (rendering_ && client_)
    client_->StartRendering();
}

scoped_refptr<media::VideoFrame> VideoFrameCompositor::GetCurrentFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  return GetDeterministicVideoFrameIfNeeded(current_frame_);
}

scoped_refptr<media::VideoFrame>
VideoFrameCompositor::GetCurrentFrameOnAnyThread() {
  base::AutoLock lock(current_frame_lock_);

  // Treat frames vended to external consumers as being rendered. This ensures
  // that hidden elements that are being driven by WebGL/WebGPU/Canvas rendering
  // don't mark all frames as dropped.
  rendered_last_frame_ = true;

  return current_frame_;
}

void VideoFrameCompositor::SetCurrentFrame_Locked(
    scoped_refptr<media::VideoFrame> frame,
    base::TimeTicks expected_display_time) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  TRACE_EVENT1("media", "VideoFrameCompositor::SetCurrentFrame", "frame",
               frame->AsHumanReadableString());
  current_frame_lock_.AssertAcquired();
  current_frame_ = std::move(frame);
  last_presentation_time_ = tick_clock_->NowTicks();
  last_expected_display_time_ = expected_display_time;
  ++presentation_counter_;
}

void VideoFrameCompositor::SetDeterministicVideoSourceUrl(
    std::string source_url,
    bool is_looping) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  deterministic_video_source_url_ = std::move(source_url);
  deterministic_video_is_looping_ = is_looping;
  deterministic_video_last_begin_frame_time_ = base::TimeTicks();
}

void VideoFrameCompositor::SetDeterministicVideoMediaTimeState(
    base::TimeDelta media_time,
    base::TimeTicks sample_ticks,
    double playback_rate) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  deterministic_video_media_time_ = media_time;
  deterministic_video_media_time_sample_ticks_ = sample_ticks;
  deterministic_video_playback_rate_ = playback_rate;
}

void VideoFrameCompositor::PutCurrentFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  base::AutoLock lock(current_frame_lock_);
  rendered_last_frame_ = true;
}

bool VideoFrameCompositor::UpdateCurrentFrame(base::TimeTicks deadline_min,
                                              base::TimeTicks deadline_max) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  TRACE_EVENT2("media", "VideoFrameCompositor::UpdateCurrentFrame",
               "deadline_min", deadline_min, "deadline_max", deadline_max);
  base::TimeDelta interval = deadline_max - deadline_min;
  if (interval.is_positive()) {
    // VideoFrameSubmitter passes the presentation deadline derived from the
    // caller's beginFrame; that is the timeline native substitution follows.
    deterministic_video_last_begin_frame_time_ = deadline_min;
  }
  return CallRender(deadline_min, deadline_max, RenderingMode::kNormal);
}

void VideoFrameCompositor::WillDrawCurrentFrame(base::TimeTicks deadline_min,
                                                base::TimeTicks deadline_max) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (!IsDeterministicVideoEnabled()) {
    return;
  }
  base::TimeDelta interval = deadline_max - deadline_min;
  if (interval.is_positive()) {
    // The visible <video> layer can draw a previously decoded frame without the
    // VideoFrameSubmitter path. Capture must still use the current BeginFrame
    // deadline so native pixels never leak into the recorded surface.
    deterministic_video_last_begin_frame_time_ = deadline_min;
  }
}

scoped_refptr<media::VideoFrame>
VideoFrameCompositor::GetDeterministicVideoFrameIfNeeded(
    scoped_refptr<media::VideoFrame> frame) {
  if (!IsDeterministicVideoEnabled()) {
    return frame;
  }

  if (deterministic_video_last_begin_frame_time_.is_null()) {
    if (deterministic_video_source_url_.empty()) {
      return frame;
    }
    // Manual startup submissions do not carry BeginFrame timing. Drop them
    // rather than exposing native pixels; visible capture draws set timing via
    // WillDrawCurrentFrame() immediately before GetCurrentFrame().
    return nullptr;
  }

  if (!frame) {
    DeterministicVideoFatal("capture beginFrame requested video without a "
                            "current media::VideoFrame");
  }
  if (deterministic_video_source_url_.empty()) {
    DeterministicVideoFatal("capture beginFrame reached video without a "
                            "deterministic source URL");
  }

  const int fps = DeterministicVideoFps();
  // Use WebMediaPlayerImpl::GetCurrentTimeInternal() as the single media-time
  // source. Do not fall back to frame timestamps or source-load epochs.
  const base::TimeDelta local_time = GetDeterministicVideoMediaTime();
  int64_t frame_index =
      local_time.InMicroseconds() * static_cast<int64_t>(fps) /
      base::Time::kMicrosecondsPerSecond;
  DeterministicVideoCacheEntry entry =
      GetDeterministicVideoCache().GetOrCreate(deterministic_video_source_url_,
                                               frame->natural_size(), fps);
  if (frame_index >= entry.frame_count) {
    if (!deterministic_video_is_looping_) {
      DeterministicVideoFatal(base::StringPrintf(
          "non-looping video reached end of deterministic cache: index=%" PRId64
          " frame_count=%" PRId64 " source=%s",
          frame_index, entry.frame_count,
          deterministic_video_source_url_.c_str()));
    }
    // HTML <video loop> repeats from the beginning. Keep non-looping media
    // fatal above so an ended video is never silently repeated.
    frame_index %= entry.frame_count;
  }
  return ReadDeterministicVideoFrame(entry, frame_index, local_time, *frame);
}

base::TimeDelta VideoFrameCompositor::GetDeterministicVideoMediaTime() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (deterministic_video_media_time_ == media::kNoTimestamp ||
      deterministic_video_media_time_sample_ticks_.is_null()) {
    DeterministicVideoFatal("deterministic video media-time state is missing");
  }
  base::TimeDelta media_time = deterministic_video_media_time_;
  if (deterministic_video_playback_rate_ != 0.0) {
    if (deterministic_video_last_begin_frame_time_ <
        deterministic_video_media_time_sample_ticks_) {
      DeterministicVideoFatal("deterministic video beginFrame moved before "
                              "the media-time sample");
    }
    // The media-time sample comes from WebMediaPlayerImpl::GetCurrentTimeInternal()
    // on the main thread. Advance that single media clock by the same
    // beginFrame timeline used for capture, instead of reading another video
    // timestamp or falling back to source-load time.
    media_time +=
        (deterministic_video_last_begin_frame_time_ -
         deterministic_video_media_time_sample_ticks_) *
        deterministic_video_playback_rate_;
  }
  if (media_time == media::kNoTimestamp || media_time < base::TimeDelta() ||
      media_time.is_inf()) {
    DeterministicVideoFatal(base::StringPrintf(
        "deterministic video media time is invalid: us=%" PRId64 " source=%s",
        media_time.InMicroseconds(), deterministic_video_source_url_.c_str()));
  }
  return media_time;
}

bool VideoFrameCompositor::HasCurrentFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  return static_cast<bool>(GetCurrentFrame());
}

base::TimeDelta VideoFrameCompositor::GetPreferredRenderInterval() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  base::AutoLock lock(callback_lock_);

  if (!callback_)
    return viz::BeginFrameArgs::MinInterval();
  return callback_->GetPreferredRenderInterval();
}

void VideoFrameCompositor::Start(RenderCallback* callback) {
  // Called from the media thread, so acquire the callback under lock before
  // returning in case a Stop() call comes in before the PostTask is processed.
  base::AutoLock lock(callback_lock_);
  DCHECK(!callback_);
  callback_ = callback;
  PostCrossThreadTask(
      *task_runner_, FROM_HERE,
      CrossThreadBindOnce(&VideoFrameCompositor::OnRendererStateUpdate,
                          weak_ptr_factory_.GetWeakPtr(), true));
}

void VideoFrameCompositor::Stop() {
  // Called from the media thread, so release the callback under lock before
  // returning to avoid a pending UpdateCurrentFrame() call occurring before
  // the PostTask is processed.
  base::AutoLock lock(callback_lock_);
  DCHECK(callback_);
  callback_ = nullptr;
  PostCrossThreadTask(
      *task_runner_, FROM_HERE,
      CrossThreadBindOnce(&VideoFrameCompositor::OnRendererStateUpdate,
                          weak_ptr_factory_.GetWeakPtr(), false));
}

void VideoFrameCompositor::PaintSingleFrame(
    scoped_refptr<media::VideoFrame> frame,
    bool repaint_duplicate_frame) {
  if (!task_runner_->BelongsToCurrentThread()) {
    PostCrossThreadTask(
        *task_runner_, FROM_HERE,
        CrossThreadBindOnce(&VideoFrameCompositor::PaintSingleFrame,
                            weak_ptr_factory_.GetWeakPtr(), std::move(frame),
                            repaint_duplicate_frame));
    return;
  }
  if (ProcessNewFrame(std::move(frame), tick_clock_->NowTicks(),
                      repaint_duplicate_frame) &&
      IsClientSinkAvailable()) {
    client_->DidReceiveFrame();
  }
}

void VideoFrameCompositor::UpdateCurrentFrameIfStale(UpdateType type) {
  TRACE_EVENT0("media", "VideoFrameCompositor::UpdateCurrentFrameIfStale");
  DCHECK(task_runner_->BelongsToCurrentThread());

  // If we're not rendering, then the frame can't be stale.
  if (!rendering_ || !is_background_rendering_)
    return;

  // If we have a client, and it is currently rendering, then it's not stale
  // since the client is driving the frame updates at the proper rate.
  if (type != UpdateType::kBypassClient && IsClientSinkAvailable() &&
      client_->IsDrivingFrameUpdates()) {
    return;
  }

  // We're rendering, but the client isn't driving the updates.  See if the
  // frame is stale, and update it.

  DCHECK(!last_background_render_.is_null());

  const base::TimeTicks now = tick_clock_->NowTicks();
  const base::TimeDelta interval = now - last_background_render_;

  // Cap updates to 250Hz which should be more than enough for everyone.
  if (interval < base::Milliseconds(4))
    return;

  {
    base::AutoLock lock(callback_lock_);
    // Update the interval based on the time between calls and call background
    // render which will give this information to the client.
    last_interval_ = interval;
  }
  BackgroundRender();
}

void VideoFrameCompositor::SetOnNewProcessedFrameCallback(
    OnNewProcessedFrameCB cb) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  new_processed_frame_cb_ = std::move(cb);
}

void VideoFrameCompositor::SetOnFramePresentedCallback(
    OnNewFramePresentedCB present_cb) {
  base::AutoLock lock(current_frame_lock_);
  new_presented_frame_cb_ = std::move(present_cb);

  PostCrossThreadTask(
      *task_runner_, FROM_HERE,
      CrossThreadBindOnce(&VideoFrameCompositor::StartForceBeginFrames,
                          weak_ptr_factory_.GetWeakPtr()));
}

void VideoFrameCompositor::StartForceBeginFrames() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (!submitter_)
    return;

  submitter_->SetForceBeginFrames(true);
  force_begin_frames_timer_.Reset();
}

void VideoFrameCompositor::StopForceBeginFrames() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  submitter_->SetForceBeginFrames(false);
}

std::unique_ptr<WebMediaPlayer::VideoFramePresentationMetadata>
VideoFrameCompositor::GetLastPresentedFrameMetadata() {
  auto frame_metadata =
      std::make_unique<WebMediaPlayer::VideoFramePresentationMetadata>();

  scoped_refptr<media::VideoFrame> last_frame;
  {
    // Manually acquire the lock instead of calling GetCurrentFrameOnAnyThread()
    // to also fetch the other frame dependent properties.
    base::AutoLock lock(current_frame_lock_);
    last_frame = current_frame_;
    frame_metadata->presentation_time = last_presentation_time_;
    frame_metadata->expected_display_time = last_expected_display_time_;
    frame_metadata->presented_frames = presentation_counter_;
  }

  if (last_frame) {
    frame_metadata->width = last_frame->visible_rect().width();
    frame_metadata->height = last_frame->visible_rect().height();
    frame_metadata->media_time = last_frame->timestamp();
    frame_metadata->metadata.MergeMetadataFrom(last_frame->metadata());
  }

  {
    base::AutoLock lock(callback_lock_);
    if (callback_) {
      frame_metadata->average_frame_duration =
          callback_->GetPreferredRenderInterval();
    } else if (last_frame && last_frame->metadata().frame_duration) {
      frame_metadata->average_frame_duration =
          *last_frame->metadata().frame_duration;
    }
    frame_metadata->rendering_interval = last_interval_;
  }

  return frame_metadata;
}

bool VideoFrameCompositor::ProcessNewFrame(
    scoped_refptr<media::VideoFrame> frame,
    base::TimeTicks presentation_time,
    bool repaint_duplicate_frame) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  if (!frame || (GetCurrentFrame() && !repaint_duplicate_frame &&
                 frame->unique_id() == GetCurrentFrame()->unique_id())) {
    return false;
  }

  // TODO(crbug.com/40064689): Add other cases where the frame is not readable.
  bool is_frame_readable = !frame->metadata().dcomp_surface;

  // Copy to a local variable to avoid potential deadlock when executing the
  // callback.
  OnNewFramePresentedCB frame_presented_cb;
  {
    base::AutoLock lock(current_frame_lock_);

    // Set the flag indicating that the current frame is unrendered, if we get a
    // subsequent PutCurrentFrame() call it will mark it as rendered.
    rendered_last_frame_ = false;

    SetCurrentFrame_Locked(std::move(frame), presentation_time);
    frame_presented_cb = std::move(new_presented_frame_cb_);
  }

  if (new_processed_frame_cb_) {
    std::move(new_processed_frame_cb_)
        .Run(tick_clock_->NowTicks(), is_frame_readable);
  }

  if (frame_presented_cb) {
    std::move(frame_presented_cb).Run();
  }

  return true;
}

void VideoFrameCompositor::SetIsPageVisible(bool is_visible) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (submitter_)
    submitter_->SetIsPageVisible(is_visible);
}

void VideoFrameCompositor::SetForceSubmit(bool force_submit) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  // The `submitter_` can be null in tests.
  if (submitter_)
    submitter_->SetForceSubmit(force_submit);
}

base::TimeDelta VideoFrameCompositor::GetLastIntervalWithoutLock()
    NO_THREAD_SAFETY_ANALYSIS {
  DCHECK(task_runner_->BelongsToCurrentThread());
  // |last_interval_| is only updated on the compositor thread, so it's safe to
  // return it without acquiring |callback_lock_|
  return last_interval_;
}

void VideoFrameCompositor::BackgroundRender(RenderingMode mode) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  const base::TimeTicks now = tick_clock_->NowTicks();
  last_background_render_ = now;
  bool new_frame = CallRender(now, now + GetLastIntervalWithoutLock(), mode);
  if (new_frame && IsClientSinkAvailable())
    client_->DidReceiveFrame();
}

bool VideoFrameCompositor::CallRender(base::TimeTicks deadline_min,
                                      base::TimeTicks deadline_max,
                                      RenderingMode mode) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  bool have_unseen_frame;
  {
    base::AutoLock lock(current_frame_lock_);
    have_unseen_frame = !rendered_last_frame_ && HasCurrentFrame();
  }

  base::AutoLock lock(callback_lock_);

  if (!callback_) {
    // Even if we no longer have a callback, return true if we have a frame
    // which |client_| hasn't seen before.
    return have_unseen_frame;
  }

  DCHECK(rendering_);

  // If the previous frame was never rendered and we're in the normal rendering
  // mode and haven't just exited background rendering, let the client know.
  //
  // We don't signal for mode == kBackground since we expect to drop frames. We
  // also don't signal for mode == kStartup since UpdateCurrentFrame() may occur
  // before the PutCurrentFrame() for the kStartup induced CallRender().
  const bool was_background_rendering = is_background_rendering_;
  if (have_unseen_frame && mode == RenderingMode::kNormal &&
      !was_background_rendering) {
    callback_->OnFrameDropped();
  }

  const bool new_frame = ProcessNewFrame(
      callback_->Render(deadline_min, deadline_max, mode), deadline_min, false);

  // In cases where mode == kStartup we still want to treat it like background
  // rendering mode since CallRender() wasn't generated by UpdateCurrentFrame().
  is_background_rendering_ = mode != RenderingMode::kNormal;
  last_interval_ = deadline_max - deadline_min;

  // We may create a new frame here with background rendering, but the provider
  // has no way of knowing that a new frame had been processed, so keep track of
  // the new frame, and return true on the next call to |CallRender|.
  const bool had_new_background_frame = new_background_frame_;
  new_background_frame_ = is_background_rendering_ && new_frame;

  // Restart the background rendering timer whether we're background rendering
  // or not; in either case we should wait for |kBackgroundRenderingTimeoutMs|.
  if (background_rendering_enabled_)
    background_rendering_timer_.Reset();
  return new_frame || had_new_background_frame;
}

void VideoFrameCompositor::OnContextLost() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  // current_frame_'s resource in the context has been lost, so current_frame_
  // is not valid any more. current_frame_ should be reset. Now the compositor
  // has no concept of resetting current_frame_, so a black frame is set.
  base::AutoLock lock(current_frame_lock_);
  if (!current_frame_ || (!current_frame_->HasSharedImage() &&
                          !current_frame_->HasMappableSharedImage())) {
    return;
  }
  scoped_refptr<media::VideoFrame> black_frame =
      media::VideoFrame::CreateBlackFrame(current_frame_->natural_size());
  SetCurrentFrame_Locked(std::move(black_frame), tick_clock_->NowTicks());
}

}  // namespace blink
