# Native deterministic audio capture

V0.22 is V0.21's capture path plus browser-native deterministic audio. It keeps
V0.21's absolute `HeadlessExperimental.beginFrame` clock, parallel chunking,
fixed `2.0s` replay tail, final `0.5s` display tail, and boundary-at-chunk-start
ordering. It removes the audio DOM shim and source-WAV assembly from the
capture path.

## Reproducible environment

The validated host was Ubuntu 24.04 on the GPU server. Its relevant runtime
components were:

| component | validated configuration |
| --- | --- |
| CPU allocation | cores 0-13 for capture/build, leaving cores 14-15 free |
| GPU | NVIDIA RTX PRO 6000 Blackwell Server Edition |
| NVIDIA driver | 610.43.02 |
| Python | 3.12.3 |
| Playwright for Python | 1.61.0 |
| ffmpeg / ffprobe | Ubuntu ffmpeg 6.1.1 |
| video encoder | `h264_nvenc` |
| graphics path | NVIDIA Vulkan ICD through `/usr/share/vulkan/icd.d/nvidia_icd.json` |

The Python runner requires only Playwright beyond the standard library. It
uses the custom Chromium executable, so `playwright install` is not required:

```bash
python3 -m venv "$HOME/deterministic-audio-test-venv"
"$HOME/deterministic-audio-test-venv/bin/python" -m pip install \
  'playwright==1.61.0'
ffmpeg -hide_banner -encoders 2>/dev/null | grep h264_nvenc
```

`ffmpeg` and `ffprobe` must be on `PATH`. The default command also requires an
ffmpeg build that exposes `h264_nvenc`; selecting a non-NVENC encoder requires
matching encoder arguments rather than only changing `CAPTURE_VIDEO_ENCODER`.
The NVIDIA Vulkan loader and ICD must be installed for V0.21's inherited GPU
flags. `ldd headless_shell` must not report missing libraries.

The capture client is a single self-contained script. It does not import a
previous capture version or install a legacy control shim:

```text
deterministic-capture/
  README.md
  capture.py
  build/x64-official.gn
  build/arm64-official.gn
```

## Chromium source and release build

The browser patch is maintained at:

```text
repository: https://github.com/gomida/DeterministicChromium.git
branch:     dev-deterministic-native-audio
validated:  291e6fd347310dd910adc74d73435af83ce995c9
```

The branch includes V0.21's native deterministic-video patch and V0.22's
browser-owned deterministic audio output. A normal non-debug Chromium build is
not sufficient for performance: Chromium enables `DCHECK` in non-official
Release builds. Use the same official-like profile as V0.2/V0.21 in a separate
output directory:

```gn
is_debug = false
is_component_build = false
is_official_build = true
dcheck_always_on = false
symbol_level = 0
blink_symbol_level = 0
v8_symbol_level = 0
proprietary_codecs = true
ffmpeg_branding = "Chrome"
rtc_use_h264 = true
use_sysroot = true
use_remoteexec = false
treat_warnings_as_errors = false
chrome_pgo_phase = 0
```

`is_official_build = true` is the material difference from the initial V0.22
build. With this Chromium revision on Linux x64 it disables release `DCHECK`s
and enables the default ThinLTO and CFI configuration. `chrome_pgo_phase = 0`
keeps PGO disabled, so this is the V0.2/V0.21 official-like experiment profile,
not Google's production Chrome profile.

From an already synchronized Chromium checkout with Linux build dependencies
and `depot_tools` installed:

```bash
cd "$HOME/DeterministicChromium/src"
git switch dev-deterministic-native-audio

mkdir -p out/deterministic-capture-official
"${EDITOR:-vi}" out/deterministic-capture-official/args.gn
buildtools/linux64/gn gen out/deterministic-capture-official
taskset -c 0-13 "$HOME/depot_tools/autoninja" \
  -C out/deterministic-capture-official headless_shell
```

Write the GN block above to
`out/deterministic-capture-official/args.gn` before `gn gen`. Keeping a
new output directory prevents development objects built with
`is_official_build = false` from being mistaken for release results. Verify the
effective values before benchmarking:

```bash
for arg in is_debug is_official_build dcheck_always_on use_thin_lto is_cfi; do
  buildtools/linux64/gn args out/deterministic-capture-official \
    --list="$arg" --short
done
ldd out/deterministic-capture-official/headless_shell | grep 'not found'
```

