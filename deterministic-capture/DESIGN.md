# Deterministic Capture v2: general-purpose design

Status: draft v2.1, 2026-11-24. Supersedes drafts v1 and v2 and the V0.22
presentation-specific pipeline described in `README.md`. Nothing here has been
built; measurements are cited where they exist. Section 12 records the reviews
that shaped this draft and the disagreements settled against source. Claims
marked *(verify)* are scheduled for a prototype.

## 1. Problem statement

Render web content into video such that the same input produces the same raw
frames and the same PCM on every run of the same browser build, GPU, driver,
Mesa version, fonts, flags, profile seed and network inputs, within the
browser's viewport limits and even output dimensions, with native
`<audio>`/`<video>` playback following the capture clock, GPU rasterization,
raw pixel transport to an external encoder, and chunked parallel rendering
whose output is verified equal to a serial render at the sampled frames
(exhaustively under `full` verification).

V0.22 solved this for one page shape (an `index.html` with an
`audio_manifest.json`, a synthetic click, a single `HTMLAudioElement`, pages
advancing on native `ended`). That becomes a scenario plugin; the engine knows
nothing of it.

### 1.1 Goals

| Id | Goal |
| --- | --- |
| G1 | Any URL whose observable timing depends on work the engine pins: main-thread tasks under virtual time, compositor frames, the media pipeline, and the asynchronous completions listed in 4.8. The APIs listed in 4.9 are detected and reported; others are not guaranteed |
| G2 | One timebase, renderer virtual time, from which page timers, rAF, animations, compositor frame time, media time and audio output derive |
| G3 | Native media through Chromium's demuxers and software decoders; frame selection and PCM are functions of capture time only, independent of decode speed |
| G4 | RGBA readback converted to NV12 BT.709 limited for an external encoder over a FIFO, with a per-frame hash of the canonical RGBA bitmap |
| G5 | PCM of all audio output sinks of the page, mixed deterministically, one file per capture |
| G6 | Chunked parallel rendering with `overlap`, `boundaries`, `full` and `repeat` verification |
| G7 | Capability negotiation; the controller refuses a browser whose protocol major or features do not match |
| G8 | Per-capture configuration over CDP; one mode switch on the command line |
| G9 | Linux x64; ANGLE GL over Mesa D3D12 on WSL2 and ANGLE Vulkan on native Linux; SwiftShader for functional tests only, never selected automatically |

### 1.2 Non-goals for v2

Real-time capture; MediaStream, WebRTC, `getUserMedia`, `captureStream`,
`MediaRecorder`; EME; hardware video decoders; WebGPU asynchronous paths;
shared and service workers; cross-process `postMessage` and
`BroadcastChannel`; HDR and 10-bit output; more than one capturing target per
browser; byte identity with V0.22 output; cross-machine or cross-backend pixel
identity; arm64 (deferred); network archive replay (deferred; `offline`
means local `file://` or a local HTTP server).

### 1.3 Trust boundary

Capture runs with `--no-sandbox`, `--disable-gpu-sandbox` and a browser that
trails Stable by weeks. Content must be trusted. The controller refuses
`network: allow` for non-local URLs unless `trusted: true` is set.

## 2. Findings from V0.22 and from source that shape v2

Source: `capture.py` at `dev-doc` 1ece89feac; browser patches at
`291e6fd347` (audio, raw frames) and `629f30c22d` (native video); Chromium
150.0.7871.46 and the 152 snapshot for line references (section 12).

1. **Renderer virtual time is process-wide, but only the main thread advances
   it.** `AutoAdvancingVirtualTimeDomain` installs
   `base::subtle::ScopedTimeClockOverrides` through
   `ProcessTimeOverrideCoordinator` (`process_time_override_coordinator.cc:82`),
   swapping the process-global `g_time_ticks_now_function`. `TimeTicks::Now()`
   and `Time::Now()` return virtual values on every renderer thread, including
   threads created earlier. Delayed tasks on non-main threads compute their
   wake-up from virtual "now" and therefore never run early, but those threads
   are not coordinator clients: when virtual time jumps they are not woken, so
   a 250 ms task runs at that thread's next real wake-up, at a virtual time that
   depends on real speed (`message_pump_default.cc:95`,
   `process_time_override_coordinator.cc:129-133`). `ConditionVariable` and
   `WaitableEvent` timed waits use real time. The `TimeTicks` base of virtual
   time is the real clock at enable time; `initialVirtualTime` only sets the
   `Date` base and is ignored if virtual time was already enabled.
2. **Media with an audio track is clocked by output pulls.** `RendererImpl`
   uses `AudioRendererImpl::GetTimeSource()` when an audio stream exists
   (`renderer_impl.cc:505-510`); `audio_clock_` advances only in `Render`, and
   with `delay=0` the interpolation is capped at the back timestamp, so
   `currentTime` is a step function between pulls. Muted media with
   `render_muted_audio_` false plays to a `NullAudioSink` paced by the
   real-time `FakeAudioWorker`. Video-only media uses `WallClockTimeSource`,
   which already follows virtual time. V0.22 pulled the browser-side
   `FakeAudioOutputStream` from `beginFrame` through a nested `RunLoop` on the
   UI thread, with a sticky error on the second output stream and `SyncReader`
   zero-filling after a 0.95×buffer real-time wait. The native-video
   substitution patch predates the pull patch; it hooked the `VideoLayer` path
   (inactive on desktop), advanced only when the native pipeline produced a
   frame, and aborted the renderer on unexpected states. It is deleted.
3. **Video leads audio by 1-2 intervals in the stock path.**
   `VideoFrameSubmitter::OnBeginFrame` renders for
   `[frame_time+interval, frame_time+2·interval]`
   (`video_frame_submitter.cc:497-498`). A frame captured at T shows media
   time about 1.5 frames ahead of the audio pulled to T.
4. **Decode speed leaks into output.** Audio underflow becomes silence; a
   composited frame shows whatever the decoder had delivered. V0.22's WAV
   contains such silence, so byte identity with it is not a v2 goal.
5. **Configuration and state are process-global** (switches, static writer,
   `NoDestructor` dump); no capability discovery; errors lose the frame index
   and the encoder's exit status (`errno 32` is EPIPE: the reader was already
   gone, not slow).
6. **Scenario logic and engine logic are interleaved**, with real-time waits,
   per-worker differing `capture_offset`, two BeginFrames per captured frame
   against one per warm-forward frame, and a plan pass that grants no virtual
   time per frame.
7. **Surviving ideas:** controller-supplied absolute frame time with audio
   pulled to that time before the BeginFrame, and a serial plan pass followed
   by parallel replay workers with verification.

## 3. Architecture

```text
+------------------------------ dcap (one native binary, Rust + libav) ---------------------+
| spec -> probe -> plan (serial; action log; markers; hashes in windows)                    |
|      -> render: one browser per chunk replaying the action log; per browser an encoder   |
|         thread reads the shared-memory ring and writes chunk_k.<mp4|mov> in process       |
|      -> verify (overlap | boundaries | full | repeat) -> join (stream copy) -> report     |
+------+--------------------------------------------^---------------------------------------+
       | CDP over --remote-debugging-pipe (fds 3/4)  | canonical BGRA frames + PCM, shm ring (fds 5/6/7)
+------v--------------------------------------------+---------------------------------------+
| headless_shell --deterministic-capture                                                    |
|  browser: DeterministicCapture domain, CaptureSession (one per browser),                  |
|           internal DevTools session that owns renderer virtual time,                      |
|           worker: SHA-256 of the readback bitmap in place -> RawFrameSink (shm|file|null) |
|  renderer: CaptureAgent (thread wake, quiescence, audio render, markers),                 |
|            capture AudioRendererSink per element and per AudioContext,                    |
|            cancellable wait in VideoRendererImpl::Render,                                 |
|            capture-mode timers, virtual-time pausers where Blink has them                 |
+-------------------------------------------------------------------------------------------+
```

Dependencies point one way: controller → engine CDP contract → nothing above.
Scenarios are compiled into dcap and see only the restricted `Page` API.
No pixel data crosses a pipe: the browser copies each frame once into a
ring slot, the encoder reads it there. There is no ffmpeg process; the libav
libraries are built statically from the Chromium checkout's ffmpeg source
(`ffmpeg/build.sh`) and linked into dcap.

## 4. Browser engine

### 4.1 Mode switch and capabilities

`--deterministic-capture` (browser; forwarded to renderers through
`HeadlessContentBrowserClient::AppendExtraCommandLineSwitches`) implies
`--enable-begin-frame-control`, `--run-all-compositor-stages-before-draw`,
`--deterministic-mode`, `--disable-audio-output`, in-process audio service,
`--disable-accelerated-video-decode`, `--autoplay-policy=no-user-gesture-required`,
site isolation off (`IsolateOrigins`, `site-per-process`), bfcache,
prerendering and paint holding off, new-window blocking,
`MediaSessionService` off, idle media suspend off, `SilentSinkSuspender`
off, `TimeClamper` jitter off, `SetRenderMutedAudio(true)` for every player,
software video decoders only. It must not enable headless `kVirtualTime`,
which suppresses audio tracks.

`DeterministicCapture.getCapabilities` (browser session) returns:

```text
protocolVersion   "2.1"                 major must match the controller
patchVersion      "2.1.0-p5"
chromium          {version, lastChange}
build             {official, dcheck}
pixelFormats      ["bgra8-premul"]      the canonical form; nothing is converted in the browser
colorSpaces       ["srgb"]
frameSinks        ["shm","file","null"]
audio             {sampleFormats:["s16"], deviceFormat:{rate,channels,framesPerBuffer}}
hash              ["sha256-bgra8-premul"]
features          [{name,version}...]: pipelinedAdvance, advanceRange, shmSink, canonicalBgra (built);
                  quiescence, audioRender, markers, gpuCrashDetect (patches 5-9)
limits            {maxQueuedFrames, maxTargets:1}
gpu               {glRenderer, glVersion, glVendor}  from GpuDataManager; the controller's backend identity
```

