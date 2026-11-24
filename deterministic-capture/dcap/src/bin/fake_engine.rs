//! Fake engine for tests: a process that speaks the subset of CDP the
//! controller uses over `--remote-debugging-pipe` (fds 3/4), implements the
//! `DeterministicCapture` contract with synthetic frames, and publishes
//! canonical BGRA frames into the shared-memory ring exactly like the
//! browser's `RawFrameSink`. No Chromium involved.
//!
//! Environment knobs for tests:
//!   DCAP_FAKE_RENDERER      renderer string returned by the WebGL probe
//!   DCAP_FAKE_MARKER_FRAME  emits marker `dcap:test:done` at this frame
//!   DCAP_FAKE_DIVERGE       "NAME:F": the browser whose profile directory is
//!                           profile-NAME (c00a1 = chunk 0 attempt 1, plan,
//!                           probe) renders frame F differently
//!   DCAP_FAKE_CRASH         "NAME:F": that browser exits abruptly before
//!                           answering advance at frame F

use std::io::{BufRead, BufReader, Write};
use std::os::fd::{FromRawFd, OwnedFd};
use std::time::Duration;

use dcap::shm::Producer;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

struct Fake {
    out: std::fs::File,
    session_id: Option<String>,
    width: u32,
    height: u32,
    configured: bool,
    next_frame: i64,
    frames_written: i64,
    chain: String,
    fps_num: i64,
    fps_den: i64,
    expected_frames: Option<i64>,
    producer: Option<Producer>,
    file_sink: Option<std::fs::File>,
    marker_frame: Option<i64>,
    name: String,
    diverge: Option<(String, i64)>,
    crash: Option<(String, i64)>,
    pending_hashes: Vec<Value>,
    state: &'static str,
    // Audio device format from configure; PCM is synthesized per frame.
    audio_rate: i64,
    audio_channels: i64,
    audio_s16: bool,
    audio_total: i64,
    pcm_chain: String,
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn renderer() -> String {
    std::env::var("DCAP_FAKE_RENDERER").unwrap_or_else(|_| "ANGLE (fake, D3D12 (Fake GPU), fake)".into())
}

impl Fake {
    fn send(&mut self, v: Value) {
        let mut bytes = serde_json::to_vec(&v).unwrap();
        bytes.push(0);
        let _ = self.out.write_all(&bytes);
        let _ = self.out.flush();
    }

    fn event(&mut self, method: &str, params: Value) {
        let mut ev = json!({"method": method, "params": params});
        if let Some(s) = &self.session_id {
            ev["sessionId"] = json!(s);
        }
        self.send(ev);
    }

    fn frame_pixels(&self, index: i64) -> Vec<u8> {
        let (w, h) = (self.width as usize, self.height as usize);
        let mut px = vec![0u8; w * h * 4];
        let diverge = self.diverge.as_ref().map(|(name, frame)| *name == self.name && *frame == index).unwrap_or(false);
        let shift: u8 = if diverge { 1 } else { 0 };
        for y in 0..h {
            for x in 0..w {
                let o = (y * w + x) * 4;
                px[o] = ((x + index as usize) & 0xff) as u8;
                px[o + 1] = (y & 0xff) as u8;
                px[o + 2] = (((index * 7) & 0xff) as u8).wrapping_add(shift);
                px[o + 3] = 255;
            }
        }
        px
    }

    fn frame_time_us(&self, index: i64) -> i64 {
        let numerator = index as i128 * self.fps_den as i128 * 1_000_000;
        let q = numerator.div_euclid(self.fps_num as i128);
        let r = numerator.rem_euclid(self.fps_num as i128);
        (q + if 2 * r > self.fps_num as i128 { 1 } else { 0 }) as i64
    }

    /// Device sample frames in frames [0, index): floor(index * rate * den / num).
    fn samples_through(&self, index: i64) -> i64 {
        ((index as i128 * self.audio_rate as i128 * self.fps_den as i128) / self.fps_num as i128) as i64
    }

    /// Deterministic PCM for one frame: a function of the absolute sample position.
    fn frame_audio(&self, index: i64) -> (Vec<u8>, i64) {
        let start = self.samples_through(index);
        let frames = self.samples_through(index + 1) - start;
        let mut bytes = Vec::with_capacity((frames * self.audio_channels * if self.audio_s16 { 2 } else { 4 }) as usize);
        for k in 0..frames {
            let p = start + k;
            for c in 0..self.audio_channels {
                let v = (((p * 7919 + c * 1301) % 20000) - 10000) as f32 / 10000.0;
                if self.audio_s16 {
                    bytes.extend_from_slice(&((v * 32767.0).round() as i16).to_le_bytes());
                } else {
                    bytes.extend_from_slice(&v.to_le_bytes());
                }
            }
        }
        (bytes, frames)
    }

