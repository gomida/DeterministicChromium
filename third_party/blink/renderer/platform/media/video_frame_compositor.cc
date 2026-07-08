// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/media/video_frame_compositor.h"

#include <cinttypes>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "media/base/media_switches.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/ffmpeg_common.h"
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
constexpr double kDeterministicVideoQueueAheadSeconds = 0.5;
constexpr double kDeterministicVideoQueueBehindSeconds = 0.25;
constexpr double kDeterministicVideoSequentialGapSeconds = 1.0;

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

struct AVFormatContextDeleter {
  void operator()(AVFormatContext* context) const {
    if (context) {
      avformat_close_input(&context);
    }
  }
};

struct AVIOContextDeleter {
  void operator()(AVIOContext* context) const {
    if (context) {
      av_free(context->buffer);
      av_free(context);
    }
  }
};

struct DeterministicQueuedFrame {
  int64_t output_index = 0;
  scoped_refptr<media::VideoFrame> frame;
};

std::string DeterministicVideoCacheKey(const std::string& source_url,
                                       const gfx::Size& size,
                                       int fps) {
  return base::StringPrintf("%s|%dx%d|%d", source_url.c_str(), size.width(),
                            size.height(), fps);
}

base::TimeDelta DeterministicVideoFrameTime(int64_t frame_index, int fps) {
  if (frame_index < 0) {
    DeterministicVideoFatal(base::StringPrintf(
        "negative deterministic frame index: %" PRId64, frame_index));
  }
  return base::Microseconds(frame_index * base::Time::kMicrosecondsPerSecond /
                            fps);
}

int64_t DeterministicVideoFrameIndex(base::TimeDelta media_time, int fps) {
  if (media_time < base::TimeDelta()) {
    DeterministicVideoFatal(base::StringPrintf(
        "negative deterministic media time: us=%" PRId64,
        media_time.InMicroseconds()));
  }
  return media_time.InMicroseconds() * static_cast<int64_t>(fps) /
         base::Time::kMicrosecondsPerSecond;
}

int64_t DeterministicVideoQueueFrames(double seconds, int fps) {
  return static_cast<int64_t>(std::ceil(seconds * fps));
}

class DeterministicVideoFileAvio {
 public:
  explicit DeterministicVideoFileAvio(const base::FilePath& input_path)
      : input_file_(input_path,
                    base::File::FLAG_OPEN | base::File::FLAG_READ) {
    if (!input_file_.IsValid()) {
      DeterministicVideoFatal("failed to open deterministic video file: " +
                              input_path.AsUTF8Unsafe());
    }

    constexpr int kAvioBufferSize = 64 * 1024;
    auto* buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (!buffer) {
      DeterministicVideoFatal("failed to allocate deterministic AVIO buffer");
    }

    avio_context_.reset(
        avio_alloc_context(buffer, kAvioBufferSize, 0, this, &ReadPacket,
                           nullptr, &Seek));
    if (!avio_context_) {
      av_free(buffer);
      DeterministicVideoFatal("failed to allocate deterministic AVIO context");
    }
    avio_context_->seekable = AVIO_SEEKABLE_NORMAL;
    avio_context_->write_flag = 0;
  }

  AVIOContext* context() const { return avio_context_.get(); }

 private:
  static int ReadPacket(void* opaque, uint8_t* buffer, int buffer_size) {
    if (buffer_size < 0) {
      return AVERROR(EIO);
    }
    auto* self = static_cast<DeterministicVideoFileAvio*>(opaque);
    std::optional<size_t> read = self->input_file_.ReadAtCurrentPos(
        UNSAFE_BUFFERS(base::span(buffer,
                                  static_cast<size_t>(buffer_size))));
    if (!read) {
      return AVERROR(EIO);
    }
    if (*read == 0) {
      return AVERROR_EOF;
    }
    return static_cast<int>(*read);
  }

  static int64_t Seek(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<DeterministicVideoFileAvio*>(opaque);
    if (whence == AVSEEK_SIZE) {
      const int64_t length = self->input_file_.GetLength();
      return length >= 0 ? length : AVERROR(EIO);
    }

    base::File::Whence origin;
    switch (whence) {
      case SEEK_SET:
        origin = base::File::FROM_BEGIN;
        break;
      case SEEK_CUR:
        origin = base::File::FROM_CURRENT;
        break;
      case SEEK_END:
        origin = base::File::FROM_END;
        break;
      default:
        return AVERROR(EIO);
    }
    const int64_t position = self->input_file_.Seek(origin, offset);
    return position >= 0 ? position : AVERROR(EIO);
  }