### 4.2 Session model, configuration and state

`getCapabilities` is sent on the browser session. `configure`, `advance`,
`advanceRange`, `finish`, `abort` and `getState` are sent on the flattened
session of the initial page target; the controller creates no other target.
Target states: `unconfigured → configured` (configure repeatable) `→
capturing` (first advance) `→ finished | failed | aborted`, plus `detached`.
`finish` in `failed` closes the sink and returns partial totals with
`status:"failed"`. `getState` returns `{state, nextFrameIndex, lastError}`.

```text
configure({
  requiredFeatures: ["pipelinedAdvance","advanceRange","shmSink","canonicalBgra", ...],
  frameSink: {type:"shm"|"file"|"null", ringFd:5, readyFd:6, freeFd:7 | path,
              expectedFrames, maxQueuedFrames:4, stallTimeoutMs:60000},
  audio: {sink:{type:"file"|"null", path, sampleFormat:"s16"|"f32", header:"wav"|"rf64"},
          deviceFormat:{rate:48000, channels:2}, stems:false},
  virtualTime: {initialVirtualTime, maxTaskStarvationCount:0},
  frameRate: {num:60000, den:1001},
  markers: {prefix:"dcap:", sources:["performanceMark","binding"]},
  media: {waitTimeoutMs:30000, failOnUnsupportedApi:true},
  navigation: "fail"|"warn",
  backgroundColor: "#ffffff"
}) -> {effective: {...as applied...}, virtualTimeTicksBase}
```

Rules:

- `configure` must precede the captured document's navigation: the engine
  enables renderer virtual time (policy `pause`, `initialVirtualTime`) through
  its own internal DevTools session at that moment, before any document or
  player exists, and returns `virtualTimeTicksBase`. All frame times are
  offsets from that base, so the real-clock origin of virtual time does not
  matter. The engine filters `Emulation.setVirtualTimePolicy` from every
  other session in `HeadlessDevToolsManagerDelegate::HandleCommand`
  (`VirtualTimeOwned`).
- Refused with `UnknownFeature` if a required feature is missing; the
  effective configuration is echoed because CDP ignores unknown parameters.
- Idempotent before the first `advance`; `AlreadyStarted` afterwards.
- The ring is mapped and validated at `configure` (`SinkNotConnected`,
  `InvalidParameters` on a geometry or version mismatch); its descriptors are
  inherited at launch, like the CDP pipe.
- Output dimensions `width·dsf × height·dsf` must be even (`InvalidViewport`)
  so every delivery codec can take the frame. The page is composited over
  `backgroundColor`; the canonical form keeps premultiplied alpha.
- Frame time: `T_n = base + offset(n+1)` with
  `offset(k) = round_half_down(k·den·1e6/num)` microseconds, computed from the
  index. Every frame, including frame 0, is preceded by a grant of
  `offset(n+1) − offset(n)`: a document cannot load or paint while virtual
  time is paused (measured on 158: no damage and no readback until the first
  grant), so there is no frame at `base` itself. Audio sample frames through
  frame n: `S(n) = floor(n·rate·den/num)`.

### 4.3 advance: one frame, one round trip

```text
advance({frameIndex, captureVideo:true, captureAudio:true, hash:true, inputs:[...]})
 -> {frameIndex, virtualTimeMs, hasSurface, hasDamage, reusedReadback,
     audioSampleFrames, pcmFrameHash,
     markers:[{name, frameIndex, source}],
     media:[{playerId, state, mediaTimeMs, waitedMs, ackedBeginFrame}],
     timings:{grantMs, wakeMs, quiesceMs, audioMs, beginFrameMs, readbackMs, convertMs, hashMs, sinkWaitMs, queueDepth},
     completed:[{frameIndex, frameHash, chainHash}],   hashes of earlier frames, pipelined
     warnings:[...]}
```

`frameIndex` must equal the next expected index (`FrameIndexMismatch`).
Repeating the last completed index returns the cached response, so a lost
response is retried safely. `captureAudio` means "write this interval's PCM
to the sink"; audio is always rendered. `hash` is independent of
`captureVideo`. `inputs` are injected before the grant with timestamp
`T_{n−1}` and acknowledged inside the frame (rAF-aligned events such as
`mousemove` are acknowledged only after a main frame, so awaiting
`Input.dispatch*` between frames would deadlock).

The engine performs, in this order:

1. **Grant.** Through its internal DevTools session, grant
   `offset(n+1) − offset(n)` of renderer virtual time (`pauseIfNetworkFetchesPending`,
   `maxVirtualTimeTaskStarvationCount: 0` so a held pauser is never bypassed)
   and wait for `virtualTimeBudgetExpired`. Content's `EmulationHandler` has no
   virtual-time code; the command reaches the renderer's
   `InspectorEmulationAgent`, which keeps loader deferral and pause-on-expiry.
2. **Wake.** `CaptureAgent.Prepare(T)` first posts an immediate task to the
   media thread, the VideoFrameCompositor thread and the capture audio thread,
   and signals the ThreadPool delayed-task manager, so delayed tasks whose
   virtual run time has passed execute now rather than at a real-time-dependent
   later moment. The specific timers that matter (video `ended` delay,
   compositor background render, media-element event queue) are also patched
   to re-evaluate on the next `Render` in capture mode *(verify in
   prototype 1: measure the virtual time at which a 250 ms compositor-thread
   task runs)*.
3. **Quiesce.** Wait until no player has a media-pipeline operation in flight:
   `PipelineController::IsStable()`, no pending seek, suspend or resume, and a
   media-thread round trip after any play or rate change (which have no
   acknowledgement). The wait is asynchronous: the main thread is never
   blocked, because `MultiBufferDataSource` delivers reads on it. Transitions
   that complete only after more virtual time (a 250 ms delayed
   `OnBufferingStateChange` for zero-duration loops, loader retries) finish in
   later grants and are not waited for. Timeout `media.waitTimeoutMs`
   (`MediaWaitTimeout{players}`).
4. **Render audio** on the capture audio thread up to T (4.6), then run one
   task on the media-element event queue so `ended`, `timeupdate` and
   `canplay` handlers queued by the pull execute before the main frame.
   Repeat steps 3-4 until both find nothing to do (an `ended` handler can
   start a new transition).
5. **Collect** markers with virtual timestamps in `[T_{n−1}, T_n]`
   (a mark made in rAF at exactly `T_{n−1}` belongs to frame n−1 and was
   already delivered), media state, and each player's last acked BeginFrame
   sequence.
6. **BeginFrame** at `frame_time = T`, `interval = Δ`, with `CopyFromSurface`
   when `captureVideo` or `hash`. In capture mode `VideoFrameSubmitter`
   renders for `[T, T+Δ]` instead of `[T+Δ, T+2Δ]`, so the frame shows the
   media time the audio reached. `VideoRendererImpl::Render` waits until
   decoded frames cover the deadline (4.5). viz under
   `--run-all-compositor-stages-before-draw` waits for every surface that
   received the BeginFrame; the agent verifies after the frame that each
   playing video's submitter acked this sequence number
   (`VideoFrameNotPresented` otherwise).
7. **Readback pipeline** on a worker sequence: canonical RGBA (premultiplied,
   fixed channel order, stride stripped) → SHA-256 → NV12 BT.709 limited →
   bounded queue. A cached bitmap is reused only when the compositor reported
   no damage and the cache was read back at frame n−1; otherwise the copy is
   forced. `hasDamage` is viz's report; `reusedReadback` means no copy was
   taken. Reuse is expected to be bit-identical to a repeat copy *(verify in
   prototype 2 with a `forceReadback` mode)*.
8. **Respond** as soon as the BeginFrame and its readback complete: the
   frame-finished callback fires when both the BeginFrame ack and the
   `CopyFromSurface` result have arrived (`PendingFrame` releases it from its
   destructor), so `beginFrameMs` includes the readback. Hash, conversion and
   sink write are posted to the worker and the hash of frame n arrives in the
   `completed` list of a later response or in `finish`. Measured on 158: the
   canonical copy still runs on the UI thread before the response; section
   13.2 moves it. The copy request
   is issued before the BeginFrame and needs an existing surface, which the
   first frame after navigation lacks; the engine then repeats the BeginFrame
   at the same frame time with the copy request (`surfaceWarmed:true`), so
   frame 0 is captured. A frame that still yields no readback reports
   `hasSurface:false`, and `captureVideo` on it fails with `NoSurface`. If the sink queue is full the response is delayed up to
   `stallTimeoutMs` (`SinkStalled`); the UI thread never blocks.

`advanceRange({fromFrameIndex, toFrameIndex, captureAudio:false, hash:false})`
(`toFrameIndex` exclusive) runs the same eight steps per frame inside the
browser without readback, returns `{frames:[...], nextFrameIndex}` and
`lastCompletedFrameIndex` on error, and may emit `rangeProgress` events. The
controller splits ranges at action-log frames and at most every K frames so
each call stays inside the CDP timeout. There is no "no-display" primitive:
`animate_only` BeginFrames never reach `VideoFrameSubmitter` and skip
main-frame commit, so they would starve video and diverge from a serial
render.

