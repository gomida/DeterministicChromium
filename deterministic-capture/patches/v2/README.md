# v2 browser patches

`git format-patch` output of the `dev-v2` branch of the Chromium checkout
against tag 158.0.8127.88:

| File | Content |
| --- | --- |
| `0001-headless-add-deterministic-capture-switch-*.patch` | Switch and implied flags, forwarding to renderers (DESIGN.md 4.1) |
| `0002-headless-DeterministicCapture-domain-CaptureSession-*.patch` | The CDP domain, `CaptureSession`, internal virtual-time client and `VirtualTimeOwned` filter, canonical BGRA hashed in place, `shm`/`file`/`null` sinks (patches 2-5 of section 7) |
| `0003-blink-pin-navigationStart-and-TimeClamper-*.patch` | Virtual-time `navigationStart` and a constant `TimeClamper` secret under the switch |

Apply with `git am` on the tag; build with `../../build/x64-dev.gn` or the
official profiles (`angle_enable_gl` and `angle_use_wayland` are required for
the Mesa D3D12 path). Regenerate with
`git format-patch -o deterministic-capture/patches/v2 158.0.8127.88..dev-v2`.
