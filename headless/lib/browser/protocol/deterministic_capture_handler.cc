// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/protocol/deterministic_capture_handler.h"

#include <utility>

#include "base/command_line.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/web_contents.h"
#include "gpu/config/gpu_info.h"
#include "headless/lib/browser/capture/canonical_hash.h"
#include "headless/lib/browser/capture/capture_session.h"
#include "headless/lib/browser/headless_browser_impl.h"
#include "headless/public/switches.h"

namespace headless::protocol {

namespace {

using capture::CaptureSession;

std::unique_ptr<std::vector<String>> Strings(
    std::initializer_list<const char*> values) {
  auto result = std::make_unique<std::vector<String>>();
  for (const char* value : values) {
    result->push_back(value);
  }
  return result;
}

Response ToResponse(const CaptureSession::Error& error) {
  return Response::ServerError(error.Render());
}

base::ListValue ListOf(const base::DictValue& dict, const char* key) {
  const base::ListValue* list = dict.FindList(key);
  return list ? list->Clone() : base::ListValue();
}

base::DictValue DictOf(const base::DictValue& dict, const char* key) {
  const base::DictValue* value = dict.FindDict(key);
  return value ? value->Clone() : base::DictValue();
}

}  // namespace

DeterministicCaptureHandler::DeterministicCaptureHandler(
    HeadlessBrowserImpl* browser,
    content::WebContents* web_contents)
    : browser_(browser), web_contents_(web_contents) {}

DeterministicCaptureHandler::~DeterministicCaptureHandler() = default;

void DeterministicCaptureHandler::Wire(UberDispatcher* dispatcher) {
  frontend_ =
      std::make_unique<DeterministicCapture::Frontend>(dispatcher->channel());
  DeterministicCapture::Dispatcher::wire(dispatcher, this);
}

Response DeterministicCaptureHandler::Disable() {
  return Response::Success();
}

bool DeterministicCaptureHandler::ModeEnabled() const {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kDeterministicCapture);
}

Response DeterministicCaptureHandler::RequirePageSession() const {
  if (!ModeEnabled()) {
    return Response::ServerError(
        "NotConfigured: browser was not started with --deterministic-capture");
  }
  if (!web_contents_) {
    return Response::ServerError(
        "NotConfigured: this command requires a page target session");
  }
  return Response::Success();
}

CaptureSession* DeterministicCaptureHandler::Session() {
  if (!web_contents_) {
    return nullptr;
  }
  CaptureSession::CreateForWebContents(web_contents_);
  return CaptureSession::FromWebContents(web_contents_);
}

Response DeterministicCaptureHandler::GetCapabilities(
    String* out_protocol_version,
    String* out_patch_version,
    bool* out_mode_enabled,
    std::unique_ptr<protocol::DictionaryValue>* out_chromium,
    std::unique_ptr<protocol::DictionaryValue>* out_build,
    std::unique_ptr<protocol::Array<String>>* out_pixel_formats,
    std::unique_ptr<protocol::Array<String>>* out_color_spaces,
    std::unique_ptr<protocol::Array<String>>* out_frame_sinks,
    std::unique_ptr<protocol::DictionaryValue>* out_audio,
    std::unique_ptr<protocol::Array<String>>* out_hash,
    std::unique_ptr<protocol::Array<DeterministicCapture::Feature>>*
        out_features,
    std::unique_ptr<protocol::DictionaryValue>* out_limits,
    std::unique_ptr<protocol::DictionaryValue>* out_gpu) {
  *out_protocol_version = kProtocolVersion;
  *out_patch_version = kPatchVersion;
  *out_mode_enabled = ModeEnabled();

  auto chromium = std::make_unique<base::DictValue>();
  chromium->Set("version", std::string(version_info::GetVersionNumber()));
  chromium->Set("lastChange", std::string(version_info::GetLastChange()));
  *out_chromium = std::move(chromium);

  auto build = std::make_unique<base::DictValue>();
#if defined(OFFICIAL_BUILD)
  build->Set("official", true);
#else
  build->Set("official", false);
#endif
#if DCHECK_IS_ON()
  build->Set("dcheck", true);
#else
  build->Set("dcheck", false);
#endif
  *out_build = std::move(build);

  *out_pixel_formats = Strings({capture::kCanonicalPixelFormat});
  *out_color_spaces = Strings({"srgb"});
  *out_frame_sinks = Strings({"shm", "file", "null"});

  auto audio = std::make_unique<base::DictValue>();
  base::ListValue sample_formats;
  sample_formats.Append("s16");
  audio->Set("sampleFormats", std::move(sample_formats));
  base::DictValue device_format;
  device_format.Set("rate", 48000);
  device_format.Set("channels", 2);
  device_format.Set("framesPerBuffer", 480);
  audio->Set("deviceFormat", std::move(device_format));
  *out_audio = std::move(audio);

  *out_hash = Strings({capture::kCanonicalHashName});

  // Features appear as the patch series lands them.
  auto features =
      std::make_unique<protocol::Array<DeterministicCapture::Feature>>();
  for (const char* name : {"pipelinedAdvance", "advanceRange", "shmSink", "canonicalBgra"}) {
    features->push_back(DeterministicCapture::Feature::Create()
                            .SetName(name)
                            .SetVersion(1)
                            .Build());
  }
  *out_features = std::move(features);

  auto limits = std::make_unique<base::DictValue>();
  limits->Set("maxTargets", 1);
  limits->Set("maxQueuedFrames", 64);
  *out_limits = std::move(limits);

  auto gpu = std::make_unique<base::DictValue>();
  const gpu::GPUInfo& info =
      content::GpuDataManager::GetInstance()->GetGPUInfo();
  gpu->Set("glRenderer", info.gl_renderer);
  gpu->Set("glVersion", info.gl_version);
  gpu->Set("glVendor", info.gl_vendor);
  *out_gpu = std::move(gpu);
  return Response::Success();
}