    fn advance_one(&mut self, capture_video: bool, capture_audio: bool, hash: bool) -> Result<Value, String> {
        let index = self.next_frame;
        if let Some((name, frame)) = &self.crash {
            if *name == self.name && *frame == index {
                std::process::exit(137);
            }
        }
        let pixels = self.frame_pixels(index);
        let digest: [u8; 32] = Sha256::digest(&pixels).into();
        let frame_hash = hex(&digest);
        let (audio, audio_frames) = self.frame_audio(index);
        let pcm_frame_hash = hex(&Sha256::digest(&audio));
        {
            let mut h = Sha256::new();
            h.update(self.pcm_chain.as_bytes());
            h.update(pcm_frame_hash.as_bytes());
            self.pcm_chain = hex(&h.finalize());
        }
        let ring_has_audio = self.producer.as_ref().map(|p| p.geometry.audio_format != 0).unwrap_or(false);
        let (slot_audio, slot_audio_frames): (&[u8], u32) = if capture_audio && ring_has_audio { (&audio, audio_frames as u32) } else { (&[], 0) };
        if capture_audio {
            self.audio_total += audio_frames;
        }
        if capture_video {
            if let Some(exp) = self.expected_frames {
                if self.frames_written >= exp {
                    return Err("IncompleteOutput: more frames than expectedFrames".into());
                }
            }
            if let Some(p) = &self.producer {
                if !p.publish(index, self.frame_time_us(index + 1), &pixels, Some(&digest), slot_audio, slot_audio_frames, Duration::from_secs(60)) {
                    return Err("SinkStalled: consumer did not release a ring slot".into());
                }
            } else if let Some(f) = &mut self.file_sink {
                f.write_all(&pixels).map_err(|e| format!("SinkClosed: {e}"))?;
            }
            self.frames_written += 1;
        }
        let mut completed = std::mem::take(&mut self.pending_hashes);
        if hash {
            let mut h = Sha256::new();
            h.update(self.chain.as_bytes());
            h.update(frame_hash.as_bytes());
            self.chain = hex(&h.finalize());
            completed.push(json!({"frameIndex": index, "frameHash": frame_hash, "chainHash": self.chain, "hashMs": 0.1, "sinkMs": 0.1}));
        }
        let markers = match self.marker_frame {
            Some(m) if m == index => vec![json!({"name": "dcap:test:done", "frameIndex": index, "source": "performanceMark"})],
            _ => vec![],
        };
        self.next_frame += 1;
        self.state = "capturing";
        Ok(json!({
            "frameIndex": index, "virtualTimeMs": self.frame_time_us(index + 1) as f64 / 1000.0, "hasSurface": true, "hasDamage": true,
            "reusedReadback": false, "surfaceWarmed": index == 0, "audioSampleFrames": audio_frames, "pcmFrameHash": pcm_frame_hash, "markers": markers, "media": [],
            "timings": {"grantMs": 0.1, "beginFrameMs": 0.5, "queueDepth": 0, "sinkQueued": 0}, "completed": completed, "warnings": [],
        }))
    }

    fn capabilities(&self) -> Value {
        json!({
            "protocolVersion": "2.1", "patchVersion": "fake-2.1.0", "modeEnabled": true,
            "chromium": {"version": "0.0.0.0", "lastChange": "fake"}, "build": {"official": false, "dcheck": false},
            "pixelFormats": ["bgra8-premul"], "colorSpaces": ["srgb"], "frameSinks": ["shm", "file", "null"],
            "audio": {"sampleFormats": ["s16"], "deviceFormat": {"rate": 48000, "channels": 2, "framesPerBuffer": 480}},
            "hash": ["sha256-bgra8-premul"],
            "features": [{"name": "pipelinedAdvance", "version": 1}, {"name": "advanceRange", "version": 1}, {"name": "shmSink", "version": 1}, {"name": "canonicalBgra", "version": 1}, {"name": "markers", "version": 1}, {"name": "prepare", "version": 1}, {"name": "audioRender", "version": 1}],
            "limits": {"maxTargets": 1, "maxQueuedFrames": 64},
            "gpu": {"glRenderer": renderer()},
        })
    }

