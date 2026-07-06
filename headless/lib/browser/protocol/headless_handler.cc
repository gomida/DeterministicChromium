// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/protocol/headless_handler.h"

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "base/base_switches.h"
#include "base/check_deref.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/posix/eintr_wrapper.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "cc/base/switches.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/switches.h"
#include "content/public/common/content_switches.h"
#include "headless/lib/browser/headless_browser_impl.h"
#include "headless/lib/browser/headless_web_contents_impl.h"  // nogncheck http://crbug.com/1227378
#include "third_party/libyuv/include/libyuv/convert_from_argb.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/codec/webp_codec.h"
#include "ui/gfx/image/image.h"

namespace headless::protocol {

using HeadlessExperimental::ScreenshotParams;

namespace {

constexpr int kDefaultScreenshotQuality = 80;
constexpr char kHeadlessRawFrameDumpSwitch[] = "headless-raw-frame-dump";
constexpr char kHeadlessRawFrameDumpFormatSwitch[] =
    "headless-raw-frame-dump-format";
constexpr char kHeadlessRawFrameDumpAsyncSwitch[] =
    "headless-raw-frame-dump-async";
constexpr size_t kMaxAsyncRawFrameQueuedBytes = 512 * 1024 * 1024;

using BitmapEncoder =
    base::RepeatingCallback<std::optional<std::vector<uint8_t>>(
        const SkBitmap& bitmap)>;

std::optional<std::vector<uint8_t>> EncodeBitmapAsPngSlow(
    const SkBitmap& bitmap) {
  TRACE_EVENT0("devtools", "EncodeBitmapAsPngSlow");
  return gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                           /*discard_transparency=*/false);
}

std::optional<std::vector<uint8_t>> EncodeBitmapAsPngFast(
    const SkBitmap& bitmap) {
  TRACE_EVENT0("devtools", "EncodeBitmapAsPngFast");
  return gfx::PNGCodec::FastEncodeBGRASkBitmap(bitmap,
                                               /*discard_transparency=*/false);
}

std::optional<std::vector<uint8_t>> EncodeBitmapAsJpeg(int quality,
                                                       const SkBitmap& bitmap) {
  TRACE_EVENT0("devtools", "EncodeBitmapAsJpeg");
  return gfx::JPEGCodec::Encode(bitmap, quality);
}

std::optional<std::vector<uint8_t>> EncodeBitmapAsWebp(int quality,
                                                       const SkBitmap& bitmap) {
  TRACE_EVENT0("devtools", "EncodeBitmapAsWebp");
  return gfx::WebpCodec::Encode(bitmap, quality);
}


base::FilePath GetRawFrameDumpPath() {
  return base::FilePath::FromUTF8Unsafe(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kHeadlessRawFrameDumpSwitch));
}

std::string GetRawFrameDumpFormat() {
  std::string format =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          kHeadlessRawFrameDumpFormatSwitch);
  if (format.empty()) {
    return "nv12";
  }
  return format;
}

bool IsRawFrameDumpAsyncEnabled() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      kHeadlessRawFrameDumpAsyncSwitch);
}