void DeterministicCaptureHandler::Configure(
    std::unique_ptr<protocol::DictionaryValue> in_config,
    std::unique_ptr<ConfigureCallback> callback) {
  Response check = RequirePageSession();
  if (!check.IsSuccess()) {
    callback->sendFailure(check);
    return;
  }
  Session()->Configure(
      std::move(*in_config),
      base::BindOnce(
          [](std::unique_ptr<ConfigureCallback> callback,
             std::optional<CaptureSession::Error> error,
             base::DictValue result) {
            if (error) {
              callback->sendFailure(ToResponse(*error));
              return;
            }
            callback->sendSuccess(
                std::make_unique<base::DictValue>(DictOf(result, "effective")),
                result.FindDouble("virtualTimeTicksBase").value_or(0.0));
          },
          std::move(callback)));
}

void DeterministicCaptureHandler::Advance(
    int in_frame_index,
    std::optional<bool> in_capture_video,
    std::optional<bool> in_capture_audio,
    std::optional<bool> in_hash,
    std::unique_ptr<protocol::Array<protocol::DictionaryValue>> in_inputs,
    std::unique_ptr<AdvanceCallback> callback) {
  Response check = RequirePageSession();
  if (!check.IsSuccess()) {
    callback->sendFailure(check);
    return;
  }
  base::DictValue params;
  params.Set("frameIndex", in_frame_index);
  params.Set("captureVideo", in_capture_video.value_or(true));
  params.Set("captureAudio", in_capture_audio.value_or(true));
  params.Set("hash", in_hash.value_or(true));
  Session()->Advance(
      std::move(params),
      base::BindOnce(
          [](std::unique_ptr<AdvanceCallback> callback,
             std::optional<CaptureSession::Error> error,
             base::DictValue r) {
            if (error) {
              callback->sendFailure(ToResponse(*error));
              return;
            }
            auto markers = std::make_unique<protocol::Array<protocol::DictionaryValue>>();
            for (base::Value& v : ListOf(r, "markers")) {
              markers->push_back(std::make_unique<base::DictValue>(std::move(v.GetDict())));
            }
            auto media = std::make_unique<protocol::Array<protocol::DictionaryValue>>();
            for (base::Value& v : ListOf(r, "media")) {
              media->push_back(std::make_unique<base::DictValue>(std::move(v.GetDict())));
            }
            auto completed = std::make_unique<protocol::Array<protocol::DictionaryValue>>();
            for (base::Value& v : ListOf(r, "completed")) {
              completed->push_back(std::make_unique<base::DictValue>(std::move(v.GetDict())));
            }
            auto warnings = std::make_unique<std::vector<String>>();
            for (const base::Value& v : ListOf(r, "warnings")) {
              if (const std::string* s = v.GetIfString()) {
                warnings->push_back(*s);
              }
            }
            const std::string* pcm = r.FindString("pcmFrameHash");
            callback->sendSuccess(
                static_cast<int>(r.FindDouble("frameIndex").value_or(0)),
                r.FindDouble("virtualTimeMs").value_or(0.0),
                r.FindBool("hasSurface").value_or(false),
                r.FindBool("hasDamage").value_or(false),
                r.FindBool("reusedReadback").value_or(false),
                static_cast<int>(r.FindDouble("audioSampleFrames").value_or(0)),
                pcm ? std::optional<String>(*pcm) : std::nullopt,
                std::move(markers), std::move(media),
                std::make_unique<base::DictValue>(DictOf(r, "timings")),
                std::move(completed), std::move(warnings));
          },
          std::move(callback)));
}

