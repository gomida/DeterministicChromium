// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_BROWSER_CAPTURE_CAPTURE_SESSION_H_
#define HEADLESS_LIB_BROWSER_CAPTURE_CAPTURE_SESSION_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/web_contents_user_data.h"
#include "headless/lib/browser/capture/raw_frame_sink.h"
#include "headless/lib/browser/capture/virtual_time_owner.h"
#include "headless/public/mojom/capture_agent.mojom.h"
#include "mojo/public/cpp/bindings/associated_remote.h"

class SkBitmap;

namespace headless {
class HeadlessWebContentsImpl;
}

namespace headless::capture {

// Per-target state of a deterministic capture (DESIGN.md 4.2-4.3). Lives on
// the UI thread as WebContents user data; conversion, hashing and sink
// writes run on a blocking sequence so the UI thread never waits on the
// consumer.
class CaptureSession : public content::WebContentsUserData<CaptureSession> {
 public:
  enum class State {
    kUnconfigured,
    kConfigured,
    kCapturing,
    kFinished,
    kFailed,
    kAborted,
  };

  // Typed error with the same vocabulary as the controller (DESIGN.md 4.3).
  struct Error {
    std::string kind;
    std::string message;
    base::DictValue details;

    Error() = default;
    Error(std::string kind_in,
          std::string message_in,
          base::DictValue details_in = base::DictValue())
        : kind(std::move(kind_in)),
          message(std::move(message_in)),
          details(std::move(details_in)) {}
    // base::DictValue is move-only; errors are copied into callbacks and
    // kept as the sticky last error, so copy explicitly through Clone().
    Error(const Error& other)
        : kind(other.kind),
          message(other.message),
          details(other.details.Clone()) {}
    Error& operator=(const Error& other) {
      kind = other.kind;
      message = other.message;
      details = other.details.Clone();
      return *this;
    }
    Error(Error&&) = default;
    Error& operator=(Error&&) = default;

    std::string Render() const;  // "Kind: message" for Response::ServerError
  };

  using ResultCallback = base::OnceCallback<void(std::optional<Error>, base::DictValue)>;

  ~CaptureSession() override;

  void Configure(base::DictValue config, ResultCallback callback);
  void Advance(base::DictValue params, ResultCallback callback);
  void AdvanceRange(base::DictValue params, ResultCallback callback);
  void Finish(ResultCallback callback);
  void Abort();
  base::DictValue GetState() const;

  State state() const { return state_; }
  bool OwnsVirtualTime() const { return state_ != State::kUnconfigured; }
  bool IsOwnerClient(const content::DevToolsAgentHostClient* client) const {
    return virtual_time_ && client == virtual_time_.get();
  }

  static const char* StateName(State state);

 private:
  friend class content::WebContentsUserData<CaptureSession>;
  struct FrameConfig {
    int64_t num = 60;
    int64_t den = 1;
  };
  struct Processed {
    int64_t frame_index;
    std::string frame_hash;
    std::string chain_hash;
    std::optional<SinkError> sink_error;
    base::TimeDelta hash;
    base::TimeDelta sink;
    uint32_t audio_frames_written = 0;
  };
  struct PendingAdvance {
    int64_t frame_index;
    bool capture_video;
    bool capture_audio;
    bool hash;
    base::TimeTicks started;
    base::TimeTicks after_grant;
    base::TimeTicks after_prepare;
    base::TimeTicks after_begin_frame;
    // Filled by the renderer agent (DESIGN.md 4.3 steps 2-5).
    base::ListValue markers;
    base::ListValue media;
    base::ListValue warnings;
    double wake_ms = 0;
    double quiesce_ms = 0;
    double audio_ms = 0;
    // Audio of the interval (DESIGN.md 4.6): bytes in the configured sample
    // format, their hash, and the frame count the renderer delivered.
    std::shared_ptr<const std::vector<uint8_t>> audio;
    uint32_t audio_frames = 0;
    std::string pcm_frame_hash;
    // Set when the surface did not exist before this frame's BeginFrame and a
    // second BeginFrame at the same frame time was issued to obtain readback
    // (only ever the first frame; DESIGN.md 4.3 step 8).
    bool retried_for_surface = false;
    ResultCallback callback;
  };

  explicit CaptureSession(content::WebContents* web_contents);

  base::TimeTicks FrameTicks(int64_t frame_index) const;
  int64_t FrameOffsetMicros(int64_t frame_index) const;
  // Device sample frames belonging to frames [0, frame_index): floor(i*rate*den/num).
  int64_t AudioSamplesThrough(int64_t frame_index) const;
  void Fail(Error error);
  std::optional<Error> CheckState(const char* command) const;

