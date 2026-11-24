# V1 patch series (reference only)

`git format-patch` output of the V0.22 browser patches against Chromium
150.0.7871.46 (`5b586c06e0`). Patches 0001-0003 are the raw NV12 dump and
deterministic native audio (`dev-deterministic-native-audio`, tag
`deterministic-capture-v0.22`). Patches 0010-0013 are the native-video
substitution (`dev-deterministic-native-video`), which v2 deletes.

Kept so that the bounded async writer, the libyuv conversion and the WAV
writer can be ported to the v2 base; nothing here applies cleanly to a newer
Chromium without review. See `../../DESIGN.md`.
