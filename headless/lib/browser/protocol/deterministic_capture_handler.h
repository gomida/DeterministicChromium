// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_BROWSER_PROTOCOL_DETERMINISTIC_CAPTURE_HANDLER_H_
#define HEADLESS_LIB_BROWSER_PROTOCOL_DETERMINISTIC_CAPTURE_HANDLER_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "headless/lib/browser/protocol/deterministic_capture.h"
#include "headless/lib/browser/protocol/domain_handler.h"

namespace content {
class WebContents;
}

namespace headless {
class HeadlessBrowserImpl;
namespace capture {
class CaptureSession;
}

namespace protocol {

// DevTools backend for the DeterministicCapture domain. One instance per
// DevTools session; `web_contents_` is null on the browser session, where only
// getCapabilities is available.
class DeterministicCaptureHandler : public DomainHandler,
                                    public DeterministicCapture::Backend {
 public:
  DeterministicCaptureHandler(HeadlessBrowserImpl* browser,
                              content::WebContents* web_contents);
  DeterministicCaptureHandler(const DeterministicCaptureHandler&) = delete;
  DeterministicCaptureHandler& operator=(const DeterministicCaptureHandler&) =
      delete;
  ~DeterministicCaptureHandler() override;

  // DomainHandler implementation.
  void Wire(UberDispatcher* dispatcher) override;
  Response Disable() override;

  // DeterministicCapture::Backend implementation.
  Response GetCapabilities(
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
      std::unique_ptr<protocol::DictionaryValue>* out_gpu) override;
  void Configure(std::unique_ptr<protocol::DictionaryValue> in_config,
                 std::unique_ptr<ConfigureCallback> callback) override;
  void Advance(int in_frame_index,
               std::optional<bool> in_capture_video,
               std::optional<bool> in_capture_audio,
               std::optional<bool> in_hash,
               std::unique_ptr<protocol::Array<protocol::DictionaryValue>>
                   in_inputs,
               std::unique_ptr<AdvanceCallback> callback) override;
  void AdvanceRange(int in_from_frame_index,
                    int in_to_frame_index,
                    std::optional<bool> in_capture_audio,
                    std::optional<bool> in_hash,
                    std::unique_ptr<AdvanceRangeCallback> callback) override;
  void Finish(std::unique_ptr<FinishCallback> callback) override;
  Response Abort() override;
  Response GetState(
      String* out_state,
      int* out_next_frame_index,
      std::unique_ptr<protocol::DictionaryValue>* out_last_error) override;

  static constexpr char kProtocolVersion[] = "2.1";
  static constexpr char kPatchVersion[] = "2.1.0-p5";

 private:
  bool ModeEnabled() const;
  Response RequirePageSession() const;
  capture::CaptureSession* Session();

  raw_ptr<HeadlessBrowserImpl> browser_;
  raw_ptr<content::WebContents> web_contents_;
  std::unique_ptr<DeterministicCapture::Frontend> frontend_;
};

}  // namespace protocol
}  // namespace headless

#endif  // HEADLESS_LIB_BROWSER_PROTOCOL_DETERMINISTIC_CAPTURE_HANDLER_H_