`finish()` closes the sink (the encoder sees EOF), finalizes the audio
header, and returns `{status, framesWritten, expectedFrames, reusedFrames,
audioSampleFrames, chainHash, pcmHash, histograms, gpuProcessCrashes,
firstSinkError, completed:[...]}`. If `framesWritten < expectedFrames` the
status is `failed` with `IncompleteOutput`. It is idempotent. `abort()` stops
without totals; the controller terminates the encoder first, because EOF
alone cannot distinguish abort from finish. `chainHash_n = SHA-256(chainHash_{n−1}
‖ frameHash_n)` over hashed frames since `configure`; `getState()` and
`captureDebugFrame()` (PNG of the last composited frame) exist for diagnosis.

Typed errors, sticky after the first terminal one: `VersionMismatch`,
`UnknownFeature`, `NotConfigured`, `AlreadyStarted`, `FrameIndexMismatch`,
`InvalidViewport`, `NoSurface`, `SinkNotConnected`, `SinkClosed{frameIndex,
errno, peerClosed, bytesIntoFrame, framesFullyWritten}`, `SinkStalled`,
`MediaWaitTimeout{players}`, `VideoFrameNotPresented{players}`,
`AudioUnderflow{sink}`, `AudioFormatUnsupported`, `UnsupportedApiUsed{api}`,
`VirtualTimeStall`, `VirtualTimeOwned`, `NavigationDuringCapture`,
`RendererCrashed`, `GpuProcessLost`, `TargetDetached`, `IncompleteOutput`.
Events (`sinkError`, `mediaWaiting`, `unsupportedApiUsed`, `gpuProcessLost`,
`rangeProgress`) are advisory; the next response is authoritative because CDP
does not order renderer events against browser responses.

### 4.4 Clock

There is no new clock. Phase table for frame n:

| Reader | During the grant for frame n | After step 4 | At BeginFrame(T_n) |
| --- | --- | --- | --- |
| `Date.now()`, `performance.now()`, timers, rAF timestamp | advances T_{n−1} → T_n | T_n | T_n |
| `video.currentTime` (audio track) | step function: audio clock at T_{n−1} | T_n | T_n |
| `video.currentTime` (no audio) | virtual time | T_n | T_n |
| `AudioContext.currentTime` | last rendered quantum | first quantum boundary ≥ T_n | same |
| Compositor `BeginFrameArgs.frame_time` | T_{n−1} | T_{n−1} | T_n |

Two sources of per-run drift in this table were found and fixed on 158 with
the first engine build (2026-11-24): the document's `navigationStart`, which
the browser supplies from wall time and which seeds `performance.timeOrigin`
and the animation timeline's zero time, is snapped to the frozen virtual now
in capture mode (`document_load_timing.cc`); and `TimeClamper`, whose 100 µs
rounding of `performance.now()` and of timeline time uses a per-instance
random secret, uses a constant secret in capture mode (`time_clamper.cc`).
Without the second fix the same frame sampled an animation at 16.6 or 16.7 ms
depending on the run. With both fixes ten animated frames hashed identically
across runs on SwiftShader and on Mesa D3D12. `performance.now()` as read by
a DevTools evaluate between frames still differs by up to 0.1 ms between runs
and is an open item (section 8).

### 4.5 Readiness: cancellable waits plus quiescence

- `VideoRendererImpl::Render` (VideoFrameCompositor thread): in capture mode,
  expire frames up to `deadline_min`, post `AttemptRead`, then wait on a
  `base::ConditionVariable` bound to `lock_` until `VideoRendererAlgorithm`
  holds a frame whose end is at or past `deadline_max` or end of stream,
  signaled from `FrameReady`. The wait is cancellable: `StopSink`, `Flush` and
  the destructor set a flag and signal before taking `callback_lock_`, which
  `VideoFrameCompositor::CallRender` holds across the render; without the
  cancel, `OnTimeStopped` on the media thread would deadlock against the
  waiting compositor thread. Timeout `media.waitTimeoutMs`.
- `AudioRendererImpl::Render` (capture audio thread) waits for decoded audio
  instead of filling silence, except after end of stream (`AudioUnderflow`
  on timeout).
- Background rendering in `VideoFrameCompositor` is disabled in capture mode;
  canvas and WebGL readers of a video get the frame selected by the last
  BeginFrame, and `requestVideoFrameCallback` metadata uses capture time.
- Media transitions hold a `kInstant` virtual-time pauser *(changed when
  patch 7 was built; see 14.2)*: `WebMediaPlayerImpl` pauses virtual time
  from every play, pause, seek, rate change, suspend, resume and pipeline
  start until the media thread has absorbed it, no `PipelineController`
  operation is pending and a running, unsuspended pipeline has reached
  `HAVE_FUTURE_DATA`. The completion therefore lands at the virtual time of
  the request. The earlier objection (a held pauser stops the main-thread
  delayed tasks some transitions need) was real for `MultiBufferDataSource`'s
  20 ms seek delay, which is now zero in capture mode; the quiescence wait of
  4.3 step 3 remains as the fallback and the source of the `media` report.

### 4.6 Audio: renderer-side capture sinks

Headless registers a `blink::AudioDeviceFactory` subclass from
`HeadlessContentRendererClient` (the Fuchsia and Cast embedders are the
precedents) overriding `NewMixableSink`, `NewAudioRendererSink` and
`GetOutputDeviceInfo`:

- Each `<audio>`/`<video>` element gets its own `CaptureAudioRendererSink`
  (a `SwitchableAudioRendererSink`); Blink's `AudioRendererMixer` is not
  used, so its 10 s pause timer is moot. Muted elements still render
  (`SetRenderMutedAudio(true)`); volume is applied in the sink.
- Each `AudioContext` gets a capture sink that renders fixed buffers of its
  `callback_buffer_size_` (`AudioDestination::Render` checks the size) on a
  dedicated non-main thread; the AudioWorklet runs on its own thread and the
  destination waits for it, deterministically. `AudioContext.currentTime`
  therefore reaches the first quantum boundary at or past T.
- `kWebRtc` and `kNonRtcAudioTrack` sink requests are reported as
  `UnsupportedApiUsed`.

On `Prepare(T)` the agent renders each sink in whole buffers until at least
`S(n+1)` sample frames exist, keeps the excess in a per-sink FIFO, and reports
`delay` = FIFO fill at `delay_timestamp = T` so `AudioClock` reads T. Sinks
are resampled (`media::MultiChannelResampler`) and channel-mixed to the
device format, summed in float in sink-creation order, converted once, and
returned to the browser with optional per-sink stems; the browser writes WAV
or RF64 and hashes the PCM per frame (`pcmFrameHash`) and in total
(`pcmHash`). `AudioContext.getOutputTimestamp()` reports capture time.

This removes the browser-side `FakeAudioOutputStream` pump, the
audio-service `SyncReader` real-time timeout and prefetch race, the
one-stream limit, the nested `RunLoop` and the sample-rate mismatch error.
Fallback if the factory override fails in practice: the V0.22 browser-side
pump with `RequestBeforeRead`, an unbounded `SyncReader` wait and float
mixing in `services/audio`, behind the same CDP contract.

### 4.7 Frame sink

Input is the `CopyFromSurface` N32 bitmap already produced in
`HeadlessWebContentsImpl::BeginFrame`: shared memory from viz, premultiplied
BGRA on desktop builds. That is the canonical form (`canonical_hash.h`):
rows packed, no conversion. The UI thread only hands the bitmap to the
worker sequence, which hashes it in place (row by row, so a non-BGRA color
type would be converted one row at a time, never a full-frame copy) and
calls the sink:

- `shm`: the controller's memfd ring (`shm_ring.h`, mirrored by
  `dcap/src/shm.rs`): header with geometry, `produced`/`consumed` counters
  and a `closed` flag; `maxQueuedFrames` slots of a slot header (frame index,
  virtual time, flags, SHA-256) plus the frame and an audio area. The worker
  waits for a free slot on the `free` eventfd up to `stallTimeoutMs`
  (`SinkStalled`), copies the rows once, publishes with a release store and
  signals the `ready` eventfd. `finish` and `abort` set `closed` (and the
  error flag on abort) and signal; the consumer drains and exits.
- `file`: packed canonical BGRA appended through a writer thread, for tests.
- `null`: frames are counted and dropped, for the plan pass and benchmarks.

The sink never writes more than `expectedFrames`. Errors carry the frame
index and frames fully written and are reported on the next `advance` and on
`finish`. `beginFrameMs` includes the GPU readback (the frame-finished
callback fires after both the ack and the copy result); `hashMs` and `sinkMs`
are reported per frame and as histograms in `finish`.

### 4.8 Asynchronous completions

Work finished off the main thread lands at a virtual time that otherwise
depends on real speed. Covered in v2: resource and media fetches
(`pauseIfNetworkFetchesPending`), media pipeline transitions (4.3 steps
2-4), image decoding promises (`img.decode`, `createImageBitmap`, `toBlob`),
`decodeAudioData`, IndexedDB and Cache Storage requests, `AudioContext`
`resume`/`suspend` state changes, font loading under the pinned policy. Each
either holds an instant pauser until it completes or is released at step 2
in a canonical order. Dedicated workers join virtual time only when the
engine auto-attaches a DevTools session to them and sets the same policy; an
idle worker client can freeze the coordinator's minimum rule *(verify in
prototype 1; until then workers are reported)*. Reported, not pinned: shared
and service workers (bypassed by default), `BroadcastChannel`, cross-process
messaging, WebGPU asynchronous results, WebCodecs. The engine also pins
`--force-effective-connection-type`, device scale factor, `Save-Data`,
`--hide-scrollbars`, and installs a capture-mode hook in the renderer's
`base::RandBytes`/`crypto::RandBytes` so `crypto.getRandomValues`, UUIDs and
blob URLs repeat *(built: `base::subtle::SetRandBytesOverride`, a seeded
xoshiro256** stream from `--deterministic-capture-seed`; see 14.4)*.