void DeterministicCaptureHandler::AdvanceRange(
    int in_from_frame_index,
    int in_to_frame_index,
    std::optional<bool> in_capture_audio,
    std::optional<bool> in_hash,
    std::unique_ptr<AdvanceRangeCallback> callback) {
  Response check = RequirePageSession();
  if (!check.IsSuccess()) {
    callback->sendFailure(check);
    return;
  }
  base::DictValue params;
  params.Set("fromFrameIndex", in_from_frame_index);
  params.Set("toFrameIndex", in_to_frame_index);
  params.Set("captureAudio", in_capture_audio.value_or(false));
  params.Set("hash", in_hash.value_or(false));
  Session()->AdvanceRange(
      std::move(params),
      base::BindOnce(
          [](std::unique_ptr<AdvanceRangeCallback> callback,
             std::optional<CaptureSession::Error> error,
             base::DictValue r) {
            if (error) {
              callback->sendFailure(ToResponse(*error));
              return;
            }
            auto frames = std::make_unique<protocol::Array<protocol::DictionaryValue>>();
            for (base::Value& v : ListOf(r, "frames")) {
              frames->push_back(std::make_unique<base::DictValue>(std::move(v.GetDict())));
            }
            callback->sendSuccess(
                std::move(frames),
                static_cast<int>(r.FindDouble("nextFrameIndex").value_or(0)));
          },
          std::move(callback)));
}

void DeterministicCaptureHandler::Finish(
    std::unique_ptr<FinishCallback> callback) {
  Response check = RequirePageSession();
  if (!check.IsSuccess()) {
    callback->sendFailure(check);
    return;
  }
  Session()->Finish(base::BindOnce(
      [](std::unique_ptr<FinishCallback> callback,
         std::optional<CaptureSession::Error> error, base::DictValue r) {
        if (error) {
          callback->sendFailure(ToResponse(*error));
          return;
        }
        auto completed = std::make_unique<protocol::Array<protocol::DictionaryValue>>();
        for (base::Value& v : ListOf(r, "completed")) {
          completed->push_back(std::make_unique<base::DictValue>(std::move(v.GetDict())));
        }
        const std::string* chain = r.FindString("chainHash");
        const std::string* pcm = r.FindString("pcmHash");
        std::optional<double> expected = r.FindDouble("expectedFrames");
        std::unique_ptr<base::DictValue> first_sink_error;
        if (const base::DictValue* err = r.FindDict("firstSinkError")) {
          first_sink_error = std::make_unique<base::DictValue>(err->Clone());
        }
        callback->sendSuccess(
            *r.FindString("status"),
            static_cast<int>(r.FindDouble("framesWritten").value_or(0)),
            expected ? std::optional<int>(static_cast<int>(*expected)) : std::nullopt,
            static_cast<int>(r.FindDouble("reusedFrames").value_or(0)),
            static_cast<int>(r.FindDouble("audioSampleFrames").value_or(0)),
            chain ? std::optional<String>(*chain) : std::nullopt,
            pcm ? std::optional<String>(*pcm) : std::nullopt,
            std::make_unique<base::DictValue>(DictOf(r, "histograms")),
            r.FindInt("gpuProcessCrashes").value_or(0), std::move(first_sink_error),
            std::move(completed));
      },
      std::move(callback)));
}

Response DeterministicCaptureHandler::Abort() {
  Response check = RequirePageSession();
  if (!check.IsSuccess()) {
    return check;
  }
  Session()->Abort();
  return Response::Success();
}

Response DeterministicCaptureHandler::GetState(
    String* out_state,
    int* out_next_frame_index,
    std::unique_ptr<protocol::DictionaryValue>* out_last_error) {
  Response check = RequirePageSession();
  if (!check.IsSuccess()) {
    return check;
  }
  base::DictValue state = Session()->GetState();
  *out_state = *state.FindString("state");
  *out_next_frame_index = static_cast<int>(state.FindDouble("nextFrameIndex").value_or(0));
  if (const base::DictValue* err = state.FindDict("lastError")) {
    *out_last_error = std::make_unique<base::DictValue>(err->Clone());
  }
  return Response::Success();
}

}  // namespace headless::protocol