  base::File input_file_;
  std::unique_ptr<AVIOContext, AVIOContextDeleter> avio_context_;
};

class DeterministicVideoFfmpegStream {
 public:
  DeterministicVideoFfmpegStream(const base::FilePath& input_path,
                                 const gfx::Size& output_size,
                                 int fps)
      : input_path_(input_path),
        output_size_(output_size),
        fps_(fps),
        frame_bytes_(static_cast<size_t>(output_size.width()) *
                     output_size.height() * 3 / 2) {}

  ~DeterministicVideoFfmpegStream() { Stop(); }

  std::vector<uint8_t> ReadFrame(int64_t output_index) {
    if (!process_.IsValid() || output_index != next_output_index_) {
      Start(output_index);
    }

    std::vector<uint8_t> raw_frame(frame_bytes_);
    std::optional<size_t> read =
        stdout_pipe_.ReadAtCurrentPos(base::span(raw_frame));
    if (read != raw_frame.size()) {
      int exit_code = 0;
      const bool exited =
          process_.WaitForExitWithTimeout(base::Milliseconds(0), &exit_code);
      DeterministicVideoFatal(base::StringPrintf(
          "ffmpeg deterministic stream ended early: path=%s index=%" PRId64
          " read=%zu expected=%zu exited=%d exit_code=%d",
          input_path_.AsUTF8Unsafe().c_str(), output_index,
          read.value_or(0), raw_frame.size(), exited, exit_code));
    }

    ++next_output_index_;
    return raw_frame;
  }

  void Stop() {
    stdout_pipe_.Close();
    if (process_.IsValid()) {
      process_.Terminate(1, true);
      process_.Close();
    }
    next_output_index_ = -1;
  }

 private:
  void Start(int64_t output_index) {
    Stop();

    base::ScopedFD read_fd;
    base::ScopedFD write_fd;
    if (!base::CreatePipe(&read_fd, &write_fd, false)) {
      DeterministicVideoFatal("failed to create deterministic ffmpeg pipe");
    }

    base::File dev_null(base::FilePath("/dev/null"),
                        base::File::FLAG_OPEN | base::File::FLAG_WRITE);
    if (!dev_null.IsValid()) {
      DeterministicVideoFatal("failed to open /dev/null for ffmpeg stderr");
    }

    const base::TimeDelta start_time =
        DeterministicVideoFrameTime(output_index, fps_);
    // Decode through a Chromium-owned ffmpeg stdout stream. The capture harness
    // supplies only the common clock; it never prepares per-video frame files.
    base::CommandLine ffmpeg(DeterministicVideoFfmpegPath());
    ffmpeg.AppendArg("-v");
    ffmpeg.AppendArg("error");
    ffmpeg.AppendArg("-nostdin");
    ffmpeg.AppendArg("-ss");
    ffmpeg.AppendArg(base::StringPrintf("%.6f", start_time.InSecondsF()));
    ffmpeg.AppendArg("-i");
    ffmpeg.AppendArgPath(input_path_);
    ffmpeg.AppendArg("-an");
    ffmpeg.AppendArg("-sn");
    ffmpeg.AppendArg("-dn");
    ffmpeg.AppendArg("-vf");
    ffmpeg.AppendArg(base::StringPrintf("fps=%d,scale=%d:%d,format=nv12",
                                        fps_, output_size_.width(),
                                        output_size_.height()));
    ffmpeg.AppendArg("-f");
    ffmpeg.AppendArg("rawvideo");
    ffmpeg.AppendArg("pipe:1");

    base::LaunchOptions options;
    options.fds_to_remap.emplace_back(write_fd.get(), STDOUT_FILENO);
    options.fds_to_remap.emplace_back(dev_null.GetPlatformFile(),
                                      STDERR_FILENO);
    process_ = base::LaunchProcess(ffmpeg, options);
    write_fd.reset();
    dev_null.Close();
    if (!process_.IsValid()) {
      DeterministicVideoFatal("failed to launch deterministic ffmpeg stream: " +
                              ffmpeg.GetCommandLineString());
    }

    stdout_pipe_ = base::File(read_fd.release());
    next_output_index_ = output_index;
  }