### 4.9 Detection of unsupported APIs

Use counters and sink requests for MediaStream, `getUserMedia`,
`captureStream`, `MediaRecorder`, EME, WebRTC, WebGPU, `setSinkId`, shared
and service workers, `Atomics.wait` on the main thread, and cross-document
navigation after frame zero are surfaced as `UnsupportedApiUsed{api}` on the
next response; `media.failOnUnsupportedApi` chooses between failing and
warning. GPU process loss and WebGL context loss are terminal
(`GpuProcessLost`): the fallback renderer would change pixels.

## 5. Controller: dcap

One Rust binary (`deterministic-capture/dcap/`, dependencies: libc, serde,
serde_json, sha2) with a 600-line C shim over libavcodec/libavformat/libswscale
(`csrc/dcap_av.c`) linked statically from the libraries that
`ffmpeg/build.sh` builds out of the Chromium checkout's ffmpeg source.
CDP runs over `--remote-debugging-pipe` with a reader thread and blocking
calls with timeouts; no async runtime, no Playwright, no WebSocket. Children
are launched in a new session with `PR_SET_PDEATHSIG`; the work directory
must not be on drvfs/9p. Modules: `cdp`, `launch`, `shm`, `engine`,
`session`, `browser`, `spec`, `scenario` (+`scenarios/presentation`), `plan`,
`render`, `verify`, `report`, `encode`, `cli`, `smoke`; `bin/fake_engine`
implements the engine contract for `tests/pipeline.rs`.

### 5.1 Capture spec

Required: `url`, `viewport`, `frameRate`, `timeline.stop`, `render.backend`,
`output.dir`. The minimal serial run:

```json
{"url": "file:///path/index.html", "viewport": {"width": 1920, "height": 1080},
 "frameRate": 60, "timeline": {"stop": {"type": "frames", "count": 600}},
 "render": {"backend": "angle-gl-d3d12"}, "output": {"dir": "./run"}}
```

Full schema with defaults:

```json
{
  "url": "...", "viewport": {"width": 3840, "height": 2160, "deviceScaleFactor": 1},
  "frameRate": {"num": 60000, "den": 1001},
  "timeline": {"start": {"type": "frame", "index": 0}, "stop": {"type": "marker", "name": "dcap:done"}, "maxFrames": 36000},
  "ready": {"fonts": true, "images": true, "media": true},
  "session": {"profile": "fresh", "storageState": null, "userAgent": null, "headers": {}},
  "determinism": {"seed": 20260703, "startTime": 1700000000000, "timezone": "UTC", "locale": "en-US",
                  "network": "offline", "mediaFeatures": {}, "predictableGc": false},
  "inputs": [{"frame": 0, "type": "click", "x": 1, "y": 1}],
  "render": {"backend": "angle-gl-d3d12", "backgroundColor": "#ffffff"},
  "audio": {"capture": false, "deviceFormat": {"rate": 48000, "channels": 2}, "sampleFormat": "s16"},
  "encoder": {"template": "x264-lossless", "args": {}},
  "parallel": {"workers": 1, "chunks": "auto"},
  "verify": {"mode": "repeat", "windowBefore": 2, "windowAfter": 60, "overlapFrames": 30, "repeat": 1},
  "scenario": {"module": null, "options": {}},
  "media": {"waitTimeoutMs": 30000, "failOnUnsupportedApi": true},
  "navigation": "fail", "trusted": false, "timeouts": {},
  "output": {"dir": "./run", "keepChunks": false}
}
```

