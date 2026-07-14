// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/fake_audio_output_stream.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "media/audio/audio_manager_base.h"
#include "media/audio/deterministic_audio_dump.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_sample_types.h"

namespace media {

// static
AudioOutputStream* FakeAudioOutputStream::MakeFakeStream(
    AudioManagerBase* manager,
    const AudioParameters& params) {
  return new FakeAudioOutputStream(manager, params);
}

FakeAudioOutputStream::FakeAudioOutputStream(AudioManagerBase* manager,
                                             const AudioParameters& params)
    : audio_manager_(manager),
      params_(params),
      worker_task_runner_(manager->GetWorkerTaskRunner()),
      deterministic_audio_pump_(
          base::MakeRefCounted<DeterministicAudioPump>(worker_task_runner_)),
      fixed_data_delay_(FakeAudioWorker::ComputeFakeOutputDelay(params)),
      callback_(nullptr),
      fake_worker_(manager->GetWorkerTaskRunner(), params),
      audio_bus_(AudioBus::Create(params)) {}

FakeAudioOutputStream::~FakeAudioOutputStream() {
  DCHECK(!callback_);
}

bool FakeAudioOutputStream::Open() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  audio_bus_->Zero();
  return true;
}

void FakeAudioOutputStream::Start(AudioSourceCallback* callback) {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  callback_ = callback;
  if (auto* dump = DeterministicAudioDump::GetIfEnabled()) {
    deterministic_audio_pump_->Activate(this);
    dump->RegisterStream(deterministic_audio_pump_, params_);
    return;
  }
  fake_worker_.Start(base::BindRepeating(&FakeAudioOutputStream::CallOnMoreData,
                                         base::Unretained(this)));
}

void FakeAudioOutputStream::Stop() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  if (auto* dump = DeterministicAudioDump::GetIfEnabled()) {
    dump->UnregisterStream(deterministic_audio_pump_.get());
    deterministic_audio_pump_->Deactivate(this);
    callback_ = nullptr;
    return;
  }
  fake_worker_.Stop();
  callback_ = nullptr;
}

void FakeAudioOutputStream::Close() {
  DCHECK(!callback_);
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  audio_manager_->ReleaseOutputStream(this);
}

void FakeAudioOutputStream::Flush() {}

void FakeAudioOutputStream::SetVolume(double volume) {}

void FakeAudioOutputStream::GetVolume(double* volume) {
  *volume = 0;
}

void FakeAudioOutputStream::CallOnMoreData(base::TimeTicks ideal_time,
                                           base::TimeTicks now) {
  DCHECK(audio_manager_->GetWorkerTaskRunner()->BelongsToCurrentThread());
  // Real streams provide small tweaks to their delay values, alongside the
  // current system time; and so the same is done here.
  const auto delay =
      fixed_data_delay_ + std::max(base::TimeDelta(), ideal_time - now);
  callback_->OnMoreData(delay, now, {}, audio_bus_.get());
}

void FakeAudioOutputStream::PumpForDeterministicCapture(
    std::vector<int16_t>* samples) {
  DCHECK(worker_task_runner_->RunsTasksInCurrentSequence());
  DCHECK(callback_);
  // The audio source uses delay_timestamp to estimate actual device latency.
  // A begin-frame timestamp can run much faster than wall time, so exposing it
  // here makes HTMLMediaElement playback treat decoded data as underflowed.
  // There is also no hardware queue in dump mode, so report zero output delay.
  // Sample progression remains driven solely by the number of pulls.
  const base::TimeTicks now = base::TimeTicks::Now();
  callback_->OnMoreData(base::TimeDelta(), now, {}, audio_bus_.get());
  samples->resize(audio_bus_->frames() * audio_bus_->channels());
  audio_bus_->ToInterleaved<SignedInt16SampleTypeTraits>(audio_bus_->frames(),
                                                         samples->data());
}

void FakeAudioOutputStream::SetMute(bool muted) {}

}  // namespace media
