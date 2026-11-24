// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/renderer/capture_agent.h"

#include "headless/lib/renderer/capture_audio.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/barrier_closure.h"
#include "base/functional/callback_helpers.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/string_escape.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time_override.h"
#include "content/public/common/isolated_world_ids.h"
#include "third_party/blink/public/common/use_counter/use_counter_feature.h"
#include "third_party/blink/public/mojom/use_counter/metrics/web_feature.mojom.h"
#include "gin/converter.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/platform/media/web_capture_media_registry.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "v8/include/v8.h"

namespace headless {

namespace {

// Isolated world used to read the performance timeline without touching the
// page's own JavaScript state. Content reserves ids up to
// ISOLATED_WORLD_ID_CONTENT_END; embedders may use the ten above it.
constexpr int32_t kCaptureWorldId = content::ISOLATED_WORLD_ID_CONTENT_END + 1;

// Returns a flat JSON array [name, startTime, age, name, startTime, age, ...]
// of the marks with the prefix. `age` is performance.now() - startTime at the
// moment of collection: with virtual time paused at T_n, that is how far
// before T_n the mark was made.
constexpr char kCollectMarksScript[] =
    "(function(prefix){"
    "const now=performance.now();const out=[];"
    "for(const e of performance.getEntriesByType('mark')){"
    "if(e.name.startsWith(prefix)){out.push(e.name,e.startTime,now-e.startTime);}}"
    "return JSON.stringify(out);})(";

}  // namespace

CaptureAgent::CaptureAgent(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {
  render_frame->GetAssociatedInterfaceRegistry()->AddInterface<mojom::CaptureAgent>(
      base::BindRepeating(&CaptureAgent::BindReceiver, weak_factory_.GetWeakPtr()));
}

CaptureAgent::~CaptureAgent() = default;

void CaptureAgent::OnDestruct() {
  delete this;
}

void CaptureAgent::DidCreateNewDocument() {
  reported_marks_.clear();
  unsupported_reported_.clear();
}

void CaptureAgent::DidObserveNewFeatureUsage(const blink::UseCounterFeature& feature) {
  // Use counters for APIs whose results depend on real devices, real time or
  // other processes (DESIGN.md 4.9). The names are the contract's vocabulary.
  struct Entry {
    blink::mojom::WebFeature feature;
    const char* api;
  };
  static constexpr Entry kUnsupported[] = {
    {blink::mojom::WebFeature::kSharedWorkerStart, "SharedWorker"},
    {blink::mojom::WebFeature::kEncryptedMediaSecureOrigin, "EncryptedMedia"},
    {blink::mojom::WebFeature::kServiceWorkerControlledPage, "ServiceWorker"},
    {blink::mojom::WebFeature::kRTCPeerConnectionConstructorConstraints, "RTCPeerConnection"},
    {blink::mojom::WebFeature::kRTCPeerConnectionConstructorCompliant, "RTCPeerConnection"},
    {blink::mojom::WebFeature::kGetUserMediaPrefixed, "getUserMedia"},
    {blink::mojom::WebFeature::kGetUserMediaLegacy, "getUserMedia"},
    {blink::mojom::WebFeature::kGetUserMediaPromise, "getUserMedia"},
    {blink::mojom::WebFeature::kGetDisplayMedia, "getDisplayMedia"},
    {blink::mojom::WebFeature::kCanvasCaptureStream, "captureStream"},
    {blink::mojom::WebFeature::kMediaRecorder_MimeType, "MediaRecorder"},
    {blink::mojom::WebFeature::kMediaRecorder_VideoBitsPerSecond, "MediaRecorder"},
    {blink::mojom::WebFeature::kMediaRecorder_AudioBitsPerSecond, "MediaRecorder"},
    {blink::mojom::WebFeature::kWebCodecsAudioDecoder, "WebCodecs"},
    {blink::mojom::WebFeature::kWebCodecsVideoDecoder, "WebCodecs"},
    {blink::mojom::WebFeature::kWebCodecsVideoEncoder, "WebCodecs"},
    {blink::mojom::WebFeature::kWebCodecsVideoTrackReader, "WebCodecs"},
    {blink::mojom::WebFeature::kHTMLMediaElementSetSinkId, "setSinkId"},
    {blink::mojom::WebFeature::kWebGPURequestAdapter, "WebGPU"},
    {blink::mojom::WebFeature::kAudioContextSetSinkId, "setSinkId"},
  };
  if (feature.type() != blink::mojom::UseCounterFeatureType::kWebFeature) {
    return;
  }
  for (const Entry& entry : kUnsupported) {
    if (static_cast<uint32_t>(entry.feature) == feature.value() &&
        unsupported_reported_.insert(entry.api).second) {
      unsupported_pending_.push_back(entry.api);
    }
  }
}

void CaptureAgent::BindReceiver(
    mojo::PendingAssociatedReceiver<mojom::CaptureAgent> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void CaptureAgent::Prepare(mojom::PrepareParamsPtr params, PrepareCallback callback) {
  auto pending = std::make_unique<Pending>();
  pending->params = std::move(params);
  pending->callback = std::move(callback);
  pending->started = base::subtle::TimeTicksNowIgnoringOverride();

  // Durations are measured with the real clock: base::TimeTicks::Now() in a
  // capture renderer is virtual time, which is paused here.
  // Step 2 (wake): a round trip through the media thread and the compositor
  // thread runs every task already due there before we go on, so work that
  // became due during the grant does not land at a real-time-dependent later
  // moment.
  std::vector<scoped_refptr<base::SequencedTaskRunner>> runners;
  if (auto media = blink::Platform::Current()->MediaThreadTaskRunner()) {
    runners.push_back(std::move(media));
  }
  if (auto compositor = blink::Platform::Current()->CompositorThreadTaskRunner()) {
    runners.push_back(std::move(compositor));
  }
  if (runners.empty()) {
    OnWoken(std::move(pending));
    return;
  }
  Pending* raw = pending.get();
  base::RepeatingClosure barrier = base::BarrierClosure(
      runners.size(),
      base::BindOnce(&CaptureAgent::OnWoken, weak_factory_.GetWeakPtr(), std::move(pending)));
  (void)raw;
  for (auto& runner : runners) {
    runner->PostTaskAndReply(FROM_HERE, base::DoNothing(), barrier);
  }
}

void CaptureAgent::OnWoken(std::unique_ptr<Pending> pending) {
  pending->woken = base::subtle::TimeTicksNowIgnoringOverride();
  Quiesce(std::move(pending));
}

void CaptureAgent::Quiesce(std::unique_ptr<Pending> pending) {
  // Step 3: wait until no player has a pipeline operation in flight and no
  // play, pause, seek or rate change happened since the previous snapshot.
  // Each round is a media-thread round trip (play and rate changes have no
  // acknowledgement, so the trip is what makes them land). The main thread is
  // never blocked.
  blink::WebCaptureMediaSnapshot snapshot = blink::WebCaptureMediaRegistry::Snapshot();
  const bool all_stable = std::all_of(snapshot.players.begin(), snapshot.players.end(),
                                      [](const blink::WebCaptureMediaState& p) { return p.stable; });
  const bool settled = all_stable && pending->rounds > 0 &&
                       snapshot.transitions == pending->last_transitions;
  pending->last_transitions = snapshot.transitions;
  const double waited_ms =
      (base::subtle::TimeTicksNowIgnoringOverride() - pending->woken).InMillisecondsF();
  const bool timed_out = !settled && waited_ms >= pending->params->wait_timeout_ms;
  if (snapshot.players.empty() || settled || timed_out) {
    pending->quiesced = base::subtle::TimeTicksNowIgnoringOverride();
    auto result = mojom::PrepareResult::New();
    for (const blink::WebCaptureMediaState& p : snapshot.players) {
      auto state = mojom::CaptureMediaState::New();
      state->player_id = p.player_id;
      state->state = p.state;
      state->media_time_ms = p.media_time_ms;
      state->stable = p.stable;
      state->has_video = p.has_video;
      state->has_audio = p.has_audio;
      result->media.push_back(std::move(state));
      if (timed_out && !p.stable) {
        result->timed_out_players.push_back(p.player_id);
      }
    }
    RenderAudio(std::move(pending), std::move(result));
    return;
  }
  ++pending->rounds;
  scoped_refptr<base::SequencedTaskRunner> media = blink::Platform::Current()->MediaThreadTaskRunner();
  if (!media) {
    media = base::SequencedTaskRunner::GetCurrentDefault();
  }
  // A short real-time pause between rounds keeps a long decoder start from
  // turning into a busy loop; the media thread does the work meanwhile.
  base::OnceClosure next =
      base::BindOnce(&CaptureAgent::Quiesce, weak_factory_.GetWeakPtr(), std::move(pending));
  media->PostTaskAndReply(FROM_HERE, base::BindOnce([] {
                            base::PlatformThread::Sleep(base::Microseconds(250));
                          }),
                          std::move(next));
}

void CaptureAgent::RenderAudio(std::unique_ptr<Pending> pending, mojom::PrepareResultPtr result) {
  // Step 4: pull every capture sink through S(n+1) frames on the capture
  // audio thread and take the interval's mix.
  if (!pending->params->render_audio) {
    FlushMediaEvents(std::move(pending), std::move(result), 0);
    return;
  }
  const int64_t through = pending->params->audio_frames_through;
  CaptureAudioEngine::Get()->RenderThrough(
      through, base::BindOnce(&CaptureAgent::OnAudioRendered, weak_factory_.GetWeakPtr(),
                              std::move(pending), std::move(result)));
}

void CaptureAgent::OnAudioRendered(std::unique_ptr<Pending> pending,
                                   mojom::PrepareResultPtr result,
                                   std::vector<float> pcm,
                                   uint32_t frames,
                                   int sinks) {
  const CaptureAudioFormat& format = CaptureAudioEngine::Get()->format();
  result->pcm = std::move(pcm);
  result->audio_frames = frames;
  result->audio_channels = format.channels;
  result->audio_rate = format.rate;
  result->audio_sinks = sinks;
  for (const std::string& api : CaptureAudioEngine::Get()->TakeUnsupported()) {
    result->warnings.push_back("UnsupportedApiUsed: " + api);
  }
  for (const std::string& api : unsupported_pending_) {
    result->warnings.push_back("UnsupportedApiUsed: " + api);
  }
  unsupported_pending_.clear();
  FlushMediaEvents(std::move(pending), std::move(result), 0);
}

void CaptureAgent::FlushMediaEvents(std::unique_ptr<Pending> pending,
                                    mojom::PrepareResultPtr result,
                                    int stage) {
  // Step 4: media element events (canplay, playing, timeupdate, ended) are
  // queued as ordinary tasks; the BeginFrame arrives on the compositor queue
  // and may overtake them. Tasks of equal priority run in posting order, so a
  // round trip through the media queues guarantees everything queued before
  // it has run before the browser issues the BeginFrame.
  static constexpr blink::TaskType kQueues[] = {blink::TaskType::kMediaElementEvent,
                                                blink::TaskType::kInternalMedia};
  content::RenderFrame* frame = render_frame();
  if (!frame || stage >= static_cast<int>(std::size(kQueues))) {
    Complete(std::move(pending), std::move(result));
    return;
  }
  frame->GetTaskRunner(kQueues[stage])->PostTask(
      FROM_HERE, base::BindOnce(&CaptureAgent::FlushMediaEvents, weak_factory_.GetWeakPtr(),
                                std::move(pending), std::move(result), stage + 1));
}

void CaptureAgent::Complete(std::unique_ptr<Pending> pending, mojom::PrepareResultPtr result) {
  if (pending->params->collect_marks) {
    CollectMarks(*pending->params, result.get());
  }
  const base::TimeTicks done = base::subtle::TimeTicksNowIgnoringOverride();
  result->wake_ms = (pending->woken - pending->started).InMillisecondsF();
  result->quiesce_ms = (pending->quiesced - pending->woken).InMillisecondsF();
  result->audio_ms = (done - pending->quiesced).InMillisecondsF();
  std::move(pending->callback).Run(std::move(result));
}

void CaptureAgent::CollectMarks(const mojom::PrepareParams& params, mojom::PrepareResult* result) {
  content::RenderFrame* frame = render_frame();
  if (!frame || !frame->GetWebFrame()) {
    result->warnings.push_back("markers: no frame");
    return;
  }
  blink::WebLocalFrame* web_frame = frame->GetWebFrame();
  v8::Isolate* isolate = web_frame->GetAgentGroupScheduler()->Isolate();
  v8::HandleScope handle_scope(isolate);
  const std::string source = std::string(kCollectMarksScript) +
                             base::GetQuotedJSONString(params.marker_prefix) + ")";
  v8::Local<v8::Value> value = web_frame->ExecuteScriptInIsolatedWorldAndReturnValue(
      kCaptureWorldId, blink::WebScriptSource(blink::WebString::FromUtf8(source)),
      blink::BackForwardCacheAware::kAllow);
  if (value.IsEmpty() || !value->IsString()) {
    result->warnings.push_back("markers: performance timeline unavailable");
    return;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(gin::V8ToString(isolate, value), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    result->warnings.push_back("markers: unreadable result");
    return;
  }
  const base::ListValue& flat = parsed->GetList();
  std::map<std::pair<std::string, double>, int> seen;
  for (size_t i = 0; i + 2 < flat.size(); i += 3) {
    const std::string* name = flat[i].GetIfString();
    const bool numeric = (flat[i + 1].is_double() || flat[i + 1].is_int()) &&
                         (flat[i + 2].is_double() || flat[i + 2].is_int());
    if (!name || !numeric) {
      continue;
    }
    const double start = flat[i + 1].GetDouble();
    const double age = flat[i + 2].GetDouble();
    const std::pair<std::string, double> key(*name, start);
    int& count = seen[key];
    ++count;
    if (count <= reported_marks_[key]) {
      continue;  // already reported on an earlier frame
    }
    auto marker = mojom::CaptureMarker::New();
    marker->name = *name;
    marker->source = "performanceMark";
    marker->age_ms = age;
    result->markers.push_back(std::move(marker));
  }
  // Remember what has been reported; forget marks the page cleared.
  reported_marks_ = std::move(seen);
}

}  // namespace headless