Semantics: `frameRate` is an integer or a reduced rational. `start` types:
`frame`, `load` (frame of the main document's load event in the plan pass),
`marker`. `stop` types: `frame`, `frames` (count from start), `marker`,
`mediaEnded` (all or selected players). Output covers `[start, stop)`. A
marker resolves to the frame index the engine assigns. Reaching `maxFrames`
before `stop` is `StopNotReached`, not truncation. Precedence: an engine
terminal error, then `timeline.stop`, then the scenario's `should_stop`.
`startTime` is `initialVirtualTime`, recorded in the plan and reused by every
worker. `seed` feeds `--js-flags=--random-seed` and the renderer RNG hook.
`inputs[].type` ∈ click, mouseMove, mouseDown, mouseUp, key, keyDown, keyUp,
wheel; coordinates are CSS pixels. `encoder.args` override only keys the
template declares. `parallel.chunks` is `"auto"`, an integer, or a frame
list. `verify.overlapFrames` is F for `overlap`; `verify.repeat` is the
number of extra complete runs for `repeat`. `scenario.module` is an import
path. `backend` is explicit and pinned: the probe asserts the renderer string
(`D3D12 (NVIDIA`, or the Vulkan device name; never llvmpipe, lavapipe or
SwiftShader) and every worker is checked against it.

### 5.2 Phases

1. **Probe.** One browser: `getCapabilities`, renderer string, NVENC session
   limit, Mesa and driver versions, font fingerprint, and the pre-roll cost
   ratio `r` measured on the spec's page over a short range. Fail on mismatch.
2. **Plan.** Requires a horizon: `timeline.maxFrames` or the scenario's
   `horizon()`. Chunk boundaries are fixed before the pass starts
   (work-balanced on the horizon using `r`, or the scenario's boundaries), so
   the plan pass can hash the verification windows as it passes them and
   workers may start as soon as their window is behind the pass. The pass is
   serial with real BeginFrames: `frameSink: null`, `advance(captureVideo:false,
   captureAudio:<spec>, hash: in-window)`; it runs the scenario prelude through
   the restricted hook API, records every action as `{frame, phase, kind,
   payload, resultHash}` in the action log, collects markers and media events,
   resolves start and stop frames, and captures the full PCM from frame 0 when
   audio is requested. When `verify.mode` is `full` the pass hashes every
   frame and no second serial pass exists.
3. **Render.** Frame-zero sequence, identical in every browser: create the
   ring and launch with its descriptors; attach; apply 5.5 overrides and init
   scripts; `configure` (virtual time enabled here); `Page.navigate`; wait for the main
   frame's `Page.frameNavigated` with no grant and no evaluate; `advance(0)`.
   Workers replay the action log (no plugin code runs in workers), pre-roll
   with `advanceRange` to `start_k`, then advance with `captureVideo` and
   `hash` from `start_k`, plus `overlapFrames` more frames with
   `captureVideo:false, hash:true` for `overlap`. Chunk sizes are balanced by
   work (`r·start_k + n_k` equal across chunks). Pre-roll is the dominant cost
   of parallelism: total pre-roll is about N(c−1)/2 frames, and video pages
   decode every frame of it. The encoder thread of each worker opens
   `chunk_k.partial.<ext>` before the first frame, encodes each slot with
   `pts = frameIndex − start_k` in units of `den/num`, checks that the ring
   delivers consecutive indices and that the slot hash equals the engine's
   reported hash, and writes the trailer when the ring closes. The chunk is
   renamed only when `finish` reports every frame written and the encoder
   produced exactly that many packets.
4. **Verify.** `overlap`: compare each worker's overlap hashes with the next
   worker's output; `boundaries`: compare the plan pass hashes in the windows
   and the last frame of each chunk; `full`: compare every frame with the
   plan pass; `repeat`: run the whole thing `verify.repeat` more times and
   compare `chainHash` and `pcmHash` (encoded bytes are never compared).
   Markers recorded by workers must match the plan; `pcmFrameHash` of
   workers is compared with the plan inside the windows even though workers
   do not write PCM. Acceptance is exact equality; the report names the first
   differing frame, both hashes, both `hasDamage`/`reusedReadback` values, the
   media states, and PNGs of the frame and its neighbours from both runs.
5. **Join.** Stream-copy the chunk files into `capture.<ext>`: every input
   must match chunk 0 in codec, dimensions, pixel format and extradata, hold
   exactly its frame count of packets starting with a keyframe (and exactly
   `S(end_k) − S(start_k)` audio samples once audio lands); video `pts` is
   `first_frame_k − start + ordinal`, audio `pts` is `S(first_frame_k) −
   S(start) + samples so far`; input timestamps are never read. Chunks and
   output are MP4 or MOV: Matroska's 1 ms `TimecodeScale` cannot carry
   `1001/60000`. Write the report.

### 5.3 Scenario plugins

A plugin module exposes `browser_options()`, `horizon()`, `ready(page)`,
`prelude(page)`, `should_stop(state)` and `boundaries(plan)`, all optional.
`page` exposes only `evaluate(expr)` at the defined point between frames,
`dispatch_input(event)`, `advance_until(predicate, max_frames)` and
`markers()`; no sleep and no wall clock. Action-log entries are `{frame,
phase:"preNavigation"|"beforeGrant", kind:"initScript"|"evaluate"|"input"|
"cdp", payload, resultHash}`; `advance_until` is not logged (its effect is the
frame of the next entry); predicates run in an isolated world with
`throwOnSideEffect` and are never replayed; main-world `evaluate` is replayed
and a `resultHash` mismatch is an early divergence error. Spec `inputs` and
plugin `dispatch_input` both compile to `input` entries, spec first at the
same frame.

The `presentation` plugin: a `preNavigation` init script wrapping
`HTMLMediaElement.prototype.play` to retain elements and emitting
`dcap:presentation:playing:i` / `ended:i` marks; `horizon()` from
`audio_manifest.json` durations; one recorded click at frame 0; stop at the
last `ended`; boundaries at `ended` marks split in two. Acceptance is v2
golden hashes (section 9).

### 5.4 Encoders

`encoder: {codec, pixelFormat, options, threads, container, audioCodec}`
selects a compiled-in libavcodec encoder; options are validated against a
per-codec allowlist and echoed in the report. The canonical BGRA frame is
passed to the encoder directly when it accepts `bgr0`/`bgra` (FFV1,
ffvhuff, rawvideo, NVENC) and converted once with libswscale otherwise
(planar RGB for utvideo and magicyuv; BT.709 limited YUV for delivery
formats). All codecs are LGPL components of the Chromium ffmpeg source,
built by `ffmpeg/build.sh`; x264 (GPL) is not available.

| Codec | Default pixel format | Container | Lossless to the hash | Note |
| --- | --- | --- | --- | --- |
| `ffv1` (default) | `bgr0` | mp4 | yes | level 3, 16 slices, slice CRC, intra only, range coder; byte-stable |
| `utvideo`, `magicyuv` | `gbrp` | mov | yes | intra RGB, fastest; MP4 has no tag for them |
| `ffvhuff` | `bgr0` | mov | yes | |
| `rawvideo` | `bgra` | mov | yes | archive without compression |
| `h264_nvenc`, `hevc_nvenc` | `yuv444p` | mp4 | no (hardware RGB→YUV) | `tune lossless`, 4:4:4 profile; `libnvidia-encode.so.1` loaded at run time |

`dcap codecs` reports which encoders open at a given size (NVENC creates a
session, so missing libraries or exhausted sessions show here) and `probe`
fails early with `EncoderUnavailable`. Failure attribution picks the earliest
of encoder error (`EncoderFailed`, `EncoderWriteFailed`), sink error,
controller timeout, target crash and GPU process loss and records it as
`failure.primary`.

### 5.5 Determinism controls

Flags: `--deterministic-capture`, `--js-flags=--random-seed=N`,
`--force-color-profile=srgb`, `--font-render-hinting=none`,
`--disable-lcd-text`, `--hide-scrollbars`, `--disable-software-rasterizer`,
`--ignore-gpu-blocklist`, `--enable-gpu-rasterization`,
`--force-effective-connection-type=4G`, backend flags; optional
`--js-flags=--predictable --single-threaded-gc`. CDP:
`Emulation.setTimezoneOverride`, `setLocaleOverride`,
`setDeviceMetricsOverride`, `setEmulatedMedia`,
`setDefaultBackgroundColorOverride`, `Network.setBypassServiceWorker`,
`Fetch` rules for `block-third-party`. The report records every flag,
override, backend identity (renderer string, Mesa, driver, Windows build
under WSL), font fingerprint, profile seed and spec hash; the reproducibility
promise is scoped to all of them.

## 6. Rendering backends

| Backend | Flags and environment | Status |
| --- | --- | --- |
| `angle-gl-d3d12` | `--use-angle=gl`, `GALLIUM_DRIVER=d3d12`, `MESA_LOADER_DRIVER_OVERRIDE=d3d12`, never `VK_ICD_FILENAMES` | WSL2. Stock headless_shell 158 measured 279-463 fps single-instance 4K render-only; capture throughput is bounded by the 33 MB readback and memory bandwidth, estimated 120-250 fps aggregate, to be measured |
| `angle-vulkan` | `--use-angle=vulkan --enable-features=Vulkan --disable-vulkan-surface` | Native Linux with a vendor ICD (July configuration). On WSL it resolves to lvp or SwiftShader and the probe rejects it |
| `swiftshader` | `--use-angle=swiftshader` | Functional tests and the browser test suite |

Encoded output is not byte-deterministic (NVENC); determinism is defined on
raw frames and PCM.

## 7. Patch series and repository layout

Base: Chrome Stable 158.0.8127.88 (checked out as `dev-v2` on 2026-11-24),
then Extended Stable cadence, off-cycle for security respins. Ordered so each
patch builds and is testable:

| # | Patch | Files |
| --- | --- | --- |
| 1 | Switch, implied flags, forwarding | `headless/public/switches.*`, `headless/lib/browser/command_line_handler.cc`, `headless_content_browser_client.cc` |
| 2 | CDP domain, capabilities, `VirtualTimeOwned` filter | `third_party/blink/public/devtools_protocol/domains/DeterministicCapture.pdl`, `browser_protocol.pdl`, `headless/protocol_config.json`, `headless/lib/browser/protocol/deterministic_capture_handler.{h,cc}`, `headless_devtools_session.cc`, `headless_devtools_manager_delegate.cc` |
| 3 | CaptureSession, internal DevTools session for virtual time, RawFrameSink, canonical hash | `headless/lib/browser/capture/{capture_session,virtual_time_owner,raw_frame_sink,canonical_hash}.{h,cc}` |
| 4 | `advance`/`advanceRange`/`finish` over BeginFrame and readback, pipelined responses | handler, `headless_web_contents_impl.cc` (minimal hook) |
| 5 (built) | Canonical BGRA hashed in place, `shm` ring sink (`shm_ring.h`), `file`/`null` sinks, `fifo` removed, protocol 2.1; `angle_enable_gl=true` in the build profiles so `--use-angle=gl` exists | `capture/{canonical_hash,raw_frame_sink,shm_ring}.*`, `capture_session.cc`, handler, `build/*.gn` |
| 6 (built) | Renderer CaptureAgent: wake, quiescence, markers, ack verification | `headless/lib/renderer/capture_agent.{h,cc}`, `headless/public/mojom/capture_agent.mojom`, `headless_content_renderer_client.cc`, `CaptureMediaRegistry` in `third_party/blink/renderer/platform/media/` |
| 7 (built) | Capture-mode media: cancellable video wait, audio wait, submitter deadlines, timers, muted rendering | `media/renderers/video_renderer_impl.cc`, `audio_renderer_impl.cc`, `third_party/blink/renderer/platform/media/video_frame_compositor.cc`, `web_media_player_impl.cc`, `third_party/blink/renderer/platform/graphics/video_frame_submitter.cc`, `renderer_web_media_player_delegate.cc` |
| 8 (built) | Capture audio sinks and mixing | `headless/lib/renderer/capture_audio_device_factory.{h,cc}`, `capture_audio_renderer_sink.{h,cc}`, `renderer_webaudiodevice_impl.cc` (suspender off) |
| 9 (partial: `TimeClamper` in 3, RNG hook built; pausers for image/IndexedDB completions and worker auto-attach open) | Pausers for asynchronous completions, `TimeClamper`, RNG hook, worker auto-attach | Blink image decode, Web Audio decode, IndexedDB, `time_clamper.cc`, `base::RandBytes` hook |
| 10 (built) | Unsupported-API detection | use-counter hooks in the agent |
| 11 | Protocol tests and browser tests | `headless/test/data/protocol/deterministic_capture/*`, `headless/test/` |

Budget for changes to existing upstream files: under 300 lines, tracked in
`patches/UPSTREAM_TOUCHES.txt` generated from `git diff --stat`. The series is
kept as `git format-patch` output under `deterministic-capture/patches/v2/`
and as the `dev-v2` branch; `patches/v1/` holds the V0.22 series for
reference. Estimated size: 3.5-4.5k production lines plus 1.5k of tests in
the ten patches above; V0.22 was 986 lines for audio and raw frames and 777
for the deleted video substitution.

## 8. Risks and prototype order

| Risk | Mitigation |
| --- | --- |
| Delayed tasks on non-main threads released at real-time-dependent virtual times | Wake step plus patched timers; prototype 1 measures when a 250 ms compositor-thread task runs under paused virtual time |
| Transition nondeterminism (play, preroll, seek, rate) and media→main event delivery racing `BeginMainFrame` | Quiescence with media-thread round trips and the event-queue drain; prototype 1 injects CPU contention (`stress`) and random controller sleeps and compares hashes over 10 runs |
| `AudioDeviceFactory` override gaps (mixable sinks, device info, WebRTC paths) | Override all three entry points; fallback is the hardened browser-side pump; decided in prototype 3 |
| Video wait deadlock against `callback_lock_` | Cancellable wait; covered by a pause-during-wait test |
| 4K readback throughput on Mesa D3D12 | Prototype 2 measures RGBA readback versus GPU NV12 with per-stage `timings`; conversion and hashing off the UI thread |
| Chunk inequivalence for video and feedback canvases | Full pre-roll with real BeginFrames; `overlap` on by default; `repeat` to attribute |
| GPU process crash under load (seen once on Mesa 26.0.3) | Terminal error; chunk retry up to 2 in a fresh browser; same frame twice fails the run; Mesa pinned at 26.3 or later |
| Idle worker client freezing virtual time | Workers reported until prototype 1 settles auto-attach |
| Upstream churn | New files for logic, minimal hooks, touch budget |

Prototype order: (0) rebuild V0.22's raw-frame path on 158 with
`--use-angle=gl` and per-stage trace events to confirm the WSL path end to
end; (1) muted video and video with audio under virtual time with the wake
step, waits and quiescence; (2) readback paths; (3) audio sinks against a
two-stream page; (4) chunk verification end to end; (5) the `presentation`
plugin and golden values.

## 9. Acceptance

v2 acceptance values are measured once per backend identity on this
workstation and stored with the report fingerprint: run-to-run identity of
`chainHash` and `pcmHash` across ten runs; chunked equals serial under
`boundaries` and `overlap`; fault injection (encoder killed at frame 100:
`failure.primary` is the encoder exit and `SinkClosed.framesFullyWritten`
equals the frames the encoder consumed); and the `presentation` fixture's
frame count and boundary frames with their delta from V0.22's 2,669 / 803 /
1741 explained by the underflow silence measured in the V0.22 WAV. V0.22's
WAV SHA-256 is not an acceptance value.

## 10. Report, resume, cleanup, timeouts

`capture-report.json`: status and resumability; spec as given and as
resolved with its hash; dcap and browser identity (path, SHA-256, build ID,
capabilities); all flags and environment; backend identity; ffmpeg version and
argv; plan (frames, markers, chunk table and rationale, `initialVirtualTime`);
per chunk an `attempts[]` list with PIDs, phase timings, frames written,
reused frames, media waits with the blocking player, sink statistics, GPU
crash count, encoder exit status with stderr tail and log path, output size,
SHA-256 and packet count; per-chunk hash files (8-byte SHA-256 prefixes per
frame) and the run `chainHash` computed by dcap over `[start, stop)`; verify
results; mux checks; the failure object; peak RSS per process.

`--resume` reuses chunk outputs and the plan pass keyed on spec hash, browser
SHA-256, backend identity, encoder argv hash and frame range; a retried
chunk's hashes must match the failed attempt's prefix. Work directories are
`run/<id>/work/` with FIFOs, profiles and `TMPDIR`; stale directories are swept
with a PID liveness check. Defaults: launch 30 s, load 60 s wall, ready 30 s
virtual / 120 s wall, sink stall 60 s, encoder open 10 s, encoder finish
120 s, CDP command 30 s, chunk 5× expected work. The controller's `advance`
timeout exceeds the grant wall time plus `waitTimeoutMs` plus
`stallTimeoutMs` plus 10 s so the engine's typed error always arrives first.

## 11. Compatibility

V0.22 switches, `beginFrame` parameters and `capture.py` are removed. The
V0.22 fixture remains the first scenario regression under the new acceptance
rule. If the renderer-side audio sink proves unreachable, the browser-side
pump returns in hardened form behind the same CDP contract; the controller
does not see the difference.

## 12. Review record

Draft v1 was reviewed on 2026-11-24 by six agents (three code reviews:
client, audio and raw-frame patch, native-video patch; three design reviews:
Chromium feasibility, determinism and API generality, operations and
pipeline). Draft v2 was reviewed by two more (source recheck of the engine
claims; controller-side consistency). Settled points:

- No new media clock: renderer virtual time overrides `TimeTicks::Now()`
  process-wide (verified at `process_time_override_coordinator.cc:82` and
  `base/time/time_override.cc`). The feasibility review's separate media
  clock was not adopted; its findings on blocking waits, quiescence, the
  `animate_only` gap, audio IPC timeouts and content's `EmulationHandler`
  were.
- The source recheck refuted v2's assumption that non-main-thread delayed
  tasks run at the right virtual time without help, found the 1-2 interval
  video lead in `VideoFrameSubmitter`, the muted-media `NullAudioSink`
  bypass, the `AudioDestination` fixed-buffer check, the `callback_lock_`
  deadlock, the starvation-path pauser bypass, and that an internal DevTools
  session is the feasible way for the engine to own virtual time; all are in
  4.3-4.6 and 8.
- The consistency review's contract fixes (pipelined responses, cached retry,
  state machine, first-surface rule, frame-zero sequence, `advanceRange`
  contract, PCM trimming, start/stop types, action-log encoding,
  `chunkFormat` with a lossless Annex-B default, horizon-first planning,
  timeouts, wording) are in 4.2-4.3 and 5.
- Hashing on canonical RGBA; shared-memory ring, I420/RGBA sinks, audio FIFO,
  `auto` backend, JS-predicate stop in workers, `on_frame`, `Fetch` replay,
  the no-display primitive and `displayTailFrames` were cut.

## 13. Encoder placement and the engine data path (implemented 2026-11-24)

Status: proposed and reviewed by four agents on 2026-11-24 (build and ffmpeg
configuration, data path and memory, operations and chunking, adversarial
alternatives), then implemented the same day as the owner decided on a
single native binary with no Python: patch 5 in the browser (13.2), the
`shm` ring and the Rust `dcap` binary with the libav shim (13.3), MP4 chunks
with the index-based join (13.4). Sections 3, 4.7, 5 and 7 describe the
result; this section keeps the measurements and the reasoning. Where
reviewers disagreed, the tree decided.

### 13.0 Result

Measured on TITAN (WSL2, Mesa D3D12 on the RTX 4090 through
`--use-angle=gles-egl`, patched `headless_shell` from `build/x64-dev.gn`,
`dcap` debug build, animated test page, FFV1 `bgr0` in MP4, 60 frames):

| Size | Wall per frame | `beginFrameMs` p50 (incl. readback) | Hash p50 (SHA-256, worker) | Encoder thread per frame | Chunk file per frame |
| --- | --- | --- | --- | --- | --- |
| 1920×1080 | 14.8 ms | 11.4 ms | 3.8 ms | 4.8 ms (FFV1) | 37 KB |
| 3840×2160 | 51.0 ms | 38.8 ms | 15.6 ms | 22 ms (FFV1); 5.6 ms (utvideo, 3.1 MB per frame) | 145 KB |

Run-to-run hashes are identical at every size. A real page (CSS animations,
text, WebGL canvas) at 1280×720: 120 frames with a repeat verification run
in 6.3 s wall; 180 frames in two workers with boundary verification (63
frames compared) in 6.2 s; the whole `dcap` process, encoder included, peaks
at 229 MB RSS. The joined file carries exact `1001/60000` frame durations.
Earlier figures labelled "D3D12" in this document and in the wiki before this
section were measured while the GPU process was exiting at start-up (13.2
explains why) and were software compositing.

### 13.1 Where the time and memory are (corrected)

Measured on TITAN (WSL2, Mesa D3D12, patched `headless_shell`, `file` sink):

| Component | 1080p | 4K |
| --- | --- | --- |
| Wall per frame, controller view | 14.2 ms | 56.0 ms |
| `beginFrameMs` (grant to frame-finished, **includes GPU readback**) | 8.5 ms | 31.9 ms |
| `grantMs` | 0.4 ms | 0.5 ms |
| UI-thread canonical copy: 33 MB `std::vector` zero-fill and page faults, plus BGRA to RGBA `readPixels` swizzle (`canonical_hash.cc:26-29`, called at `capture_session.cc:361`) | about 3 ms | about 12 ms |
| CDP JSON and controller round trip | 1-2 ms | 1-2 ms |
| Controller CPU per frame (Python) | 0.26 ms | 0.36 ms |
| Worker per frame (off the critical path unless `queueDepth` reaches `maxQueuedFrames`): SHA-256 with SHA-NI 2.4 GB/s, second swizzle for libyuv, NV12 convert | about 7 ms | SHA-256 14 ms + swizzle 12 ms + convert 5 ms |
| FIFO write of one 4K NV12 frame on the sink thread (1 MiB pipe) | - | 3.2 ms, throughput only; never on the wall unless the consumer is slower than the producer |

Corrections to the first draft: `beginFrameMs` already contains the readback
(`headless_web_contents_impl.cc:345-371`: `PendingFrame` runs the callback
from its destructor after both the ack and the copy result), so the gap to
the wall is the UI-thread canonical copy and CDP, not "readback, hash,
conversion". N32 is BGRA on desktop (`SkUserConfig.h`, `SK_R32_SHIFT 16`)
and libyuv's "ARGB" is BGRA byte order, so the engine swizzles every frame
twice. The two FIFO copies are a throughput cost on an idle thread.

Encoder memory (ffmpeg 8.0.1 CLI, 4K): the CLI's own queues cost about
0.8 GB before any codec (`rawvideo` copy 795 MB), so no flag setting reaches
a sub-gigabyte process. FFV1 itself allocates a worst-case packet of
w·h·168/8 ≈ 180 MB per `encode_frame` (`ffv1enc.c:1862-1884`,
`ff_alloc_packet`) unless the caller supplies `get_encode_buffer`; NVENC's
CUDA context costs about 160 MB plus one frame. Reviewed speeds on a 4K
synthetic source (optimistic; noise is 2-10× slower): utvideo, magicyuv,
ffvhuff 9-12 ms RGB intra; FFV1 RGB 20-29 ms (63 ms on noise); libx264
`qp 0` 10 ms (4:2:0, lossy against the RGBA hash); `hevc_nvenc` lossless
12-13 ms from BGRA (hardware RGB to YUV 4:4:4, lossy against the RGBA
hash); VP9 lossless 100 ms. Only RGB-native codecs are lossless to the hash.

### 13.2 Priority 1: the engine path, independent of any encoder

These changes need no ffmpeg and are expected to remove about 12 ms of UI
thread and 12 ms of worker time per 4K frame:

1. Canonical pixel form becomes N32 premultiplied BGRA with the stride
   stripped (the readback bitmap already has `stride == width*4`). The hash
   is computed in place on the worker over the shared-memory bitmap; no
   canonical copy on the UI thread, no second swizzle before libyuv.
2. One reusable conversion buffer per session; `maxQueuedFrames` bounds the
   number of readback bitmaps alive, including any encoder queue.
3. `readbackMs` is measured (the field exists and is never filled) and the
   `advance` response is sent on the frame-finished callback with nothing
   else in front of it.
4. Section 4.3 step 8 was corrected to match the code.

Gate: 4K wall per frame within 2 ms of `beginFrameMs` + `grantMs`; hashes
unchanged except for the documented channel-order change of the canonical
form (recorded as a protocol minor version). Outcome: 1080p wall is 3.4 ms
above `beginFrameMs` + `grantMs` with the FFV1 encoder thread competing for
cores; at 4K the gap is 12 ms because the worker's SHA-256 (15.6 ms) plus the
slot copy approach `beginFrameMs` and backpressure appears (`sinkMs` p95
22 ms). The hash is now the largest CPU cost after rendering; a faster hash
(BLAKE3 or hashing split across threads) is the next engine-side step.

GPU path on this build: `--use-angle=gl` failed with "Initialization of all
(0) EGL display types" because ozone headless builds set
`angle_enable_gl=false`; with it on, ANGLE still chose no display because
its generic `DisplayEGL` is compiled for platform type 0 only under
`ANGLE_USE_GBM` or `ANGLE_USE_WAYLAND` (`third_party/angle/src/libANGLE/Display.cpp`),
and the build profiles have X11 and Wayland off. The profiles set
`angle_enable_gl=true` and the controller passes `--use-angle=gles-egl`,
which selects `DisplayEGL` by device type without any of those defines
(`angle_use_wayland=true` is not needed and does not build: the Vulkan
Wayland surface lacks `VK_USE_PLATFORM_WAYLAND_KHR`); the renderer string is
`ANGLE (Microsoft Corporation, D3D12 (NVIDIA GeForce RTX 4090), OpenGL ES 3.1)`
and `probe` fails with `BackendMismatch` otherwise.

### 13.3 Priority 2: encoder over shared memory, inside dcap

Decision: the encoder does not live in the browser and does not use
Chromium's bundled ffmpeg's GN build. As implemented it is a thread of the
`dcap` binary (the review proposed a separate helper process; the owner
chose one binary); the rest of this subsection is the reviewed reasoning.

- **Browser side**: a `shm` sink. The controller creates a memfd ring of
  `maxQueuedFrames` slots (BGRA frames plus PCM) and passes the descriptor at
  launch like the CDP pipe descriptors. The sink thread publishes a slot with
  a sequence counter and an eventfd; the helper releases it. One copy of
  0.7 ms per 4K frame replaces the two FIFO copies; the browser patch shrinks
  to the sink and carries no encoder threads, codec memory or CUDA context.
- **Helper**: one native executable, `dcap-encode`, built outside GN with
  libavcodec/libavformat (system libraries first; a fork-vendored ffmpeg
  configured for exactly the needed components later, when a single static
  binary matters). It reads the ring, encodes with a bounded number of
  frames in flight (2-3 at 4K, about 100 MB), supplies `get_encode_buffer`
  so FFV1 reuses one packet buffer, writes MP4 chunks and performs the join.
  Any codec the installed ffmpeg carries is available, including GPL x264,
  utvideo, magicyuv, FFV1 and NVENC; the helper reports codec availability
  and byte stability to the controller.
- **Controller**: orchestration only, no pixel data; Python stays. The helper
  can later absorb the controller to make the single native binary the owner
  asked about, justified by the data path rather than by Python's cost.

Why not Chromium's ffmpeg (first draft): `//third_party/ffmpeg` is a separate
git repository pinned by `DEPS` (`ffmpeg_revision`), configured
`--disable-all` with `CONFIG_ENCODERS 0`, `CONFIG_MUXERS 0`, `CONFIG_BSFS 0`
and no I/O protocol (`avio_open` on a path fails). Enabling FFV1, PCM, FLAC,
Matroska, MP4 and WAV needs about 30 sources missing from
`ffmpeg_generated.gni` plus the `_select` groups (`RANGECODER`, `RIFFENC`,
`LPC`, `AUDIO_FRAME_QUEUE`, `ISO_WRITER`, several BSFs) and an `autorename`
shim for the two `flacenc.c`; NVENC additionally needs the absent
`nv-codec-headers`, `hwcontext_cuda.c` and `CONFIG_CUDA`. The generated
files cover 22 platform directories and are regenerated by
`media/ffmpeg/scripts/generate_gn.py` from per-platform build outputs, so the
fork would have to own a forked ffmpeg repository and redo the edits on every
roll, for a gain of one 0.7 ms copy and one process over the helper, while
placing encoder threads and 200-600 MB of codec memory inside the browser.
Chromium also plans to deprecate ffmpeg (Symphonia; demuxer features
disabled by default with a removal TODO around M177), which is a low risk for
12-18 months but a cost with no benefit here. All of this is verified in the
tree; the facts stand if the decision is ever revisited.

### 13.4 Chunks, audio and the join (operations review)

- **Container**: MP4/MOV for chunks and output. Matroska's `TimecodeScale`
  is hardcoded to 1 ms (`matroskaenc.c:2613`), so 60000/1001 timestamps
  round and a later remux inherits 16/17 ms durations; MP4 chunks joined by
  stream copy reproduce exact `1001/60000` timestamps (measured). Matroska
  only as a final container for integer frame rates. `finish` writes the
  trailer before returning totals; a chunk whose `finish` failed is deleted
  and re-rendered, as today.
- **Timestamps from indices**: `firstFrame` in the sink configuration; video
  `pts = frameIndex − firstFrame` in `den/num`, audio `pts = S(frameIndex) −
  S(firstFrame)` in `1/rate`, never accumulated. Pre-roll and overlap frames
  produce no packets because the sink is bound to `captureVideo` and
  `captureAudio`.
- **Audio moves into chunks**: workers capture audio on `[start_k, end_k)`
  only, exactly `S(end_k) − S(start_k)` samples; the plan-pass WAV and the
  controller's PCM trimming are deleted; the plan pass hashes PCM for every
  frame so `pcmFrameHash` verification becomes complete. All-or-none audio
  across chunks; silence of the exact count when capture is on and the page
  has no sinks.
- **Join rule**: inputs ordered by `firstFrame`; assert codec, extradata
  hash, dimensions, pixel format and color tags equal to chunk 0,
  `packets == frames`, first packet a keyframe, audio sample count exact;
  write with computed timestamps; emit JSON counts and SHA-256. FFV1 with
  fixed slices and H.264/HEVC with pinned parameter sets join exactly
  (measured); NVENC SPS/PPS equality is asserted, not assumed.
- **Escape hatch removed**: the Annex-B `chunkFormat`, byte concat, ffprobe
  validation and the FIFO path are deleted rather than kept beside the new
  path, so there is one sink, one validator, one join and one audio path.
  The fake engine for tests writes BGRA into the ring.
- **Failures**: sticky terminal `EncoderFailed{frameIndex, averror,
  packetsWritten, bytesWritten}`; retryable `EncoderDeviceLost` and
  `EncoderSessionLimit` (consumer NVENC session caps; several workers mean
  several sessions); `EncoderUnavailable` fails early when the libraries are
  missing, never a silent software fallback. Free-space preflight from the
  probe's bytes per frame.

### 13.5 Codec choice: by measurement on captured frames

The archive target stays lossless RGB because the readback carries the
surface color space (sRGB) and the BT.709 limited conversion is the only
lossy step. The codec is not decided: FFV1 RGB is the slowest candidate and
exceeds the 4K engine time on real content. Prototype gate: encode the same
captured 4K frames (not a synthetic source) with utvideo, magicyuv, FFV1
(`-g 1 -slices 16`), `libx264rgb -qp 0`, raw BGRA + zstd, and
`hevc_nvenc` lossless 4:4:4; record ms per frame, bytes per frame, RSS with
`get_encode_buffer`, byte stability and whether decode reproduces the RGBA
hash. The controller's `encoder` spec becomes codec parameters validated
against a per-codec allowlist and echoed in `effective`:

```json
"encoder": {"codec": "utvideo|magicyuv|ffv1|libx264rgb|hevc_nvenc", "pixelFormat": "bgr0|gbrp|yuv444p|nv12",
            "options": {}, "audio": {"codec": "pcm_s16le|flac"}},
"output": {"dir": "./run", "container": "mp4", "keepChunks": false}
```

Measured 2026-11-24 on captured frames (1920×1080, 30 fps, 90 frames of the
video-plus-oscillator page, Mesa D3D12, `dcap` encoder thread, release
build):

| Codec (input) | Encode ms/frame | KB/frame | Lossless to the hash | Note |
| --- | --- | --- | --- | --- |
| `ffv1` (`bgr0`, level 3, 16 slices) | 6.0 | 162 | yes | smallest; below the engine's frame time, so off the critical path |
| `utvideo` (`gbrp`) | 0.5 | 855 | yes | MOV only |
| `magicyuv` (`gbrp`) | 0.4 | 858 | yes | MOV only |
| `ffvhuff` (`bgra`, plane prediction) | 1.4 | 2074 | yes | MOV only; largest lossless |
| `rawvideo` (`bgra`) | 4.0 (copy) | 8106 | yes | 747 MB for 3 s |
| `h264_nvenc` (`yuv444p`, lossless tune, intra-only) | 2.0 | 169 | no (4:4:4 YUV) | hardware session per worker |
| `hevc_nvenc` (`yuv444p`, lossless tune, intra-only) | 1.6 | 180 | no (4:4:4 YUV) | hardware session per worker |

Wall time was 3.8-4.9 s for every codec: the engine, not the encoder, sets
the pace at 1080p. Decision: FFV1 `bgr0` stays the default (one fifth the
size of the fast lossless codecs, and its 6 ms sits under the engine's
beginFrame time); `utvideo` or `magicyuv` when encoder CPU must be minimal
and size does not matter; NVENC when a YUV deliverable is wanted directly.
`dcap codecs` reports all seven as available on this workstation.

### 13.6 Stages and gates

| Stage | Change | Gate |
| --- | --- | --- |
| 1 (done) | Engine path (13.2): BGRA canonical form, in-place hash, no UI-thread copy | Run-to-run identical hashes; 1080p gap 3.4 ms, 4K gap 12 ms (hash-bound, see 13.2) |
| 2 (done) | `shm` sink in the browser; the encoder lives in the `dcap` binary itself (not a separate helper) reading the ring; fake engine writes the ring | No ffmpeg process in a run; `dcap` RSS 229 MB at 1280×720 including the encoder; hashes identical to the FIFO path |
| 3 (done, video) | Join in `dcap`, controller spec and tests rewritten in Rust, FIFO path and Python deleted | Six end-to-end tests pass against the fake engine; exact 60000/1001 timestamps on a chunked run. Audio in chunks landed with patch 8: PCM rides in the ring slots, chunks carry it, joins assert exact sample counts (section 14.3) |
| 4 (done) | Codec matrix on captured frames (13.5); NVENC availability reporting | Measured at 1080p; FFV1 `bgr0` stays the default; `dcap codecs` reports all seven encoders |

### 13.7 Rejected alternatives

- In-engine encoding with Chromium's bundled ffmpeg (first draft): see 13.3.
- A separate `dcap-encode` helper process (the reviewed proposal): the owner
  chose one binary, so the encoder thread runs inside `dcap` and the ring
  is consumed in process; the helper's isolation argument (encoder crash
  does not take the controller down) is covered by typed `EncoderFailed`
  errors from the shim and by the chunk retry.
