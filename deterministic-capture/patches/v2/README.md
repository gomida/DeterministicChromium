# v2 browser patches

`git format-patch` output of the `dev-v2` branch of the Chromium checkout on
top of tag `158.0.8127.88`; one file per commit, in order:

| File | Content |
| --- | --- |
| `0001-headless-add-deterministic-capture-switch-*.patch` | `--deterministic-capture` switch and the flags it implies (patch 1) |
| `0002-headless-DeterministicCapture-domain-*.patch` | CDP domain, `CaptureSession`, virtual-time owner, canonical BGRA hash, shm/file/null frame sinks (patches 2-5) |
| `0003-blink-pin-navigationStart-and-TimeClamper-*.patch` | Virtual-time `navigationStart` and a constant `TimeClamper` secret under the switch |
| `0004-headless-renderer-CaptureAgent-*.patch` | Renderer `CaptureAgent` over mojo: wake, media quiescence, event-queue flush, `performance.mark` markers (patch 6) |
| `0005-media-blink-deterministic-video-playback-*.patch` | Media transitions hold virtual time, no task-queue fences while virtual time is stopped, video render wait, submitter interval, no background render (patch 7) |
| `0006-headless-media-renderer-side-capture-audio-sinks-*.patch` | Capture audio device factory and sinks, PCM mix per frame into the ring, audio render wait (patch 8) |
| `0007-headless-base-blink-media-deterministic-renderer-ent-*.patch` | Seeded renderer entropy, use-counter detection of unsupported APIs, review fixes for 6-8 (patches 9-10) |
| `0008-headless-patch-version-2.1.0-p10.patch` | Patch version string |

Apply with `git am` on the tag; build with `../../build/x64-dev.gn` or the
official profiles (`angle_enable_gl` is required for the Mesa D3D12 path).
Regenerate with
`git format-patch -o deterministic-capture/patches/v2 158.0.8127.88..dev-v2`.

`../UPSTREAM_TOUCHES.txt` is `git diff --stat` of the series restricted to
files that exist upstream (new files excluded). It stands at about 650
changed lines in 34 files against the 300-line budget of DESIGN.md section 7;
the overrun is in `web_media_player_impl.cc`, `audio_renderer_impl.cc` and
`video_renderer_impl.cc`, where the capture waits and the transition pauser
had to live.