The expected effective values are `is_debug=false`,
`is_official_build=true`, `dcheck_always_on=false`, `use_thin_lto=true`, and
`is_cfi=true`. The final `ldd` command should print nothing.

The validated x64 official build completed on 2026-07-14 in `1:50:01` using
CPU cores 0-13. It produced a 316 MiB `headless_shell` with build ID
`4ca4b77434500acfa2488ded18850e8961f1e093`; `ldd` reported no missing
libraries. The build emitted four existing-style warnings in
`headless_handler.cc` for exit-time destructors and unsafe pointer arithmetic,
but exited successfully with `26332` completed steps.

Checked-in copies of the profiles are available at `build/x64-official.gn` and
`build/arm64-official.gn`. The arm64 profile differs only by
`target_cpu="arm64"` and uses a separate output directory:

```bash
cd "$HOME/DeterministicChromium/src"
mkdir -p out/deterministic-capture-arm64-official
"${EDITOR:-vi}" out/deterministic-capture-arm64-official/args.gn
buildtools/linux64/gn gen out/deterministic-capture-arm64-official
taskset -c 0-13 "$HOME/depot_tools/autoninja" \
  -C out/deterministic-capture-arm64-official headless_shell
```

Linux arm64 retains ThinLTO under this official-like profile but Chromium's
default CFI condition is limited to Linux x64, so the expected arm64 effective
value is `is_cfi=false`. Cross-built output must be staged with `icudtl.dat`,
V8 snapshots, headless resource pak files, and the selected EGL/Vulkan runtime
before it is tested on an arm64 host.

## Page and runtime contract

`CAPTURE_URL` must point to the published `index.html`. The same directory must
serve `audio_manifest.json` with a non-empty `pages` array and a positive
`duration_ms` for every page. The manifest durations only bound native timeline
measurement; source audio is decoded and mixed by Chromium from the page's
actual media URLs.

The page must use native `HTMLMediaElement.play()` and its normal `ended`
handlers to start the next source and advance the page. V0.22 supports one
active output-device format per browser process; its sample rate and channel
count must remain stable for the chunk. Cross-origin media must remain
fetchable by Chromium under the page's normal policy.

The audio pass and each video worker launch the custom browser with:

```text
--disable-audio-output
--disable-features=AudioServiceOutOfProcess,MediaSessionService
--enable-features=AudioOutputControllerRequestBeforeRead
--headless-raw-audio-dump=/path/to/audio.wav
```

The browser advances its fake output only from beginFrame, discards warm-up
audio, and writes the final output mix in its native sample rate/channel layout.
The UI-facing media-session service is disabled because its wall-clock position
comparison is incompatible with the renderer virtual-time clock used by V0.21;
it is not part of audio decoding, mixing, or the dumped final mix.

The custom experimental `beginFrame.audioOnly` parameter advances only the
browser-owned audio output. Its small `captureAudio` companion selects whether
that interval is recorded or discarded. One serial audio pass uses
`audioOnly=true, captureAudio=true` and produces the complete `audio.wav`.
Parallel video workers use `captureAudio=false`; they advance native audio to
drive the page's real `ended` handlers but never create chunk WAV files. These
calls do not enter the renderer, and the chunk path advances page timers
alongside them. Normal no-display
BeginFrames retain V0.21's renderer semantics only for the first `1.5s` of its
fixed replay tail, followed by `0.5s` of display BeginFrames. A boundary exactly
at a chunk start remains excluded from warm-forward and is applied before that
chunk's first display commit, matching V0.21's control ordering.
The recorder validates the single browser WAV and muxes it with the concatenated
video. Python never reads source audio or assembles audio from chunks.

Playwright's default `--mute-audio` argument is explicitly removed. The audio
service must run in the browser process because the deterministic controller
and headless beginFrame handler share browser-owned state.

The runner reads `audio_manifest.json` beside `CAPTURE_URL` only to obtain the
expected page count and a bounded capture horizon. Before parallel video
capture, the audio-only pass records the final mix and measures the frames where
the page's native `ended` handlers open the next source and where the final
source finishes. The browser WAV therefore includes native pipeline
start/transition silence.

The page observer wraps `HTMLMediaElement.play()` only to retain the otherwise
unattached `Audio` element and inspect readiness. It does not replace playback,
override media time, synthesize `ended`, or modify volume.