  // Configure steps.
  void OnSinkOpened(base::DictValue config, ResultCallback callback, std::optional<SinkError> error);
  void OnVirtualTimeEnabled(base::DictValue effective, ResultCallback callback,
                            std::optional<std::string> error, base::DictValue result);

  // Advance steps (DESIGN.md 4.3, steps 1, 6, 7, 8; steps 2-5 land with the
  // renderer agent).
  void RunAdvance(std::unique_ptr<PendingAdvance> pending);
  void OnGranted(std::unique_ptr<PendingAdvance> pending, std::optional<std::string> error);
  // Steps 2-5 through the renderer's CaptureAgent. The pending advance is
  // held in prepare_pending_ while the renderer works so a timeout or a
  // renderer exit can still answer the controller.
  void PrepareFrame(std::unique_ptr<PendingAdvance> pending);
  void OnPrepared(int64_t frame_index, mojom::PrepareResultPtr result);
  void OnPrepareTimeout();
  void OnAgentDisconnected();
  void IssueBeginFrame(std::unique_ptr<PendingAdvance> pending);
  void OnFrameFinished(std::unique_ptr<PendingAdvance> pending, bool has_damage,
                       std::unique_ptr<SkBitmap> bitmap, std::string error);
  void OnFrameProcessed(Processed processed);
  void Respond(std::unique_ptr<PendingAdvance> pending, bool has_damage, bool has_surface, bool reused);
  void MaybeRespondDeferred();
  base::ListValue TakeCompleted();

  // AdvanceRange drives RunAdvance in a loop.
  void RangeStep(base::DictValue params, int64_t to_frame, base::ListValue frames, ResultCallback callback,
                 std::optional<Error> error, base::DictValue frame_result);

  void OnDrained(ResultCallback callback);

  raw_ptr<content::WebContents> web_contents_;
  State state_ = State::kUnconfigured;
  std::optional<Error> last_error_;
  base::DictValue effective_config_;
  FrameConfig frame_rate_;
  double virtual_time_ticks_base_ms_ = 0.0;
  int64_t next_frame_ = 0;
  int64_t expected_frames_ = -1;
  int max_queued_frames_ = 8;
  base::TimeDelta stall_timeout_ = base::Seconds(60);
  int width_ = 0;
  int height_ = 0;

  std::unique_ptr<VirtualTimeOwner> virtual_time_;
  mojo::AssociatedRemote<mojom::CaptureAgent> agent_;
  content::GlobalRenderFrameHostId agent_frame_id_;
  std::unique_ptr<PendingAdvance> prepare_pending_;
  base::OneShotTimer prepare_timer_;
  std::string marker_prefix_ = "dcap:";
  bool collect_marks_ = true;
  // Audio device format and sample format of the PCM handed to the sink.
  int audio_rate_ = 48000;
  int audio_channels_ = 2;
  bool audio_s16_ = true;
  int64_t audio_sample_frames_ = 0;  // captured (written) so far
  std::string pcm_chain_hash_;
  double media_wait_timeout_ms_ = 10000;
  bool fail_on_media_wait_timeout_ = true;
  bool fail_on_unsupported_api_ = true;
  std::unique_ptr<RawFrameSink> sink_;
  bool sink_wants_frames_ = false;
  scoped_refptr<base::SequencedTaskRunner> worker_;

  // Pipelining: frames whose conversion is in flight, and processed results
  // not yet reported to the controller.
  int inflight_ = 0;
  std::vector<Processed> completed_;
  std::string chain_hash_;
  std::shared_ptr<const SkBitmap> cached_bitmap_;
  int64_t cached_bitmap_frame_ = -1;
  std::unique_ptr<PendingAdvance> deferred_;   // waiting for queue space
  bool deferred_has_damage_ = false;
  bool deferred_has_surface_ = false;
  bool deferred_reused_ = false;
  base::DictValue last_response_;
  int64_t last_response_frame_ = -1;
  int64_t frames_written_ = 0;
  int64_t reused_frames_ = 0;
  int gpu_process_crashes_ = 0;
  std::optional<SinkError> first_sink_error_;
  std::vector<double> grant_ms_;
  std::vector<double> prepare_ms_;      // grant to agent reply (wake + quiesce + audio + marks)
  std::vector<double> wake_ms_;
  std::vector<double> quiesce_ms_;
  std::vector<double> begin_frame_ms_;  // prepare to frame-finished; includes the GPU readback
  std::vector<double> hash_ms_;
  std::vector<double> sink_ms_;

  base::WeakPtrFactory<CaptureSession> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace headless::capture

#endif  // HEADLESS_LIB_BROWSER_CAPTURE_CAPTURE_SESSION_H_
