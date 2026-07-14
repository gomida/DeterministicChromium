from __future__ import annotations

"""Deterministic parallel video capture with one browser-generated audio WAV."""

import asyncio
import json
import math
import os
import shutil
import subprocess
import threading
import time
import wave
from collections import deque
from pathlib import Path
from typing import Any
from urllib.parse import urljoin
from urllib.request import urlopen

from playwright.async_api import async_playwright


BASE = Path(
    os.getenv("CAPTURE_WORK_DIR", "tmp/deterministic-capture")
).resolve()
OUT = BASE / "output"
CHROME_TMP = BASE / "chromium-tmp"

WIDTH = int(os.getenv("CAPTURE_WIDTH", "3840"))
HEIGHT = int(os.getenv("CAPTURE_HEIGHT", "2160"))
FPS = int(os.getenv("CAPTURE_FPS", "60"))
PARALLELISM = max(1, int(os.getenv("CAPTURE_PARALLELISM", "6")))
SUBCHUNKS_PER_SEGMENT = max(
    1, int(os.getenv("CAPTURE_SUBCHUNKS_PER_SEGMENT", "2"))
)
MAX_CHUNK_SECONDS_VALUE = os.getenv(
    "CAPTURE_MAX_CHUNK_SECONDS", "auto"
).strip().lower()
MAX_CHUNK_SECONDS_AUTO = MAX_CHUNK_SECONDS_VALUE in {"auto", "median"}
MAX_CHUNK_SECONDS = (
    0.0
    if MAX_CHUNK_SECONDS_AUTO
    else max(0.0, float(MAX_CHUNK_SECONDS_VALUE or "0"))
)
DEFAULT_CHROME_EXECUTABLE = (
    Path.home()
    / "DeterministicChromium"
    / "src"
    / "out"
    / "deterministic-capture-official"
    / "headless_shell"
)
CHROME_EXECUTABLE_VALUE = os.getenv("CAPTURE_CHROME_EXECUTABLE", "").strip()
CHROME_EXECUTABLE = (
    Path(CHROME_EXECUTABLE_VALUE).expanduser()
    if CHROME_EXECUTABLE_VALUE
    else DEFAULT_CHROME_EXECUTABLE
)
RAW_FORMAT = "nv12"
VTIME_INITIAL = 1_700_000_000
VTIME_STARVATION_COUNT = 1000
VTIME_GRANT_POLICY = "pauseIfNetworkFetchesPending"
CDP_TIMEOUT_SECONDS = float(os.getenv("CAPTURE_CDP_TIMEOUT", "10"))
FINAL_AUDIO_BITRATE = os.getenv("CAPTURE_AUDIO_BITRATE", "96k")
WARM_FORWARD_TAIL_SECONDS = 2.0
WARM_FORWARD_TAIL_FRAMES = max(0, math.ceil(WARM_FORWARD_TAIL_SECONDS * FPS))
WARM_FORWARD_DISPLAY_TAIL_SECONDS = 0.5
WARM_FORWARD_DISPLAY_TAIL_FRAMES = min(
    WARM_FORWARD_TAIL_FRAMES,
    max(0, math.ceil(WARM_FORWARD_DISPLAY_TAIL_SECONDS * FPS)),
)
CAPTURE_AFTER_COMMIT_GAP_MS = 0.01

CAPTURE_URL = os.getenv("CAPTURE_URL", "").strip()
if not CAPTURE_URL:
    raise RuntimeError("CAPTURE_URL is required")


def require_bin(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"required executable is missing from PATH: {name}")
    return path


def ffmpeg_bin() -> str:
    return require_bin("ffmpeg")


def ffprobe_bin() -> str:
    return require_bin("ffprobe")