- Orchestration inside `headless_shell` or a browser launched only to remux:
  a 70-minute build as the debugging loop for a 200-line avformat routine.
- GPU-process NVENC zero-copy: no CUDA interop with GL or D3D12 textures
  under WSL2 (no Vulkan ICD, OpenGL-CUDA interop unsupported); NVENC is fed
  from host memory either way. Windows `D3D12VideoEncodeAccelerator` and
  Media Foundation expose no lossless mode: delivery only.
- viz `I420_PLANES`/`NV12` readback: deliverable only on the viz sequence;
  the only client is `FrameSinkVideoCapturer`, whose delivery is not tied to
  the BeginFrame ack. Kept as prototype 2 for the delivery path; moot for an
  RGB archive.
- Status quo with ffmpeg flags: cannot leave the CLI's 0.8 GB and keeps the
  Annex-B concat path the operations review found fragile.

### 13.8 Review record for section 13

Build review verified the inventory of 13.3 line by line and found the
separate ffmpeg repository, the missing sources, the absent I/O protocol and
the NVENC header gap. Data-path review found that `beginFrameMs` includes
readback, the UI-thread canonical copy, the double swizzle, the FFV1
worst-case packet allocation and the throughput-only nature of the FIFO
cost. Operations review measured the Matroska timecode rounding, the exact
MP4 join, and specified the index-based timestamps, chunk audio and the
join rule. The adversarial review argued that the first draft optimized the
wrong component, measured the CLI's 0.8 GB floor and the faster RGB intra
codecs, and proposed the shared-memory helper that 13.3 adopts.

