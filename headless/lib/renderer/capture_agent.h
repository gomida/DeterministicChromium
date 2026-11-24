// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_LIB_RENDERER_CAPTURE_AGENT_H_
#define HEADLESS_LIB_RENDERER_CAPTURE_AGENT_H_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "headless/public/mojom/capture_agent.mojom.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"

namespace headless {

// Renderer side of DESIGN.md 4.3 steps 2-5, one per RenderFrame in a
// --deterministic-capture renderer. Owned by the RenderFrame (deletes itself
// in OnDestruct).
class CaptureAgent : public content::RenderFrameObserver,
                     public mojom::CaptureAgent {
 public:
  explicit CaptureAgent(content::RenderFrame* render_frame);
  CaptureAgent(const CaptureAgent&) = delete;
  CaptureAgent& operator=(const CaptureAgent&) = delete;
  ~CaptureAgent() override;

  // content::RenderFrameObserver:
  void OnDestruct() override;
  void DidCreateNewDocument() override;
  void DidObserveNewFeatureUsage(const blink::UseCounterFeature& feature) override;

  // mojom::CaptureAgent:
  void Prepare(mojom::PrepareParamsPtr params, PrepareCallback callback) override;

 private:
  struct Pending {
    mojom::PrepareParamsPtr params;
    PrepareCallback callback;
    base::TimeTicks started;
    base::TimeTicks woken;
    base::TimeTicks quiesced;
    int rounds = 0;
    uint64_t last_transitions = 0;
  };

  void BindReceiver(mojo::PendingAssociatedReceiver<mojom::CaptureAgent> receiver);
  void OnWoken(std::unique_ptr<Pending> pending);
  void Quiesce(std::unique_ptr<Pending> pending);
  void RenderAudio(std::unique_ptr<Pending> pending, mojom::PrepareResultPtr result);
  void OnAudioRendered(std::unique_ptr<Pending> pending, mojom::PrepareResultPtr result,
                       std::vector<float> pcm, uint32_t frames, int sinks);
  void FlushMediaEvents(std::unique_ptr<Pending> pending, mojom::PrepareResultPtr result, int stage);
  void Complete(std::unique_ptr<Pending> pending, mojom::PrepareResultPtr result);
  void CollectMarks(const mojom::PrepareParams& params, mojom::PrepareResult* result);

  mojo::AssociatedReceiver<mojom::CaptureAgent> receiver_{this};
  // (name, startTime) -> occurrences already reported, so each mark is
  // reported once even though performance.getEntriesByType returns all.
  std::map<std::pair<std::string, double>, int> reported_marks_;
  // APIs outside the capture contract seen since the last Prepare, and the
  // ones already reported once for this document (DESIGN.md 4.9).
  std::vector<std::string> unsupported_pending_;
  std::set<std::string> unsupported_reported_;
  base::WeakPtrFactory<CaptureAgent> weak_factory_{this};
};

}  // namespace headless

#endif  // HEADLESS_LIB_RENDERER_CAPTURE_AGENT_H_