bool WriteAll(int fd, const uint8_t* data, size_t size) {
  while (size > 0) {
    ssize_t written = HANDLE_EINTR(write(fd, data, size));
    if (written < 0) {
      return false;
    }
    data += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

class RawFrameDumpLastFrameCache {
 public:
  struct Frame {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
  };

  RawFrameDumpLastFrameCache() = default;
  RawFrameDumpLastFrameCache(const RawFrameDumpLastFrameCache&) = delete;
  RawFrameDumpLastFrameCache& operator=(const RawFrameDumpLastFrameCache&) =
      delete;

  void Store(const base::FilePath& path,
             std::shared_ptr<const std::vector<uint8_t>> bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path.value();
    frame_ = Frame{std::move(bytes)};
  }

  std::optional<Frame> Get(const base::FilePath& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_.has_value() && path_ == path.value()) {
      return frame_;
    }
    return std::nullopt;
  }

 private:
  std::mutex mutex_;
  std::string path_;
  std::optional<Frame> frame_;
};

RawFrameDumpLastFrameCache& GetRawFrameDumpLastFrameCache() {
  static RawFrameDumpLastFrameCache cache;
  return cache;
}

class AsyncRawFrameDumpWriter {
 public:
  AsyncRawFrameDumpWriter() = default;
  AsyncRawFrameDumpWriter(const AsyncRawFrameDumpWriter&) = delete;
  AsyncRawFrameDumpWriter& operator=(const AsyncRawFrameDumpWriter&) = delete;
  ~AsyncRawFrameDumpWriter() { Stop(); }

  std::optional<std::string> Enqueue(
      base::FilePath path,
      std::shared_ptr<const std::vector<uint8_t>> frame) {
    if (path.empty()) {
      return "Raw frame dump path is empty";
    }
    if (!frame) {
      return "Raw frame dump frame is empty";
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (error_.has_value()) {
      return error_;
    }
    if (!thread_.joinable()) {
      thread_ = std::thread(&AsyncRawFrameDumpWriter::Run, this);
    }
    queue_has_space_cv_.wait(lock, [&]() {
      return error_.has_value() ||
             queued_bytes_ + frame->size() <= kMaxAsyncRawFrameQueuedBytes;
    });
    if (error_.has_value()) {
      return error_;
    }

    queued_bytes_ += frame->size();
    queue_.push_back(PendingFrame{std::move(path), std::move(frame)});
    queue_has_data_cv_.notify_one();
    return std::nullopt;
  }

 private:
  struct PendingFrame {
    base::FilePath path;
    std::shared_ptr<const std::vector<uint8_t>> data;
  };

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      queue_has_data_cv_.notify_all();
      queue_has_space_cv_.notify_all();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (fd_ >= 0) {
      IGNORE_EINTR(close(fd_));
      fd_ = -1;
      path_.clear();
    }
  }

  void Run() {
    for (;;) {
      PendingFrame frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_has_data_cv_.wait(lock,
                                [&]() { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stop_) {
            break;
          }
          continue;
        }
        frame = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= frame.data->size();
        queue_has_space_cv_.notify_all();
      }

      std::string error;
      if (!OpenIfNeeded(frame.path, &error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::move(error);
        queue_has_space_cv_.notify_all();
        break;
      }
      if (!WriteAll(fd_, frame.data->data(), frame.data->size())) {
        error = "Failed to write async raw frame pixels: errno " +
                std::to_string(errno);
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::move(error);
        queue_has_space_cv_.notify_all();
        break;
      }
    }
  }

  bool OpenIfNeeded(const base::FilePath& path, std::string* error) {
    const std::string current_path = path.value();
    if (fd_ >= 0 && path_ == current_path) {
      return true;
    }

    if (fd_ >= 0) {
      IGNORE_EINTR(close(fd_));
      fd_ = -1;
      path_.clear();
    }

    struct stat stat_info;
    const bool is_fifo = stat(current_path.c_str(), &stat_info) == 0 &&
                         S_ISFIFO(stat_info.st_mode);
    int flags = O_WRONLY;
    if (!is_fifo) {
      flags |= O_CREAT | O_TRUNC;
    }

    fd_ = HANDLE_EINTR(open(current_path.c_str(), flags, 0666));
    if (fd_ < 0) {
      *error = "Failed to open async raw frame dump output: errno " +
               std::to_string(errno);
      return false;
    }
    path_ = current_path;
    return true;
  }

  std::mutex mutex_;
  std::condition_variable queue_has_data_cv_;
  std::condition_variable queue_has_space_cv_;
  std::deque<PendingFrame> queue_;
  size_t queued_bytes_ = 0;
  bool stop_ = false;
  std::optional<std::string> error_;
  std::thread thread_;
  int fd_ = -1;
  std::string path_;
};

AsyncRawFrameDumpWriter& GetAsyncRawFrameDumpWriter() {
  static AsyncRawFrameDumpWriter writer;
  return writer;
}

std::optional<std::string> AppendRawFrameBytes(
    const base::FilePath& path,
    std::shared_ptr<const std::vector<uint8_t>> frame) {
  if (!IsRawFrameDumpAsyncEnabled()) {
    return "Raw frame dump requires --headless-raw-frame-dump-async";
  }
  return GetAsyncRawFrameDumpWriter().Enqueue(path, std::move(frame));
}

std::optional<std::string> AppendLastRawFrameAsRaw(
    const base::FilePath& path) {
  std::optional<RawFrameDumpLastFrameCache::Frame> cached =
      GetRawFrameDumpLastFrameCache().Get(path);
  if (!cached.has_value() || !cached->bytes) {
    return "Raw frame dump produced no bitmap and no previous raw frame";
  }
  return AppendRawFrameBytes(path, cached->bytes);
}

std::optional<std::string> PrepareN32Pixmap(const SkBitmap& bitmap,
                                            SkBitmap* n32_bitmap,
                                            SkPixmap* pixmap) {
  const SkBitmap* source = &bitmap;
  if (bitmap.colorType() != kN32_SkColorType) {
    const SkImageInfo info =
        SkImageInfo::MakeN32Premul(bitmap.width(), bitmap.height());
    if (!n32_bitmap->tryAllocPixels(info)) {
      return "Failed to allocate N32 raw frame bitmap";
    }
    if (!bitmap.readPixels(n32_bitmap->pixmap(), 0, 0)) {
      return "Failed to convert raw frame bitmap to N32";
    }
    source = n32_bitmap;
  }

  if (!source->peekPixels(pixmap)) {
    return "Failed to access raw frame bitmap pixels";
  }
  return std::nullopt;
}

std::optional<std::string> AppendBitmapAsNv12Raw(
    const base::FilePath& path,
    const SkBitmap& bitmap) {
  TRACE_EVENT0("devtools", "AppendBitmapAsNv12Raw");
  if (path.empty()) {
    return "Raw frame dump path is empty";
  }
  if (GetRawFrameDumpFormat() != "nv12") {
    return "This raw frame dump build only supports nv12 format";
  }

  SkBitmap n32_bitmap;
  SkPixmap pixmap;
  std::optional<std::string> prepare_error =
      PrepareN32Pixmap(bitmap, &n32_bitmap, &pixmap);
  if (prepare_error.has_value()) {
    return prepare_error;
  }

  const int width = pixmap.width();
  const int height = pixmap.height();
  if (width % 2 != 0 || height % 2 != 0) {
    return "NV12 raw frame dump requires even width and height";
  }

  const size_t y_plane_size = static_cast<size_t>(width) * height;
  const size_t uv_plane_size = static_cast<size_t>(width) * height / 2;
  std::vector<uint8_t> frame(y_plane_size + uv_plane_size);
  uint8_t* y_plane = frame.data();
  uint8_t* uv_plane = frame.data() + y_plane_size;
  const uint8_t* src_argb = static_cast<const uint8_t*>(pixmap.addr(0, 0));
  const int convert_result = libyuv::ARGBToNV12(
      src_argb, static_cast<int>(pixmap.rowBytes()), y_plane, width, uv_plane,
      width, width, height);
  if (convert_result != 0) {
    return "Failed to convert raw frame to NV12";
  }

  auto mutable_frame = std::make_shared<std::vector<uint8_t>>(std::move(frame));
  std::shared_ptr<const std::vector<uint8_t>> frame_bytes = mutable_frame;
  GetRawFrameDumpLastFrameCache().Store(path, frame_bytes);
  return AppendRawFrameBytes(path, std::move(frame_bytes));
}

std::variant<protocol::Response, BitmapEncoder>
GetEncoder(const std::string& format, int quality, bool optimize_for_speed) {
  if (quality < 0 || quality > 100) {
    return Response::InvalidParams(
        "screenshot.quality has to be in range 0..100");
  }
  if (format == ScreenshotParams::FormatEnum::Png) {
    return base::BindRepeating(optimize_for_speed ? EncodeBitmapAsPngFast
                                                  : EncodeBitmapAsPngSlow);
  }
  if (format == ScreenshotParams::FormatEnum::Jpeg)
    return base::BindRepeating(&EncodeBitmapAsJpeg, quality);
  if (format == ScreenshotParams::FormatEnum::Webp)
    return base::BindRepeating(&EncodeBitmapAsWebp, quality);
  return protocol::Response::InvalidParams("Invalid image format");
}

void OnBeginFrameFinished(
    BitmapEncoder encoder,
    std::unique_ptr<HeadlessHandler::BeginFrameCallback> callback,
    bool has_damage,
    std::unique_ptr<SkBitmap> bitmap,
    std::string error_message) {
  if (!error_message.empty()) {
    callback->sendFailure(Response::ServerError(std::move(error_message)));
    return;
  }
  if (encoder.is_null()) {
    callback->sendSuccess(has_damage, std::nullopt);
    return;
  }

  const base::FilePath raw_frame_dump_path = GetRawFrameDumpPath();
  if (!bitmap || bitmap->drawsNothing()) {
    if (!raw_frame_dump_path.empty()) {
      std::optional<std::string> error =
          AppendLastRawFrameAsRaw(raw_frame_dump_path);
      if (error.has_value()) {
        callback->sendFailure(Response::ServerError(std::move(*error)));
        return;
      }
      callback->sendSuccess(has_damage,
                            Binary::fromVector(std::vector<uint8_t>()));
      return;
    }
    callback->sendSuccess(has_damage, std::nullopt);
    return;
  }

  if (!raw_frame_dump_path.empty()) {
    std::optional<std::string> error =
        AppendBitmapAsNv12Raw(raw_frame_dump_path, *bitmap);
    if (error.has_value()) {
      callback->sendFailure(Response::ServerError(std::move(*error)));
      return;
    }
    callback->sendSuccess(has_damage,
                          Binary::fromVector(std::vector<uint8_t>()));
    return;
  }

  std::optional<std::vector<uint8_t>> result = encoder.Run(*bitmap);
  callback->sendSuccess(
      has_damage, Binary::fromVector(result.value_or(std::vector<uint8_t>())));
}

}  // namespace