## 14. Renderer agent, media and audio (implemented 2026-11-24/25)

Patches 6-8 plus the RNG hook and the use-counter detection of 9-10 were
built on `dev-v2` after section 13. The contract of 4.3-4.9 stands; the
points below are where building it corrected the design.

### 14.0 Result

| Case | Result |
| --- | --- |
| Marker page (rAF `performance.mark`, a 200 ms timer mark), 60 fps, stop on `dcap:test:done` | Marks attributed to frames 10, 11 and 25 as the page intended; `repeat` verification equal |
| Muted looping WebM (VP8, 30 fps capture, 120 frames) | Run-to-run identical; two chunks with pre-roll equal to the serial render frame by frame (`full`) |
| WebM with Vorbis audio plus a Web Audio oscillator, 60000/1001, 150 frames in three chunks | Video and PCM hashes equal to the plan pass for every frame (300 comparisons); join holds exactly 120120 sample frames with `1001/60000` frame durations; `video.currentTime` reads exactly `n/30` at frame n at 30 fps |
| Prepare cost per frame | 0.5-1.2 ms including the media-thread round trips, audio render and event-queue flush |

### 14.1 Virtual time stops the main thread: the fence

Blink's `ThreadSchedulerBase::SetVirtualTimeStopped(true)` inserts a fence on
every main-thread task queue that cannot run while virtual time is paused
(`MainThreadSchedulerImpl::OnVirtualTimePaused`), and again on queues created
while stopped (`OnTaskQueueCreated`). Under policy `pause`, and under
`pauseIfNetworkFetchesPending` whenever any pauser is held, no main-thread
task runs: media pipeline completions (`OnPipelineStarted`, data-source
reads, `canplay`) wait for the next grant and land at a real-time-dependent
point of it, and a pauser held across a transition deadlocks because its
settle callback is itself fenced. A media element's lazily created
`kInternalMedia` queue stayed fenced forever once created while stopped.
In capture mode the fence is not inserted at either place; delayed tasks
remain gated by virtual time, so the clock is still frozen between grants
while real work completes. This is the change that makes 4.3 steps 3-4
possible.