    fn handle(&mut self, msg: Value) -> Result<Value, String> {
        let method = msg["method"].as_str().unwrap_or("");
        let params = &msg["params"];
        match method {
            "Target.getTargets" => Ok(json!({"targetInfos": [{"targetId": "page-1", "type": "page", "url": "about:blank"}]})),
            "Target.attachToTarget" => {
                self.session_id = Some("session-1".into());
                Ok(json!({"sessionId": "session-1"}))
            }
            "Browser.close" => {
                self.send(json!({"id": msg["id"], "result": {}}));
                std::process::exit(0);
            }
            "Page.navigate" => {
                self.event("Page.frameNavigated", json!({"frame": {"id": "frame-1", "url": params["url"], "loaderId": "l1"}}));
                self.event("Page.loadEventFired", json!({"timestamp": 0.0}));
                Ok(json!({"frameId": "frame-1", "loaderId": "l1"}))
            }
            "Page.getFrameTree" => Ok(json!({"frameTree": {"frame": {"id": "frame-1", "url": "about:blank"}}})),
            "Page.createIsolatedWorld" => Ok(json!({"executionContextId": 7})),
            "Runtime.evaluate" => {
                let expr = params["expression"].as_str().unwrap_or("");
                let value = if expr.contains("UNMASKED_RENDERER_WEBGL") {
                    json!(renderer())
                } else if expr.contains("document.fonts") || expr.contains("document.images") || expr.contains("querySelectorAll") || expr.contains("__dcapAudio") {
                    json!(true)
                } else {
                    json!(null)
                };
                Ok(json!({"result": {"type": "object", "value": value}}))
            }
            "DeterministicCapture.getCapabilities" => Ok(self.capabilities()),
            "DeterministicCapture.configure" => {
                let config = &params["config"];
                if self.state == "capturing" {
                    return Err("AlreadyStarted: capture already started".into());
                }
                if let Some(req) = config["requiredFeatures"].as_array() {
                    let have = ["pipelinedAdvance", "advanceRange", "shmSink", "canonicalBgra", "markers"];
                    for f in req {
                        if !have.contains(&f.as_str().unwrap_or("")) {
                            return Err(format!("UnknownFeature: required feature not available: {f}"));
                        }
                    }
                }
                self.fps_num = config["frameRate"]["num"].as_i64().unwrap_or(60);
                self.fps_den = config["frameRate"]["den"].as_i64().unwrap_or(1);
                self.audio_rate = config["audio"]["deviceFormat"]["rate"].as_i64().unwrap_or(48000);
                self.audio_channels = config["audio"]["deviceFormat"]["channels"].as_i64().unwrap_or(2);
                self.audio_s16 = config["audio"]["sampleFormat"].as_str().unwrap_or("s16") == "s16";
                let sink = &config["frameSink"];
                self.expected_frames = sink["expectedFrames"].as_i64();
                match sink["type"].as_str().unwrap_or("null") {
                    "shm" => {
                        let fd = |k: &str| unsafe { OwnedFd::from_raw_fd(sink[k].as_i64().unwrap_or(-1) as i32) };
                        let p = Producer::open(fd("ringFd"), fd("readyFd"), fd("freeFd")).map_err(|e| format!("SinkNotConnected: {e}"))?;
                        if p.geometry.width != self.width || p.geometry.height != self.height {
                            return Err("InvalidViewport: ring geometry differs from the viewport".into());
                        }
                        self.producer = Some(p);
                    }
                    "file" => {
                        self.file_sink = Some(std::fs::File::create(sink["path"].as_str().unwrap_or("/dev/null")).map_err(|e| format!("SinkNotConnected: {e}"))?);
                    }
                    "null" => {}
                    other => return Err(format!("InvalidParameters: frameSink.type {other}")),
                }
                self.configured = true;
                self.state = "configured";
                self.next_frame = 0;
                let mut effective = config.clone();
                effective["protocolVersion"] = json!("2.1");
                effective["pixelFormat"] = json!("bgra8-premul");
                effective["virtualTimeTicksBase"] = json!(12345.678);
                Ok(json!({"effective": effective, "virtualTimeTicksBase": 12345.678}))
            }
            "DeterministicCapture.advance" => {
                if !self.configured {
                    return Err("NotConfigured: advance before configure".into());
                }
                let idx = params["frameIndex"].as_i64().unwrap_or(-1);
                if idx != self.next_frame {
                    return Err(format!("FrameIndexMismatch: expected frame {}", self.next_frame));
                }
                self.advance_one(params["captureVideo"].as_bool().unwrap_or(true), params["captureAudio"].as_bool().unwrap_or(true), params["hash"].as_bool().unwrap_or(true))
            }
            "DeterministicCapture.advanceRange" => {
                if !self.configured {
                    return Err("NotConfigured: advanceRange before configure".into());
                }
                let from = params["fromFrameIndex"].as_i64().unwrap_or(-1);
                let to = params["toFrameIndex"].as_i64().unwrap_or(-1);
                if from != self.next_frame {
                    return Err(format!("FrameIndexMismatch: expected frame {}", self.next_frame));
                }
                let hash = params["hash"].as_bool().unwrap_or(false);
                let capture_audio = params["captureAudio"].as_bool().unwrap_or(false);
                let mut frames = Vec::new();
                while self.next_frame < to {
                    frames.push(self.advance_one(false, capture_audio, hash)?);
                }
                Ok(json!({"frames": frames, "nextFrameIndex": self.next_frame}))
            }
            "DeterministicCapture.finish" => {
                if let Some(p) = &self.producer {
                    p.close(false);
                }
                self.file_sink = None;
                let complete = self.expected_frames.map(|e| e == self.frames_written).unwrap_or(true);
                self.state = if complete { "finished" } else { "failed" };
                Ok(json!({
                    "status": if complete { "complete" } else { "failed" }, "framesWritten": self.frames_written, "expectedFrames": self.expected_frames,
                    "reusedFrames": 0, "audioSampleFrames": self.audio_total, "pcmHash": if self.pcm_chain.is_empty() { Value::Null } else { json!(self.pcm_chain) },
                    "chainHash": if self.chain.is_empty() { Value::Null } else { json!(self.chain) },
                    "histograms": {"beginFrameMs": {"count": self.next_frame, "p50": 0.5}}, "gpuProcessCrashes": 0,
                    "completed": std::mem::take(&mut self.pending_hashes),
                }))
            }
            "DeterministicCapture.abort" => {
                if let Some(p) = &self.producer {
                    p.close(true);
                }
                self.state = "aborted";
                Ok(json!({}))
            }
            "DeterministicCapture.getState" => Ok(json!({"state": self.state, "nextFrameIndex": self.next_frame})),
            "Emulation.setDeviceMetricsOverride" => {
                let dsf = params["deviceScaleFactor"].as_u64().unwrap_or(1) as u32;
                self.width = params["width"].as_u64().unwrap_or(640) as u32 * dsf;
                self.height = params["height"].as_u64().unwrap_or(360) as u32 * dsf;
                Ok(json!({}))
            }
            "Emulation.setVirtualTimePolicy" => Err("VirtualTimeOwned: the DeterministicCapture session owns renderer virtual time".into()),
            _ => Ok(json!({})),
        }
    }
}

fn name_and_frame(var: &str) -> Option<(String, i64)> {
    let v = std::env::var(var).ok()?;
    let (name, frame) = v.rsplit_once(':')?;
    Some((name.to_string(), frame.parse().ok()?))
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if !args.iter().any(|a| a == "--remote-debugging-pipe") {
        eprintln!("dcap-fake-engine: expects --remote-debugging-pipe");
        std::process::exit(2);
    }
    let mut width = 640u32;
    let mut height = 360u32;
    let mut name = String::new();
    for a in &args {
        if let Some(v) = a.strip_prefix("--window-size=") {
            if let Some((w, h)) = v.split_once(',') {
                width = w.parse().unwrap_or(640);
                height = h.parse().unwrap_or(360);
            }
        }
        if let Some(v) = a.strip_prefix("--user-data-dir=") {
            name = v.rsplit('/').next().unwrap_or("").trim_start_matches("profile-").to_string();
        }
    }
    let input = unsafe { std::fs::File::from_raw_fd(3) };
    let out = unsafe { std::fs::File::from_raw_fd(4) };
    let mut fake = Fake {
        out,
        session_id: None,
        width,
        height,
        configured: false,
        next_frame: 0,
        frames_written: 0,
        chain: String::new(),
        fps_num: 60,
        fps_den: 1,
        expected_frames: None,
        producer: None,
        file_sink: None,
        marker_frame: std::env::var("DCAP_FAKE_MARKER_FRAME").ok().and_then(|s| s.parse().ok()),
        name,
        diverge: name_and_frame("DCAP_FAKE_DIVERGE"),
        crash: name_and_frame("DCAP_FAKE_CRASH"),
        pending_hashes: Vec::new(),
        audio_rate: 48000,
        audio_channels: 2,
        audio_s16: true,
        audio_total: 0,
        pcm_chain: String::new(),
        state: "unconfigured",
    };
    let mut reader = BufReader::new(input);
    let mut buf = Vec::new();
    loop {
        buf.clear();
        match reader.read_until(0, &mut buf) {
            Ok(0) | Err(_) => std::process::exit(0),
            Ok(_) => {}
        }
        if buf.last() == Some(&0) {
            buf.pop();
        }
        let Ok(msg) = serde_json::from_slice::<Value>(&buf) else { continue };
        let id = msg["id"].clone();
        let session = msg.get("sessionId").cloned();
        let mut response = match fake.handle(msg) {
            Ok(result) => json!({"id": id, "result": result}),
            Err(message) => json!({"id": id, "error": {"code": -32000, "message": message}}),
        };
        if let Some(s) = session {
            response["sessionId"] = s;
        }
        fake.send(response);
    }
}