  const base::FilePath input_path_;
  const gfx::Size output_size_;
  const int fps_;
  const size_t frame_bytes_;
  base::File stdout_pipe_;
  base::Process process_;
  int64_t next_output_index_ = -1;
};

class DeterministicVideoFrameQueue {
 public:
  DeterministicVideoFrameQueue(std::string source_url,
                               const gfx::Size& output_size,
                               int fps)
      : source_url_(std::move(source_url)),
        output_size_(output_size),
        fps_(fps) {
    InitializeDecoder();
  }

  scoped_refptr<media::VideoFrame> GetFrame(
      base::TimeDelta media_time,
      bool is_looping,
      const media::VideoFrame& reference_frame) {
    base::AutoLock lock(lock_);
    base::TimeDelta content_time = NormalizeMediaTime(media_time, is_looping);
    const int64_t target_index = DeterministicVideoFrameIndex(content_time,
                                                              fps_);
    EnsureFrameReady(target_index, reference_frame);
    for (const DeterministicQueuedFrame& queued_frame : output_frames_) {
      if (queued_frame.output_index == target_index) {
        return queued_frame.frame;
      }
    }
    DeterministicVideoFatal(base::StringPrintf(
        "deterministic queue failed to produce target frame: index=%" PRId64
        " source=%s",
        target_index, source_url_.c_str()));
  }

 private:
  void InitializeDecoder() {
    if (output_size_.IsEmpty() || output_size_.width() % 2 != 0 ||
        output_size_.height() % 2 != 0) {
      DeterministicVideoFatal("NV12 substitution requires non-empty even video "
                              "dimensions: " +
                              output_size_.ToString());
    }

    GURL url(source_url_);
    if (!net::FileURLToFilePath(url, &input_path_)) {
      DeterministicVideoFatal("only file:// video URLs are supported: " +
                              source_url_);
    }
    if (!base::PathExists(input_path_)) {
      DeterministicVideoFatal("video source does not exist: " +
                              input_path_.AsUTF8Unsafe());
    }

    AVFormatContext* raw_format_context = avformat_alloc_context();
    if (!raw_format_context) {
      DeterministicVideoFatal(
          "failed to allocate deterministic format context");
    }
    file_avio_ = std::make_unique<DeterministicVideoFileAvio>(input_path_);
    // Chromium's bundled FFmpeg may not expose file/url protocols. Feed the
    // browser-selected local file through AVIO so demuxing uses Chromium I/O.
    raw_format_context->flags |= AVFMT_FLAG_CUSTOM_IO;
    raw_format_context->pb = file_avio_->context();
    int result = avformat_open_input(&raw_format_context, nullptr, nullptr,
                                     nullptr);
    if (result < 0) {
      if (raw_format_context) {
        avformat_free_context(raw_format_context);
      }
      DeterministicVideoFatal("failed to open deterministic video source: " +
                              input_path_.AsUTF8Unsafe() + " url=" +
                              source_url_ + ": " +
                              media::AVErrorToString(result));
    }
    format_context_.reset(raw_format_context);

    result = avformat_find_stream_info(format_context_.get(), nullptr);
    if (result < 0) {
      DeterministicVideoFatal("failed to read deterministic video stream info: " +
                              input_path_.AsUTF8Unsafe() + ": " +
                              media::AVErrorToString(result));
    }

    auto streams = media::AVFormatContextToSpan(format_context_.get());
    for (AVStream* stream : streams) {
      if (stream && stream->codecpar &&
          stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        video_stream_ = stream;
        break;
      }
    }
    if (!video_stream_) {
      DeterministicVideoFatal("deterministic video source has no video stream: " +
                              input_path_.AsUTF8Unsafe());
    }

    if (video_stream_->duration != media::kNoFFmpegTimestamp &&
        video_stream_->duration > 0) {
      duration_ = media::ConvertFromTimeBase(video_stream_->time_base,
                                             video_stream_->duration);
    } else if (format_context_->duration != media::kNoFFmpegTimestamp &&
               format_context_->duration > 0) {
      duration_ = base::Microseconds(format_context_->duration);
    }
    if (duration_ <= base::TimeDelta()) {
      DeterministicVideoFatal("deterministic video duration is unavailable: " +
                              input_path_.AsUTF8Unsafe());
    }
    ffmpeg_stream_ = std::make_unique<DeterministicVideoFfmpegStream>(
        input_path_, output_size_, fps_);
  }

