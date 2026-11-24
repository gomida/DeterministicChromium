# dcap: deterministic capture controller and encoder

One native binary that drives the patched `headless_shell`, receives canonical
frames through a shared-memory ring, encodes them in process with the static
libav libraries built from the Chromium tree's ffmpeg source, verifies hashes
across browsers, joins chunk files by stream copy and writes the report.
Contract: `../DESIGN.md` (sections 4, 5 and 13). No Python, no external
ffmpeg process, no Playwright.

## Build

```bash
# 1. Static libav* from the Chromium checkout's third_party/ffmpeg (once per roll).
CHROMIUM_SRC=~/DeterministicChromium/src ../ffmpeg/build.sh      # -> ../ffmpeg/out/prefix
# 2. dcap (links the prefix; DCAP_FFMPEG_PREFIX overrides the location).
cargo build --release                                            # -> target/release/dcap
cargo test                                                       # unit tests + end-to-end against the fake engine
```

Requirements: Rust (stable, 2021 edition), gcc, make, nasm (the Chromium
checkout's `out/<dir>/nasm` is used when present). The release binary links
only libc, libm and libgcc dynamically; NVENC's `libnvidia-encode.so.1` and
`libcuda.so.1` are loaded at run time when an NVENC codec is selected.

## Commands

| Command | Purpose |
| --- | --- |
| `dcap validate SPEC` | Parse the spec, print the resolved values and whether a plan pass is needed |
| `dcap probe SPEC --browser PATH` | Capabilities, backend identity, encoder availability, pre-roll cost ratio |
| `dcap render SPEC --browser PATH [--run-id ID]` | Full run: probe, plan pass, chunk workers, verification, join, report |
| `dcap join --out FILE --container mp4 --fps 60000/1001 --start 0 CHUNK:FIRST_FRAME:FRAMES...` | Join existing chunk files by stream copy with index-based timestamps |
| `dcap codecs [--size WxH]` | Which compiled-in encoders open at this size, and which are lossless to the canonical hash |
| `dcap smoke --browser PATH [--backend B] [--frames N] [--size WxH] [--codec NAME]` | Development check: two runs through the real engine, hashes compared |
| `dcap versions` | dcap, libav and encoder inventory |

Exit codes: 0 complete, 1 failed (report still written), 2 usage or spec error.

## Spec

```json
{"url": "file:///path/index.html", "viewport": {"width": 1920, "height": 1080},
 "frameRate": {"num": 60000, "den": 1001}, "timeline": {"stop": {"type": "frames", "count": 600}},
 "render": {"backend": "angle-gl-d3d12"}, "output": {"dir": "./run"},
 "encoder": {"codec": "ffv1", "pixelFormat": "bgr0", "options": {"slices": 16}, "container": "mp4"},
 "parallel": {"workers": 4}, "verify": {"mode": "boundaries"}}
```

Full schema and defaults: `src/spec.rs` (`load_spec`) and DESIGN.md 5.1.
Unknown keys are errors. `audio: {"capture": true}` records the page's audio
(media elements and AudioContexts, mixed in the renderer at
`audio.deviceFormat`, default 48 kHz stereo, as `audio.sampleFormat` `s16` or
`f32`) into the chunks and the final file with exact sample counts, verified
per frame by `pcmFrameHash`; PCM audio moves the default container to `mov`
(`encoder.audioCodec`: `pcm_s16le`, `pcm_f32le`, `flac`). `media.waitTimeoutMs`
bounds the per-frame media quiescence wait (`media.failOnWaitTimeout`), and
`media.failOnUnsupportedApi` (default true) turns a use of getUserMedia,
WebRTC, MediaRecorder, EME, captureStream, WebCodecs, WebGPU, workers or
setSinkId into the error `UnsupportedApiUsed` instead of a warning.
`determinism.seed` seeds V8's `Math.random` and the renderer's entropy source
(`crypto.getRandomValues`, UUIDs, blob URLs). Markers are `performance.mark`
entries whose name starts with `markers.prefix` (default `dcap:`), attributed
to the frame in whose interval they were made. `encoder.codec` is one of `ffv1`, `utvideo`,
`magicyuv`, `ffvhuff`, `rawvideo`, `h264_nvenc`, `hevc_nvenc`; each has a
default pixel format, option set and container (`src/encode.rs`,
`codec_defaults`). Only RGB pixel formats (`bgr0`, `bgra`, `gbrp`, `gbrap`)
are lossless against the canonical hash; NVENC converts RGB to YUV 4:4:4 in
hardware. `utvideo`, `magicyuv` and `ffvhuff` need the `mov` container.
Measured at 1080p (DESIGN.md 13.5): FFV1 6 ms and 162 KB per frame, utvideo
and magicyuv 0.5 ms and about 855 KB, NVENC 2 ms and 170-180 KB; the engine,
not the encoder, sets the pace, so FFV1 stays the default.

## Outputs

`<output.dir>/capture.<ext>` (final file), `capture-report.json`,
`hashes/chunk_XX.sha256x8` (first eight bytes of every frame hash), and under
`run/<id>/work/`: `plan.json`, browser logs, profiles and (with `keepChunks`)
the chunk files.

## Layout

| Path | Contents |
| --- | --- |
| `src/cdp.rs`, `src/launch.rs` | CDP over `--remote-debugging-pipe`; browser launch with fds 3/4 and the ring fds 5/6/7 |
| `src/shm.rs` | Ring layout shared with `headless/lib/browser/capture/shm_ring.h`; consumer (`Ring`) and producer (`Producer`, fake engine) |
| `src/engine.rs` | `DeterministicCapture` client: state machine, pipelined hashes, typed errors |
| `src/session.rs`, `src/browser.rs` | One browser, one target: overrides, navigation, frames; launch flags per backend |
| `src/spec.rs`, `src/scenario.rs`, `src/scenarios/` | Spec schema; action log and compiled-in scenarios (`presentation`) |
| `src/plan.rs`, `src/render.rs`, `src/verify.rs`, `src/report.rs`, `src/cli.rs` | Plan pass, chunk workers with the encoder thread, verification, report, commands |
| `src/encode.rs`, `csrc/dcap_av.{h,c}` | libav shim: open/encode/finish, codec probe, chunk probe, stream-copy join |
| `src/bin/fake_engine.rs`, `tests/pipeline.rs` | Fake engine speaking the CDP contract and writing the ring; end-to-end tests |