### 14.2 Media transitions hold virtual time

With the fence gone, `WebMediaPlayerImpl` holds a `kInstant` pauser from each
transition request until a media-thread round trip finds no pending
`PipelineController` operation (new `HasPendingOperation()`), no seek, no
suspend/resume cycle, and, for a running unsuspended pipeline,
`readyState >= HAVE_FUTURE_DATA`. The last condition pins the preroll:
without it `canplay` and the autoplay `play()` raced the first BeginFrame
(frame 0 or 1 by decoder speed). `loadedmetadata`, `canplay`, `play` and
`playing` all land in frame 0's grant at the virtual time of the load.
`MultiBufferDataSource`'s 20 ms seek delay is zero in capture mode because
a delayed task cannot fire while the pauser holds the clock.

### 14.3 Audio

- `AudioRendererImpl` outputs at the capture device rate through the
  `AudioBufferConverter` (non-MSE playback otherwise keeps the stream rate,
  which put a resampler and a buffer-sized FIFO between the clock and the
  capture). A media element's sink is rendered as one request of exactly
  `S(n+1) − S(n)` frames with zero reported delay, so `AudioClock` and
  `video.currentTime` read exactly `T_n`; Web Audio destinations render
  128-frame quanta into a FIFO reported as output delay, so
  `AudioContext.currentTime` is the first quantum boundary at or past `T_n`
  (`--deterministic-capture-audio=RATE,CHANNELS,128`).
- The mix is a float sum in sink creation order, channel-mixed to the device
  layout; PCM crosses mojo as `array<float>` (12.8 KB per 30 fps frame at 48
  kHz stereo), the browser converts to `s16` or `f32`, hashes it
  (`pcmFrameHash`), chains `pcmHash` and writes it beside the video into the
  ring slot. Chunk audio pts are `S(start+n) − S(start)`, not `S(n)`: at
  60000/1001 the intervals are 800 or 801 frames and the local formula lost a
  sample at chunk boundaries. PCM audio moves the default container from MP4
  to MOV.
- `AudioRendererImpl::Render` waits for decoded audio instead of writing
  silence; `VideoRendererImpl::Render` waits until the queued frames cover
  the deadline. Both waits are cancelled before the sink pauses or stops.

### 14.4 Randomness and unsupported APIs

`base::subtle::SetRandBytesOverride` (new header
`base/rand_util_deterministic_capture.h`, so `rand_util.h` is untouched)
replaces the OS entropy source of a capture renderer with a seeded
xoshiro256** stream (`--deterministic-capture-seed`, from
`determinism.seed`); `crypto.getRandomValues`, `crypto.randomUUID` and blob
URLs repeat. V8's `Math.random` was already seeded through `--js-flags`.
The agent observes use counters (`RenderFrameObserver::DidObserveNewFeatureUsage`)
for getUserMedia, getDisplayMedia, RTCPeerConnection, MediaRecorder,
EncryptedMedia, captureStream, WebCodecs, WebGPU, ServiceWorker,
SharedWorker and setSinkId, and the audio factory reports WebRTC and
MediaStreamTrack sinks; each is reported once per document as
`UnsupportedApiUsed: <api>` in the next response's warnings, or as the error
`UnsupportedApiUsed{api}` when `media.failOnUnsupportedApi` is true (the
default).

### 14.5 Open after this round

Pausers for image decode, `decodeAudioData` and IndexedDB completions and
dedicated-worker auto-attach (patch 9); protocol and browser tests (patch
11); `beginFrameMs` dominates 4K, so the frame hash is no longer the
bottleneck and its replacement is deferred. Blob URL identifiers come from
the browser process and still vary between runs; a page that renders them
is not reproducible.

### 14.6 Review of patches 6-8

A code review (one agent, read-only, 2026-11-24) found ten defects, all
fixed in the patch 9-10 commit: a failed pipeline start left `seeking_` set
and held the pauser forever (now released in `OnError`, an errored network
state counts as stable, and any hold is dropped after 20 s real time); muted
elements were routed to the real-time `NullAudioSink`, so their clock was
not pinned and the audio wait could block the media thread (the capture sink
is now always used and applies the volume); the audio engine mixed through
raw sink pointers that the media thread could free (refcounted list, removal
on `Stop()`); the video wait could never cover the deadline for sources
faster than three times the capture rate (reads bypass the soft cap during
the wait); `audioSampleFrames` counted PCM the ring never received (counted
on slot write); the resolved `markers`/`media`/`audio` defaults were dropped
from `effective`; the ring geometry check ignored the audio area; the
browser's prepare timer was shorter than the renderer's bounds; the audio
wait created a `WeakPtr` on the wrong thread. The reviewer confirmed the
agent lifetime handling, the settle loop, mark attribution, the `S(n+1)`
arithmetic, the ring publication order, the s16 conversion and that every
change is behind the capture switch.

The upstream touch budget of section 7 (300 lines) is exceeded: about 650
changed lines in 34 existing files (`patches/UPSTREAM_TOUCHES.txt`), two
thirds of them in `web_media_player_impl.cc`, `audio_renderer_impl.cc` and
`video_renderer_impl.cc`.