  base::TimeDelta NormalizeMediaTime(base::TimeDelta media_time,
                                     bool is_looping) const {
    if (media_time < base::TimeDelta()) {
      DeterministicVideoFatal(base::StringPrintf(
          "negative deterministic video media time: us=%" PRId64 " source=%s",
          media_time.InMicroseconds(), source_url_.c_str()));
    }
    if (media_time < duration_) {
      return media_time;
    }
    if (!is_looping) {
      DeterministicVideoFatal(base::StringPrintf(
          "non-looping video reached deterministic end: us=%" PRId64
          " duration_us=%" PRId64 " source=%s",
          media_time.InMicroseconds(), duration_.InMicroseconds(),
          source_url_.c_str()));
    }
    const int64_t duration_us = duration_.InMicroseconds();
    if (duration_us <= 0) {
      DeterministicVideoFatal("looping video has invalid duration: " +
                              source_url_);
    }
    return base::Microseconds(media_time.InMicroseconds() % duration_us);
  }

  void EnsureFrameReady(int64_t target_index,
                        const media::VideoFrame& reference_frame) {
    if (!output_frames_.empty()) {
      for (const DeterministicQueuedFrame& queued_frame : output_frames_) {
        if (queued_frame.output_index == target_index) {
          return;
        }
      }
    }

    const int64_t max_sequential_gap = DeterministicVideoQueueFrames(
        kDeterministicVideoSequentialGapSeconds, fps_);
    if (output_frames_.empty() || target_index < next_output_index_ ||
        target_index > next_output_index_ + max_sequential_gap) {
      SeekToOutputFrame(target_index);
    }

    const int64_t last_needed =
        target_index + DeterministicVideoQueueFrames(
                           kDeterministicVideoQueueAheadSeconds, fps_);
    while (next_output_index_ <= last_needed) {
      GenerateOutputFrame(next_output_index_, reference_frame);
      ++next_output_index_;
    }
    Prune(target_index);
  }

  void SeekToOutputFrame(int64_t output_index) {
    ffmpeg_stream_->Stop();
    output_frames_.clear();
    next_output_index_ = output_index;
  }

  void GenerateOutputFrame(int64_t output_index,
                           const media::VideoFrame& reference_frame) {
    const base::TimeDelta target_time =
        DeterministicVideoFrameTime(output_index, fps_);

    std::vector<uint8_t> raw_frame = ffmpeg_stream_->ReadFrame(output_index);
    scoped_refptr<media::VideoFrame> frame = CreateOutputFrameFromRaw(
        raw_frame, target_time, reference_frame);
    output_frames_.push_back({output_index, std::move(frame)});
  }

  scoped_refptr<media::VideoFrame> CreateOutputFrameFromRaw(
      const std::vector<uint8_t>& raw_frame,
      base::TimeDelta timestamp,
      const media::VideoFrame& reference_frame) {
    scoped_refptr<media::VideoFrame> frame = media::VideoFrame::CreateFrame(
        media::PIXEL_FORMAT_NV12, output_size_, gfx::Rect(output_size_),
        output_size_, timestamp);
    if (!frame) {
      DeterministicVideoFatal("failed to allocate deterministic NV12 frame");
    }

    frame->set_color_space(reference_frame.ColorSpace());
    frame->metadata().transformation = reference_frame.metadata().transformation;
    frame->metadata().frame_duration = base::Seconds(1.0 / fps_);
    frame->metadata().frame_rate = fps_;

    const size_t y_bytes =
        static_cast<size_t>(output_size_.width()) * output_size_.height();
    if (raw_frame.size() != y_bytes * 3 / 2) {
      DeterministicVideoFatal("deterministic raw frame size changed");
    }

    const uint8_t* src_y = raw_frame.data();
    const uint8_t* src_uv = UNSAFE_BUFFERS(raw_frame.data() + y_bytes);
    uint8_t* dst_y = frame->writable_data(media::VideoFrame::Plane::kY);
    uint8_t* dst_uv = frame->writable_data(media::VideoFrame::Plane::kUV);
    const size_t dst_y_stride = frame->stride(media::VideoFrame::Plane::kY);
    const size_t dst_uv_stride = frame->stride(media::VideoFrame::Plane::kUV);
    const size_t row_bytes = static_cast<size_t>(output_size_.width());
    for (int y = 0; y < output_size_.height(); ++y) {
      UNSAFE_BUFFERS(
          std::memcpy(dst_y + y * dst_y_stride, src_y + y * row_bytes,
                      row_bytes));
    }
    for (int y = 0; y < output_size_.height() / 2; ++y) {
      UNSAFE_BUFFERS(
          std::memcpy(dst_uv + y * dst_uv_stride, src_uv + y * row_bytes,
                      row_bytes));
    }
    return frame;
  }