```bash
CAPTURE_URL='https://example.test/publish/index.html' \
CAPTURE_CHROME_EXECUTABLE="$HOME/DeterministicChromium/src/out/deterministic-capture-official/headless_shell" \
CAPTURE_WORK_DIR="$HOME/native-audio-capture" \
CAPTURE_PARALLELISM=6 \
CAPTURE_SUBCHUNKS_PER_SEGMENT=2 \
taskset -c 0-13 "$HOME/deterministic-audio-test-venv/bin/python" \
  deterministic-capture/capture.py
```

V0.22 defaults to the shown six-way configuration. Environment variables can
still override the parallelism and subchunk count. `CAPTURE_WIDTH`,
`CAPTURE_HEIGHT`, and `CAPTURE_FPS` default to 3840, 2160, and 60. The inherited
NVENC defaults are preset `p4`, tune `hq`, VBR, CQ 26, 5 Mbit/s target,
16 Mbit/s maximum, and a 20 Mbit buffer.

Outputs are written under `$CAPTURE_WORK_DIR/output/`:

- `audio.wav`: browser-generated signed 16-bit PCM final mix
- `video.mp4`: concatenated video-only capture
- `final.mp4`: video copy plus AAC-encoded browser audio
- `capture-report.json`: measured boundaries, frame counts, formats, and timing

`capture-report.json.timing` separates plan loading, browser audio capture,
parallel video capture plus mux, and total end-to-end wall time. The
end-to-end value starts before the manifest fetch and ends after the final MP4
has been muxed.

## Official x64 validation

The final-page fixture was validated with the official x64 build at 60 fps on
CPU cores 0-13. The native timeline was `[803, 1741]` with 2,669 total video
frames (44.483333 seconds). The browser-generated 24 kHz mono WAV contained
1,067,600 sample frames and had SHA-256
`47ed1c80268143d9c1ab450a13e184f70800eda3532e318c0eb6f35d0c0c051c`.

Two independent 320x180 runs produced the same timeline, chunk sizes
`[401, 402, 469, 469, 464, 464]`, and byte-identical `audio.wav`. The first run
reported `35.860s` instrumented end-to-end time and `35.98s` external wall
time; the second reported `34.869s` instrumented end-to-end time and `34.97s`
external wall time. Each output directory contained only `audio.wav` as a WAV
file; no worker `.discard_audio_XX.wav` files remained.

The selected six-chunk, six-way 3840x2160 run recorded:

- plan and manifest loading: `0.206s`
- browser audio capture: `9.427s`
- parallel video capture, encoding, concatenation, and mux: `40.572s`
- instrumented end-to-end wall time: `50.205s`
- external `/usr/bin/time` wall time: `50.31s`
- peak parent-process RSS reported by `/usr/bin/time`: `388,904 KiB`

The timed 4K `final.mp4` is 20,153,968 bytes with SHA-256
`095d9756dac9cb8d56da647781803555a9d11904c52f7e08460ab1be42eca39c`.
The corresponding `video.mp4` is 19,581,478 bytes with SHA-256
`e1e506bd4a06d6ef3ac8ae7f77b0e091b157a4ea8229cfac7b34b4f82ada7350`.
Repeated NVENC MP4 files are not byte-identical; deterministic checks apply to
the browser WAV and measured frame timeline before AAC encoding.

## Historical V0.21 parity sweep

The earlier V0.21 warm-forward restoration sweep used the initial
`out/deterministic-minimal/headless_shell`. Its effective settings were
`is_official_build=false`, `dcheck_always_on=true`, `use_thin_lto=false`, and
`is_cfi=false`, even though it used `is_debug=false` and `-O2`. Treat these
timings as functional and parallelism validation rather than the official-like
V0.22 performance baseline. The page-two to page-three samples at frames 1740,
1741, 1742, and 1770 no longer exposed the first page during the transition.

Parallelism comparison from that 4K60 sweep:

| chunks / parallelism | external wall time | versus three-way |
| --- | ---: | ---: |
| `3 / 3` | `64.72s` | baseline |
| `6 / 6` | `55.49s` | `14.3%` faster |
| `9 / 9` | `58.59s` | `9.5%` faster |

Six-way is retained as the default because nine-way adds browser and
warm-forward overhead and was `3.10s` slower than six-way.