HeadlessHandler::HeadlessHandler(HeadlessBrowserImpl* browser,
                                 content::WebContents* web_contents)
    : browser_(browser), web_contents_(web_contents) {}

HeadlessHandler::~HeadlessHandler() {}

void HeadlessHandler::Wire(UberDispatcher* dispatcher) {
  frontend_ =
      std::make_unique<HeadlessExperimental::Frontend>(dispatcher->channel());
  HeadlessExperimental::Dispatcher::wire(dispatcher, this);
}

Response HeadlessHandler::Enable() {
  return Response::Success();
}

Response HeadlessHandler::Disable() {
  return Response::Success();
}

void HeadlessHandler::BeginFrame(std::optional<double> in_frame_time_ticks,
                                 std::optional<double> in_interval,
                                 std::optional<bool> in_no_display_updates,
                                 std::unique_ptr<ScreenshotParams> screenshot,
                                 std::unique_ptr<BeginFrameCallback> callback) {
  auto& headless_contents =
      CHECK_DEREF(HeadlessWebContentsImpl::From(web_contents_));
  if (!headless_contents.begin_frame_control_enabled()) {
    callback->sendFailure(Response::ServerError(
        "Command is only supported if BeginFrameControl is enabled."));
    return;
  }

  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          ::switches::kRunAllCompositorStagesBeforeDraw)) {
    callback->sendFailure(Response::ServerError(
        "Command is only supported with "
        "--run-all-compositor-stages-before-draw, see "
        "https://goo.gle/chrome-headless-rendering for more info."));
    return;
  }

  base::TimeTicks frame_time_ticks;
  base::TimeDelta interval;
  bool no_display_updates = in_no_display_updates.value_or(false);

  if (in_frame_time_ticks.has_value()) {
    frame_time_ticks =
        base::TimeTicks() + base::Milliseconds(in_frame_time_ticks.value());
  } else {
    frame_time_ticks = base::TimeTicks::Now();
  }

  if (in_interval.has_value()) {
    double interval_double = in_interval.value();
    if (interval_double <= 0) {
      callback->sendFailure(
          Response::InvalidParams("interval has to be greater than 0"));
      return;
    }
    interval = base::Milliseconds(interval_double);
  } else {
    interval = viz::BeginFrameArgs::DefaultInterval();
  }

  base::TimeTicks deadline = frame_time_ticks + interval;

  BitmapEncoder encoder;
  if (screenshot) {
    ScreenshotParams& params = *screenshot;
    auto encoder_or_response =
        GetEncoder(params.GetFormat(ScreenshotParams::FormatEnum::Png),
                   params.GetQuality(kDefaultScreenshotQuality),
                   params.GetOptimizeForSpeed(false));
    if (std::holds_alternative<protocol::Response>(encoder_or_response)) {
      callback->sendFailure(std::get<protocol::Response>(encoder_or_response));
      return;
    }
    encoder = std::get<BitmapEncoder>(std::move(encoder_or_response));
  }

  const bool capture_screenshot = !!encoder;
  headless_contents.BeginFrame(
      frame_time_ticks, deadline, interval, no_display_updates,
      capture_screenshot,
      base::BindOnce(&OnBeginFrameFinished, std::move(encoder),
                     std::move(callback)));
}

}  // namespace headless::protocol
