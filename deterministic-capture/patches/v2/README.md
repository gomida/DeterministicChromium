# v2 browser patches

`wip-patches-1-5.diff` is `git diff --cached` of the `dev-v2` branch of the
Chromium checkout against tag 158.0.8127.88: patches 1-5 of DESIGN.md section
7 (switch and implied flags, the `DeterministicCapture` domain and
`VirtualTimeOwned` filter, `CaptureSession` with the internal virtual-time
client, `advance`/`advanceRange`/`finish` over BeginFrame and readback, the
canonical BGRA form hashed in place and the shared-memory ring sink), plus the
two determinism fixes in Blink (`document_load_timing.cc` snaps
`navigationStart` to virtual time; `time_clamper.cc` uses a constant secret
under the switch). Regenerate from the checkout with
`git add -A headless third_party/blink/public/devtools_protocol <blink files> && git diff --cached`.
The build arguments the patched browser needs are in `../../build/*.gn`
(`angle_enable_gl`, `angle_use_wayland`).