def run(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip().splitlines()
        message = detail[-1] if detail else str(proc.returncode)
        raise RuntimeError(f"{' '.join(cmd)}\n{message}")


def ffprobe_frame_count(path: Path) -> int:
    proc = subprocess.run(
        [
            ffprobe_bin(),
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=nb_frames",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(path),
        ],
        text=True,
        capture_output=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or proc.stdout.strip())
    value = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
    if not value or value == "N/A":
        raise RuntimeError(f"ffprobe did not report frame count: {path}")
    return int(value)


def write_capture_plan(plan: dict[str, Any]) -> None:
    (BASE / "capture-plan.json").write_text(
        json.dumps(plan, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def nvenc_args() -> list[str]:
    return [
        "-c:v",
        os.getenv("CAPTURE_VIDEO_ENCODER", "h264_nvenc"),
        "-preset",
        os.getenv("CAPTURE_NVENC_PRESET", "p4"),
        "-tune",
        os.getenv("CAPTURE_NVENC_TUNE", "hq"),
        "-rc",
        os.getenv("CAPTURE_NVENC_RC", "vbr"),
        "-cq",
        os.getenv("CAPTURE_NVENC_CQ", "26"),
        "-b:v",
        os.getenv("CAPTURE_NVENC_BITRATE", "5000k"),
        "-maxrate",
        os.getenv("CAPTURE_NVENC_MAXRATE", "16000k"),
        "-bufsize",
        os.getenv("CAPTURE_NVENC_BUFSIZE", "20000k"),
        "-bf",
        "0",
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
    ]


def raw_ffmpeg_cmd(input_path: Path, output: Path, frames: int) -> list[str]:
    return [
        ffmpeg_bin(),
        "-hide_banner",
        "-loglevel",
        "warning",
        "-y",
        "-f",
        "rawvideo",
        "-pix_fmt",
        RAW_FORMAT,
        "-s:v",
        f"{WIDTH}x{HEIGHT}",
        "-framerate",
        str(FPS),
        "-i",
        str(input_path),
        "-frames:v",
        str(frames),
        *nvenc_args(),
        str(output),
    ]


class RawPipeEncoder:
    def __init__(self, input_path: Path, output: Path, frames: int) -> None:
        self.output = output
        self.frames = frames
        self.proc = subprocess.Popen(
            raw_ffmpeg_cmd(input_path, output, frames),
            stderr=subprocess.PIPE,
        )
        self.stderr = self.proc.stderr
        self.stderr_lines: deque[str] = deque(maxlen=80)
        self.thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self.thread.start()
        self.closed = False

    def _drain_stderr(self) -> None:
        if not self.stderr:
            return
        for line in iter(self.stderr.readline, b""):
            text = line.decode("utf-8", errors="replace").strip()
            if text:
                self.stderr_lines.append(text)

    def close(self, terminate: bool = False) -> None:
        if self.closed:
            return
        if terminate and self.proc.poll() is None:
            self.proc.terminate()
        try:
            rc = self.proc.wait(timeout=10 if terminate else None)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = self.proc.wait()
        self.thread.join(timeout=2)
        self.closed = True
        if not terminate and rc != 0:
            detail = self.stderr_lines[-1] if self.stderr_lines else ""
            raise RuntimeError(
                f"ffmpeg raw pipe failed ({rc}) for {self.output}: {detail}"
            )
        if not terminate:
            actual = ffprobe_frame_count(self.output)
            if actual != self.frames:
                raise RuntimeError(
                    f"frame count mismatch: {self.output} "
                    f"frames={actual} expected={self.frames}"
                )


def build_chunks(plan: dict[str, Any]) -> list[dict[str, Any]]:
    total_frames = max(1, math.ceil(float(plan["total_duration"]) * FPS))
    boundaries = [0]
    boundaries += [
        max(0, min(total_frames, math.ceil(int(ms) * FPS / 1000)))
        for ms in plan["chunk_boundaries_ms"]
    ]
    boundaries.append(total_frames)
    boundaries = sorted(set(boundaries))
    segment_lengths = [
        end - start
        for start, end in zip(boundaries, boundaries[1:])
        if end > start
    ]
    if MAX_CHUNK_SECONDS_AUTO and segment_lengths:
        median_frames = sorted(segment_lengths)[(len(segment_lengths) - 1) // 2]
        max_chunk_frames = (
            median_frames if max(segment_lengths) > median_frames * 1.5 else 0
        )
    elif MAX_CHUNK_SECONDS > 0:
        max_chunk_frames = math.ceil(MAX_CHUNK_SECONDS * FPS)
    else:
        max_chunk_frames = 0

    chunks: list[dict[str, Any]] = []
    for start, end in zip(boundaries, boundaries[1:]):
        if end <= start:
            continue
        parts = SUBCHUNKS_PER_SEGMENT
        if max_chunk_frames > 0:
            parts = max(parts, math.ceil((end - start) / max_chunk_frames))
        for part in range(parts):
            sub_start = start + math.floor((end - start) * part / parts)
            sub_end = start + math.floor((end - start) * (part + 1) / parts)
            if sub_end <= sub_start:
                continue
            chunks.append(
                {
                    "index": len(chunks),
                    "start_frame": sub_start,
                    "end_frame": sub_end,
                    "frames": sub_end - sub_start,
                    "start_ms": sub_start * 1000 / FPS,
                    "end_ms": sub_end * 1000 / FPS,
                }
            )
    return chunks


async def cdp_send(
    client: Any,
    method: str,
    params: dict[str, Any] | None = None,
    timeout: float = CDP_TIMEOUT_SECONDS,
) -> Any:
    return await asyncio.wait_for(client.send(method, params or {}), timeout=timeout)


async def grant_vtime(
    client: Any, vtime: dict[str, float], target_ms: float
) -> None:
    target = max(float(vtime["elapsed"]), target_ms)
    budget = target - float(vtime["elapsed"])
    if budget <= 0.001:
        return
    loop = asyncio.get_running_loop()
    expired = loop.create_future()

    def on_expired(params: dict[str, Any] | None = None) -> None:
        if not expired.done():
            expired.set_result(params or {})

    client.on("Emulation.virtualTimeBudgetExpired", on_expired)
    timeout = max(CDP_TIMEOUT_SECONDS, budget / 1000 + 5)
    try:
        await cdp_send(
            client,
            "Emulation.setVirtualTimePolicy",
            {
                "policy": VTIME_GRANT_POLICY,
                "budget": budget,
                "maxVirtualTimeTaskStarvationCount": VTIME_STARVATION_COUNT,
            },
            timeout=timeout,
        )
        await asyncio.wait_for(expired, timeout=timeout)
        vtime["elapsed"] = target
    finally:
        client.remove_listener(
            "Emulation.virtualTimeBudgetExpired", on_expired
        )


async def wait_until(
    client: Any,
    page: Any,
    vtime: dict[str, float],
    expression: str,
    budget_ms: int,
    step_ms: int = 100,
) -> bool:
    spent = 0
    while spent < budget_ms:
        if await page.evaluate(expression):
            return True
        grant = min(step_ms, budget_ms - spent)
        await grant_vtime(client, vtime, float(vtime["elapsed"]) + grant)
        spent += grant
    return bool(await page.evaluate(expression))


def begin_frame_ticks(vtime: dict[str, float], timestamp_ms: float) -> float:
    return float(vtime["base"]) + float(vtime["capture_offset"]) + timestamp_ms


async def begin_frame(
    client: Any,
    vtime: dict[str, float],
    timestamp_ms: float,
    *,
    no_display: bool = False,
    capture: bool = False,
    capture_audio: bool = False,
) -> Any:
    if no_display and capture:
        raise ValueError("no-display beginFrame cannot capture a raw frame")
    params: dict[str, Any] = {
        "frameTimeTicks": begin_frame_ticks(vtime, timestamp_ms),
        "interval": 1000 / FPS,
        "captureAudio": capture_audio,
    }
    if no_display:
        params["noDisplayUpdates"] = True
    if capture:
        params["screenshot"] = {
            "format": "jpeg",
            "quality": 92,
            "optimizeForSpeed": True,
        }
    return await cdp_send(client, "HeadlessExperimental.beginFrame", params)


async def begin_display_frame(
    client: Any, vtime: dict[str, float], timestamp_ms: float
) -> None:
    last = vtime.get("last_display_frame_ms")
    if last is not None and abs(float(last) - timestamp_ms) <= 0.0001:
        return
    await begin_frame(client, vtime, timestamp_ms)
    vtime["last_display_frame_ms"] = timestamp_ms


def ffconcat_escape(path: Path) -> str:
    return str(path.resolve()).replace("'", "'\\''")


def concat_mp4(paths: list[Path], output: Path) -> None:
    if not paths:
        raise RuntimeError("cannot concatenate an empty MP4 list")
    list_path = OUT / "chunks.ffconcat"
    list_path.write_text(
        "".join(f"file '{ffconcat_escape(path)}'\n" for path in paths),
        encoding="utf-8",
    )
    run(
        [
            ffmpeg_bin(),
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
            str(list_path),
            "-c",
            "copy",
            str(output),
        ]
    )


RENDER_READY_JS = """
(() => {
  const fontsReady = !document.fonts || document.fonts.status === "loaded";
  const imagesReady = Array.from(document.images || []).every((img) => img.complete && img.naturalWidth > 0);
  const videosReady = Array.from(document.querySelectorAll("video")).every((video) => {
    return video.currentSrc
      && video.videoWidth > 0
      && video.videoHeight > 0
      && video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA;
  });
  return document.readyState === "complete" && fontsReady && imagesReady && videosReady;
})()
"""

DOM_CLICK_JS = """
(() => {
  const target = document.elementFromPoint(1, 1) || document.body || document.documentElement || document;
  const pointer = {bubbles: true, cancelable: true, clientX: 1, clientY: 1, pointerId: 1, pointerType: "mouse", isPrimary: true};
  const mouse = {bubbles: true, cancelable: true, clientX: 1, clientY: 1, button: 0};
  for (const [type, EventCtor, init] of [
    ["pointerdown", globalThis.PointerEvent || MouseEvent, pointer],
    ["mousedown", MouseEvent, mouse],
    ["pointerup", globalThis.PointerEvent || MouseEvent, pointer],
    ["mouseup", MouseEvent, mouse],
    ["click", MouseEvent, mouse],
  ]) {
    try {
      target.dispatchEvent(new EventCtor(type, init));
    } catch {
      target.dispatchEvent(new Event(type, {bubbles: true, cancelable: true}));
    }
  }
})()
"""

AUDIO_OBSERVER_JS = """
(() => {
  const nativePlay = HTMLMediaElement.prototype.play;
  HTMLMediaElement.prototype.play = function(...args) {
    if (this instanceof HTMLAudioElement) {
      globalThis.__captureAudioElement = this;
    }
    return Reflect.apply(nativePlay, this, args);
  };
})()
"""

AUDIO_READY_JS = """
Boolean(
  globalThis.__captureAudioElement
  && !globalThis.__captureAudioElement.paused
  && globalThis.__captureAudioElement.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA
)
"""

AUDIO_STATE_JS = """
(() => {
  const audio = globalThis.__captureAudioElement;
  if (!audio) return null;
  return {
    currentTime: audio.currentTime,
    duration: audio.duration,
    paused: audio.paused,
    readyState: audio.readyState,
    src: audio.currentSrc || audio.src,
  };
})()
"""


def load_plan() -> dict[str, Any]:
    for path in (OUT, CHROME_TMP):
        if path.exists():
            shutil.rmtree(path)
    OUT.mkdir(parents=True, exist_ok=True)
    CHROME_TMP.mkdir(parents=True, exist_ok=True)

    manifest_url = urljoin(CAPTURE_URL, "audio_manifest.json")
    with urlopen(manifest_url, timeout=30) as response:
        manifest = json.load(response)
    pages = manifest.get("pages") if isinstance(manifest, dict) else None
    if not isinstance(pages, list) or not pages:
        raise RuntimeError(f"audio manifest has no pages: {manifest_url}")
    durations_ms = [int(page.get("duration_ms") or 0) for page in pages]
    if any(duration <= 0 for duration in durations_ms):
        raise RuntimeError(f"audio manifest has invalid durations: {manifest_url}")

    boundaries_ms: list[int] = []
    elapsed_ms = 0
    for duration_ms in durations_ms[:-1]:
        elapsed_ms += duration_ms
        boundaries_ms.append(elapsed_ms)
    total_duration_ms = sum(durations_ms)
    plan = {
        "url": CAPTURE_URL,
        "fps": FPS,
        "width": WIDTH,
        "height": HEIGHT,
        "audio_manifest_url": manifest_url,
        "audio_durations_ms": durations_ms,
        "chunk_boundaries_ms": boundaries_ms,
        "total_duration": total_duration_ms / 1000,
        "audio": {
            "mode": "browser-final-mix",
            "format": "pcm_s16le",
            "sample_rate": None,
            "channels": None,
            "format_policy": "first-active-output-stream",
        },
        "video": {
            "mode": "native-deterministic",
            "fps": FPS,
            "tmpdir": str(CHROME_TMP),
        },
    }
    write_capture_plan(plan)
    return plan


def browser_args(raw_pipe: Path | None, audio_path: Path) -> list[str]:
    args = [
        "--enable-logging=stderr",
        "--v=1",
        "--no-sandbox",
        "--disable-dev-shm-usage",
        "--deterministic-mode",
        "--enable-begin-frame-control",
        "--run-all-compositor-stages-before-draw",
        "--disable-new-content-rendering-timeout",
        "--disable-background-timer-throttling",
        "--disable-renderer-backgrounding",
        "--disable-threaded-animation",
        "--disable-threaded-scrolling",
        "--disable-checker-imaging",
        "--disable-image-animation-resync",
        "--enable-surface-synchronization",
        "--force-color-profile=srgb",
        "--js-flags=--random-seed=20260703",
        "--autoplay-policy=no-user-gesture-required",
        "--allow-file-access-from-files",
        "--enable-gpu",
        "--disable-gpu-sandbox",
        "--ignore-gpu-blocklist",
        "--use-angle=vulkan",
        "--enable-features=Vulkan,AudioOutputControllerRequestBeforeRead",
        "--disable-vulkan-surface",
        "--enable-unsafe-webgpu",
        "--enable-gpu-rasterization",
        "--enable-zero-copy",
        "--disable-audio-output",
        "--disable-features=AudioServiceOutOfProcess,MediaSessionService",
        f"--headless-raw-audio-dump={audio_path}",
    ]
    if raw_pipe is not None:
        args += [
            f"--deterministic-video-ffmpeg={ffmpeg_bin()}",
            f"--deterministic-video-fps={FPS}",
            f"--headless-raw-frame-dump={raw_pipe}",
            f"--headless-raw-frame-dump-format={RAW_FORMAT}",
            "--headless-raw-frame-dump-async",
        ]
    return args


def browser_launch_kwargs(
    raw_pipe: Path | None, audio_path: Path
) -> dict[str, Any]:
    if not CHROME_EXECUTABLE.exists():
        raise RuntimeError(
            f"required Chromium executable is missing: {CHROME_EXECUTABLE}"
        )
    env = dict(os.environ)
    env.setdefault("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/nvidia_icd.json")
    env.setdefault("__GLX_VENDOR_LIBRARY_NAME", "nvidia")
    env["TMPDIR"] = str(CHROME_TMP)
    # Playwright normally adds this default even when the caller did not. It
    # zeros renderer output before the browser-owned final-mix hook.
    return {
        "headless": True,
        "args": browser_args(raw_pipe, audio_path),
        "env": env,
        "executable_path": str(CHROME_EXECUTABLE),
        "ignore_default_args": ["--mute-audio"],
    }


async def setup_page(
    playwright: Any,
    plan: dict[str, Any],
    raw_pipe: Path | None,
    audio_path: Path,
) -> tuple[Any, Any, Any, Any, dict[str, float]]:
    browser = await playwright.chromium.launch(
        **browser_launch_kwargs(raw_pipe, audio_path)
    )
    context = await browser.new_context(
        viewport={"width": WIDTH, "height": HEIGHT},
        device_scale_factor=1,
    )
    page = await context.new_page()
    client = await page.context.new_cdp_session(page)
    await cdp_send(client, "Page.enable")
    await cdp_send(client, "Runtime.enable")
    await cdp_send(client, "HeadlessExperimental.enable")
    await cdp_send(
        client,
        "Page.addScriptToEvaluateOnNewDocument",
        {"source": AUDIO_OBSERVER_JS},
    )
    result = await cdp_send(
        client,
        "Emulation.setVirtualTimePolicy",
        {"policy": "pause", "initialVirtualTime": VTIME_INITIAL},
    )
    vtime = {
        "base": float(result.get("virtualTimeTicksBase") or 0),
        "elapsed": 0.0,
        "capture_offset": 0.0,
    }
    await cdp_send(client, "Page.navigate", {"url": plan["url"]})
    await grant_vtime(client, vtime, 1000 / FPS)
    dom_ready = await wait_until(
        client,
        page,
        vtime,
        'document.readyState !== "loading"',
        20_000,
    )
    render_ready = dom_ready and await wait_until(
        client, page, vtime, RENDER_READY_JS, 30_000
    )
    if not render_ready:
        raise RuntimeError("render readiness failed")

    await page.evaluate(DOM_CLICK_JS)
    if not await wait_until(
        client, page, vtime, AUDIO_READY_JS, 10_000, 50
    ):
        raise RuntimeError("native audio did not start after the capture click")
    await asyncio.sleep(0.1)

    # The deterministic clock begins after native playback is running. Every
    # chunk replays from this same point, then discards audio during warm-up.
    vtime["capture_offset"] = vtime["elapsed"]
    vtime["audio_frame"] = 0.0
    vtime["audio_src"] = await page.evaluate(
        "globalThis.__captureAudioElement.currentSrc"
    )
    await begin_display_frame(client, vtime, 0.0)
    return browser, context, page, client, vtime


def validate_audio_wav(path: Path, expected_video_frames: int) -> None:
    with wave.open(str(path), "rb") as reader:
        params = (reader.getframerate(), reader.getnchannels(), reader.getsampwidth())
        frames = reader.getnframes()
    sample_rate, channels, sample_width = params
    if sample_rate <= 0 or channels <= 0 or sample_width != 2:
        raise RuntimeError(f"unexpected WAV format: {path} {params}")
    expected_audio_frames = round(expected_video_frames * sample_rate / FPS)
    if frames != expected_audio_frames:
        raise RuntimeError(
            f"WAV length mismatch: {path} frames={frames} "
            f"expected={expected_audio_frames}"
        )


def transition_frames(plan: dict[str, Any]) -> set[int]:
    measured = plan.get("chunk_boundary_frames")
    if not isinstance(measured, list):
        raise RuntimeError("audio timeline has no measured boundary frames")
    return {int(frame) for frame in measured}


async def settle_audio_transition(
    page: Any, client: Any, vtime: dict[str, float]
) -> None:
    previous_src = str(vtime.get("audio_src") or "")
    current_state = await page.evaluate(AUDIO_STATE_JS)
    if (
        current_state
        and str(current_state.get("src") or "") != previous_src
        and not current_state.get("paused")
        and int(current_state.get("readyState") or 0) >= 2
    ):
        vtime["audio_src"] = str(current_state["src"])
        return
    # The media renderer posts `ended` to the page task queue after the output
    # pull reaches EOF. Let that task run, then wait in real time for the next
    # native source to open and become decodable.
    await grant_vtime(client, vtime, float(vtime["elapsed"]) + 1.0)
    await asyncio.sleep(0.25)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        ready = await page.evaluate(
            """
            previousSrc => Boolean(
              globalThis.__captureAudioElement
              && globalThis.__captureAudioElement.currentSrc !== previousSrc
              && !globalThis.__captureAudioElement.paused
              && globalThis.__captureAudioElement.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA
            )
            """,
            previous_src,
        )
        if ready:
            vtime["audio_src"] = await page.evaluate(
                "globalThis.__captureAudioElement.currentSrc"
            )
            return
        await asyncio.sleep(0.05)
        await grant_vtime(client, vtime, float(vtime["elapsed"]) + 1.0)
    raise RuntimeError(f"native audio transition did not leave {previous_src}")


async def begin_audio_frame(
    client: Any,
    vtime: dict[str, float],
    timestamp_ms: float,
    *,
    capture: bool = False,
) -> Any:
    return await cdp_send(
        client,
        "HeadlessExperimental.beginFrame",
        {
            "frameTimeTicks": begin_frame_ticks(vtime, timestamp_ms),
            "interval": 1000 / FPS,
            "audioOnly": True,
            "captureAudio": capture,
        },
    )


async def capture_audio_timeline(plan: dict[str, Any]) -> None:
    audio_output = OUT / "audio.wav"
    audio_output.unlink(missing_ok=True)
    browser = context = None
    boundaries: list[int] = []
    page_count = len(plan["audio_durations_ms"])
    manifest_total_frames = math.ceil(
        sum(plan["audio_durations_ms"]) * FPS / 1000
    )
    max_frames = manifest_total_frames + page_count * FPS * 2
    try:
        async with async_playwright() as playwright:
            browser, context, page, client, vtime = await setup_page(
                playwright, plan, None, audio_output
            )
            page_index = 0
            for frame_index in range(max_frames):
                timestamp_ms = frame_index * 1000 / FPS
                await begin_audio_frame(
                    client, vtime, timestamp_ms, capture=True
                )
                completed_frames = frame_index + 1
                state = await page.evaluate(AUDIO_STATE_JS)
                if not state:
                    continue
                current_time = float(state["currentTime"])
                duration = float(state["duration"])
                if not math.isfinite(duration) or current_time + 1e-6 < duration:
                    continue
                if page_index == page_count - 1:
                    plan["total_frames"] = completed_frames
                    break
                await settle_audio_transition(page, client, vtime)
                boundaries.append(completed_frames)
                page_index += 1
            else:
                raise RuntimeError(
                    f"native audio timeline exceeded {max_frames} frames"
                )
            await context.close()
            await browser.close()
            context = browser = None
    finally:
        if context:
            await context.close()
        if browser:
            await browser.close()

    total_frames = int(plan["total_frames"])
    validate_audio_wav(audio_output, total_frames)
    with wave.open(str(audio_output), "rb") as reader:
        plan["audio"]["sample_rate"] = reader.getframerate()
        plan["audio"]["channels"] = reader.getnchannels()
    plan["chunk_boundary_frames"] = boundaries
    plan["chunk_boundaries_ms"] = [frame * 1000 / FPS for frame in boundaries]
    plan["total_duration"] = total_frames / FPS
    plan["audio_timeline"] = {
        "boundary_frames": boundaries,
        "total_frames": total_frames,
        "captured_with_begin_frame": True,
        "path": str(audio_output),
    }
    write_capture_plan(plan)


async def begin_capture_display_frame(
    client: Any, vtime: dict[str, float], timestamp_ms: float
) -> None:
    last = vtime.get("last_display_frame_ms")
    if last is not None and abs(float(last) - timestamp_ms) <= 0.0001:
        return
    await begin_frame(client, vtime, timestamp_ms)
    vtime["last_display_frame_ms"] = timestamp_ms
    vtime["audio_frame"] = max(
        float(vtime.get("audio_frame") or 0),
        timestamp_ms * FPS / 1000,
    )


async def capture_frame(
    page: Any,
    client: Any,
    vtime: dict[str, float],
    plan: dict[str, Any],
    frame_index: int,
) -> None:
    timestamp_ms = frame_index * 1000 / FPS
    await advance_audio_to(
        page,
        client,
        vtime,
        plan,
        timestamp_ms,
        include_target_boundary=True,
    )
    await begin_capture_display_frame(client, vtime, timestamp_ms)
    await grant_vtime(
        client,
        vtime,
        float(vtime["capture_offset"])
        + timestamp_ms
        + CAPTURE_AFTER_COMMIT_GAP_MS,
    )
    await begin_frame(
        client,
        vtime,
        timestamp_ms + CAPTURE_AFTER_COMMIT_GAP_MS,
        capture=True,
    )
    vtime["last_display_frame_ms"] = timestamp_ms + CAPTURE_AFTER_COMMIT_GAP_MS


async def advance_audio_to(
    page: Any,
    client: Any,
    vtime: dict[str, float],
    plan: dict[str, Any],
    target_ms: float,
    *,
    include_target_boundary: bool,
) -> None:
    boundaries = sorted(transition_frames(plan))
    target_frame = target_ms * FPS / 1000
    epsilon = 1e-6
    last_frame = math.floor(target_frame + epsilon)
    if (
        not include_target_boundary
        and abs(last_frame - target_frame) <= epsilon
    ):
        last_frame -= 1
    current_frame = math.floor(
        float(vtime.get("audio_frame") or 0) + epsilon
    )
    boundary_set = set(boundaries)
    for frame_index in range(current_frame + 1, last_frame + 1):
        frame_ms = frame_index * 1000 / FPS
        await begin_audio_frame(client, vtime, frame_ms)
        vtime["audio_frame"] = float(frame_index)
        await grant_vtime(
            client,
            vtime,
            float(vtime["capture_offset"]) + frame_ms,
        )
        if frame_index in boundary_set:
            await settle_audio_transition(page, client, vtime)
    await grant_vtime(
        client,
        vtime,
        float(vtime["capture_offset"]) + target_ms,
    )


async def render_pulse(
    page: Any,
    client: Any,
    vtime: dict[str, float],
    plan: dict[str, Any],
    timestamp_ms: float,
    *,
    no_display: bool,
) -> None:
    await advance_audio_to(
        page,
        client,
        vtime,
        plan,
        timestamp_ms,
        include_target_boundary=True,
    )
    if no_display:
        await begin_frame(client, vtime, timestamp_ms, no_display=True)
        vtime["audio_frame"] = max(
            float(vtime.get("audio_frame") or 0),
            timestamp_ms * FPS / 1000,
        )
        return
    await begin_capture_display_frame(client, vtime, timestamp_ms)


async def warm_forward(
    page: Any,
    client: Any,
    vtime: dict[str, float],
    plan: dict[str, Any],
    start_frame: int,
) -> None:
    # Jump to the start of the fixed replay tail, run the first part with
    # no-display BeginFrames, and run the
    # final 0.5 seconds with display BeginFrames so Viz surfaces materialize.
    # Native audio-only pulls advance the final mix while normal no-display
    # BeginFrames retain their renderer-side semantics.
    tail_start_frame = max(0, start_frame - WARM_FORWARD_TAIL_FRAMES)
    display_tail_start_frame = max(
        tail_start_frame,
        start_frame
        - min(
            WARM_FORWARD_TAIL_FRAMES,
            WARM_FORWARD_DISPLAY_TAIL_FRAMES,
        ),
    )
    await advance_audio_to(
        page,
        client,
        vtime,
        plan,
        tail_start_frame * 1000 / FPS,
        include_target_boundary=True,
    )
    for frame_index in range(tail_start_frame, start_frame):
        try:
            timestamp_ms = frame_index * 1000 / FPS
            await render_pulse(
                page,
                client,
                vtime,
                plan,
                timestamp_ms,
                no_display=frame_index < display_tail_start_frame,
            )
        except Exception as exc:
            raise RuntimeError(
                f"warm-forward failed at frame {frame_index}"
            ) from exc
    await advance_audio_to(
        page,
        client,
        vtime,
        plan,
        start_frame * 1000 / FPS,
        include_target_boundary=False,
    )


async def capture_chunk(
    playwright: Any,
    plan: dict[str, Any],
    chunk: dict[str, Any],
    semaphore: asyncio.Semaphore,
) -> dict[str, Any]:
    async with semaphore:
        started = time.perf_counter()
        index = int(chunk["index"])
        start_frame = int(chunk["start_frame"])
        end_frame = int(chunk["end_frame"])
        expected_frames = end_frame - start_frame
        chunk_path = OUT / f"chunk_{index:02d}.mp4"
        discard_audio = OUT / f".discard_audio_{index:02d}.wav"
        raw_pipe = OUT / f"chunk_{index:02d}.{RAW_FORMAT}.pipe"
        raw_pipe.unlink(missing_ok=True)
        discard_audio.unlink(missing_ok=True)
        os.mkfifo(raw_pipe)
        encoder = RawPipeEncoder(raw_pipe, chunk_path, expected_frames)
        browser = context = None
        completed = False
        try:
            browser, context, page, client, vtime = await setup_page(
                playwright, plan, raw_pipe, discard_audio
            )
            warm_started = time.perf_counter()
            await warm_forward(
                page,
                client,
                vtime,
                plan,
                start_frame,
            )
            warm_elapsed = time.perf_counter() - warm_started
            capture_started = time.perf_counter()
            for frame_index in range(start_frame, end_frame):
                try:
                    await capture_frame(
                        page, client, vtime, plan, frame_index
                    )
                except Exception as exc:
                    raise RuntimeError(
                        f"frame capture failed: chunk={index} "
                        f"frame={frame_index}"
                    ) from exc
            capture_elapsed = time.perf_counter() - capture_started
            await context.close()
            await browser.close()
            context = browser = None
            encoder.close()
            raw_pipe.unlink(missing_ok=True)
            if discard_audio.exists():
                raise RuntimeError(
                    f"video worker unexpectedly recorded audio: {discard_audio}"
                )
            completed = True
            return {
                "index": index,
                "path": str(chunk_path),
                "start_frame": start_frame,
                "end_frame": end_frame,
                "frames": expected_frames,
                "start_ms": chunk["start_ms"],
                "end_ms": chunk["end_ms"],
                "warm_forward_elapsed": warm_elapsed,
                "capture_loop_elapsed": capture_elapsed,
                "elapsed": time.perf_counter() - started,
            }
        finally:
            if context:
                await context.close()
            if browser:
                await browser.close()
            if not encoder.closed:
                encoder.close(terminate=not completed)
            raw_pipe.unlink(missing_ok=True)
            discard_audio.unlink(missing_ok=True)


async def capture(plan: dict[str, Any]) -> Path:
    started = time.perf_counter()
    chunks = build_chunks(plan)
    semaphore = asyncio.Semaphore(PARALLELISM)
    async with async_playwright() as playwright:
        reports = await asyncio.gather(
            *[
                capture_chunk(playwright, plan, chunk, semaphore)
                for chunk in chunks
            ]
        )
    reports.sort(key=lambda item: int(item["index"]))

    raw_video = OUT / "video.mp4"
    audio_output = OUT / "audio.wav"
    final = OUT / "final.mp4"
    concat_mp4([Path(item["path"]) for item in reports], raw_video)
    total_frames = sum(int(item["frames"]) for item in reports)
    if total_frames != int(plan["total_frames"]):
        raise RuntimeError(
            f"video length mismatch: frames={total_frames} "
            f"expected={plan['total_frames']}"
        )
    validate_audio_wav(audio_output, int(plan["total_frames"]))
    write_capture_plan(plan)
    run(
        [
            ffmpeg_bin(),
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(raw_video),
            "-i",
            str(audio_output),
            "-map",
            "0:v:0",
            "-map",
            "1:a:0",
            "-c:v",
            "copy",
            "-c:a",
            "aac",
            "-b:a",
            FINAL_AUDIO_BITRATE,
            "-shortest",
            str(final),
        ]
    )
    report = {
        "width": WIDTH,
        "height": HEIGHT,
        "fps": FPS,
        "parallelism": PARALLELISM,
        "chrome_executable": str(CHROME_EXECUTABLE),
        "audio": plan["audio"],
        "warm_forward_tail_seconds": WARM_FORWARD_TAIL_SECONDS,
        "warm_forward_display_tail_seconds": WARM_FORWARD_DISPLAY_TAIL_SECONDS,
        "chunks": reports,
        "total_duration": plan["total_duration"],
        "total_frames": total_frames,
        "elapsed": time.perf_counter() - started,
    }
    (OUT / "capture-report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return final


async def main() -> None:
    end_to_end_started = time.perf_counter()
    plan_started = time.perf_counter()
    plan = load_plan()
    plan_elapsed = time.perf_counter() - plan_started
    audio_started = time.perf_counter()
    await capture_audio_timeline(plan)
    audio_elapsed = time.perf_counter() - audio_started
    final = await capture(plan)
    end_to_end_elapsed = time.perf_counter() - end_to_end_started
    report_path = OUT / "capture-report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    report["timing"] = {
        "plan_load_elapsed": plan_elapsed,
        "audio_capture_elapsed": audio_elapsed,
        "capture_and_mux_elapsed": float(report["elapsed"]),
        "end_to_end_elapsed": end_to_end_elapsed,
    }
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(final)


if __name__ == "__main__":
    asyncio.run(main())
