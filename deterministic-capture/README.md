# Deterministic capture (v2)

Frame-exact capture of a web page into a video file with a patched headless
Chromium (`dev-v2`) and one native controller/encoder binary (`dcap`). The
design, contract and review record are in [`DESIGN.md`](DESIGN.md); the
controller's commands, spec and layout are in [`dcap/README.md`](dcap/README.md).

```text
deterministic-capture/
  DESIGN.md                      contract, decisions, review record
  README.md                      this file: layout and build
  build/x64-dev.gn               Chromium GN profiles (dev; official x64 and arm64)
  build/x64-official.gn, build/arm64-official.gn
  dcap/                          Rust crate: controller + libav shim + fake engine + tests
  ffmpeg/build.sh                static libav* from the checkout's third_party/ffmpeg (LGPL, NVENC via headers)
  third_party/nv-codec-headers/  vendored MIT headers for NVENC (include/ only)
  patches/v1/                    the V0.22 series, for reference
  patches/v2/                    diff snapshot of the dev-v2 tree
```

The browser patches live on the `dev-v2` branch of the Chromium checkout
(`~/DeterministicChromium/src`, tag 158.0.8127.88); this directory lives on
the `dev-doc` branch of the same repository and is checked out as a worktree.

## Build

### Browser

From a gclient-synced checkout with `depot_tools`:

```bash
cd ~/DeterministicChromium/src && git switch dev-v2
mkdir -p out/dev && cp ../DeterministicChromium-dev-doc/deterministic-capture/build/x64-dev.gn out/dev/args.gn
gn gen out/dev && autoninja -C out/dev headless_shell
```

`build/x64-dev.gn` keeps DCHECKs off without ThinLTO/CFI for patch iteration;
`build/x64-official.gn` is the measurement profile. Both turn off the headless
preference store and page commands, X11/Wayland, ALSA/PulseAudio, D-Bus, GIO,
udev, GTK and Qt, and turn on `angle_enable_gl` plus `angle_use_wayland`,
which ozone headless builds disable by default: without the first,
`--use-angle=gl*` has no EGL display type; without X11 or Wayland defines
ANGLE's generic `DisplayEGL` is not compiled for platform type 0
(`third_party/angle/src/libANGLE/Display.cpp`). The controller uses
`--use-angle=gles-egl`, which selects `DisplayEGL` by device type, so the
system Mesa EGL (and through it the d3d12 gallium driver) is used. Printing
and PDFium stay because `//chrome` test targets assert
`enable_print_preview`.

### dcap

```bash
cd deterministic-capture
CHROMIUM_SRC=~/DeterministicChromium/src ffmpeg/build.sh   # static libavcodec/libavformat/libavutil/libswscale -> ffmpeg/out/prefix
cd dcap && cargo build --release && cargo test              # -> target/release/dcap (about 6 MB; libc, libm, libgcc only)
```

The ffmpeg configure line enables exactly the pipeline's components: FFV1,
utvideo, magicyuv, ffvhuff, rawvideo, PCM and FLAC encoders, h264/hevc NVENC
(headers vendored, libraries loaded at run time), MP4/MOV/Matroska/WAV muxers,
the matching demuxers and the file protocol. License: LGPL 2.1.

## Run

```bash
dcap validate spec.json
dcap probe spec.json --browser ~/DeterministicChromium/src/out/dev/headless_shell
dcap render spec.json --browser ~/DeterministicChromium/src/out/dev/headless_shell
```

Outputs under `output.dir`: `capture.<mp4|mov>`, `capture-report.json`,
`hashes/`, and `run/<id>/work/` with `plan.json`, logs and profiles.

## Backends on this workstation (WSL2)

| `render.backend` | Browser flags | Needs | Status |
| --- | --- | --- | --- |
| `angle-gl-d3d12` | `--use-angle=gles-egl` + `GALLIUM_DRIVER=d3d12 MESA_LOADER_DRIVER_OVERRIDE=d3d12` | `angle_enable_gl=true` and `angle_use_wayland=true` in the build, Mesa d3d12, `/dev/dxg` | GPU path; renderer `ANGLE (Microsoft Corporation, D3D12 (NVIDIA GeForce RTX 4090), OpenGL ES 3.1)` |
| `swiftshader` | `--use-angle=swiftshader` | nothing | CPU fallback, slow at 4K |
| `angle-vulkan` | `--use-angle=vulkan` | a Vulkan ICD | none on WSL2 (no dzn ICD shipped) |

The probe fails with `BackendMismatch` when the GPU process's renderer string
does not match the requested backend, so a silent fallback to software
rendering cannot produce a run.