  void Prune(int64_t target_index) {
    const int64_t keep_before = DeterministicVideoQueueFrames(
        kDeterministicVideoQueueBehindSeconds, fps_);
    while (!output_frames_.empty() &&
           output_frames_.front().output_index < target_index - keep_before) {
      output_frames_.pop_front();
    }
  }

  base::Lock lock_;
  const std::string source_url_;
  const gfx::Size output_size_;
  const int fps_;
  base::FilePath input_path_;
  std::unique_ptr<DeterministicVideoFileAvio> file_avio_;
  std::unique_ptr<DeterministicVideoFfmpegStream> ffmpeg_stream_;
  std::unique_ptr<AVFormatContext, AVFormatContextDeleter> format_context_;
  AVStream* video_stream_ = nullptr;
  base::TimeDelta duration_;
  int64_t next_output_index_ = 0;
  std::deque<DeterministicQueuedFrame> output_frames_
      ALLOW_DISCOURAGED_TYPE("Small FIFO queue for deterministic video frames");
};

class DeterministicVideoFrameQueues {
 public:
  DeterministicVideoFrameQueue& GetOrCreate(const std::string& source_url,
                                            const gfx::Size& output_size,
                                            int fps) {
    const std::string key = DeterministicVideoCacheKey(source_url, output_size,
                                                       fps);
    base::AutoLock lock(lock_);
    auto existing = queues_.find(key);
    if (existing != queues_.end()) {
      return *existing->second;
    }
    auto queue = std::make_unique<DeterministicVideoFrameQueue>(source_url,
                                                               output_size,
                                                               fps);
    DeterministicVideoFrameQueue& result = *queue;
    queues_.emplace(key, std::move(queue));
    return result;
  }

 private:
  base::Lock lock_;
  std::map<std::string, std::unique_ptr<DeterministicVideoFrameQueue>> queues_
      ALLOW_DISCOURAGED_TYPE("Experimental deterministic-video queues");
};

DeterministicVideoFrameQueues& GetDeterministicVideoFrameQueues() {
  static base::NoDestructor<DeterministicVideoFrameQueues> queues;
  return *queues;
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
    // Pre-draw availability checks can ask for the current frame before cc
    // supplies a BeginFrame deadline. Return the native frame only for that
    // untimed startup path so drawing is not suppressed; timed capture calls
    // below still require the single media-time state and substitute pixels.
    return frame;
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
  DeterministicVideoFrameQueue& queue =
      GetDeterministicVideoFrameQueues().GetOrCreate(
          deterministic_video_source_url_, frame->natural_size(), fps);
  return queue.GetFrame(local_time, deterministic_video_is_looping_, *frame);
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
  // Availability probes run before cc has supplied the BeginFrame deadline via
  // WillDrawCurrentFrame(). They must see the native frame so drawing is not
  // skipped; deterministic substitution happens only when pixels are fetched.
  return static_cast<bool>(current_frame_);
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

  // Duplicate detection is an internal media-pipeline decision. Compare against
  // the native frame, not the deterministic replacement frame.
  if (!frame || (current_frame_ && !repaint_duplicate_frame &&
                 frame->unique_id() == current_frame_->unique_id())) {
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
