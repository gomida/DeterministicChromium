//! Client for the `DeterministicCapture` CDP domain (DESIGN.md section 4).
//!
//! The state machine, the cached-retry rule and the pipelined `completed`
//! list are handled here so the rest of the controller sees frames, hashes
//! and typed errors only.

use std::collections::BTreeMap;
use std::time::Duration;

use serde_json::{json, Value};

use crate::cdp::{CdpError, Session};

pub const PROTOCOL_MAJOR: u64 = 2;
pub const REQUIRED_FEATURES: &[&str] = &["pipelinedAdvance", "advanceRange", "shmSink", "canonicalBgra"];

const TERMINAL: &[&str] = &[
    "SinkClosed", "SinkStalled", "MediaWaitTimeout", "AudioUnderflow", "AudioFormatUnsupported", "VirtualTimeStall",
    "NavigationDuringCapture", "RendererCrashed", "GpuProcessLost", "TargetDetached", "UnsupportedApiUsed", "NoSurface",
    "IncompleteOutput",
];

/// A typed error: `kind` is the enum name from the contract, `details` the
/// structured payload the engine attached.
#[derive(Debug, Clone)]
pub struct EngineError {
    pub kind: String,
    pub message: String,
    pub details: Value,
    pub frame: Option<i64>,
}

impl EngineError {
    pub fn new(kind: &str, message: impl Into<String>) -> EngineError {
        EngineError { kind: kind.to_string(), message: message.into(), details: json!({}), frame: None }
    }
    pub fn at(mut self, frame: i64) -> EngineError {
        self.frame = Some(frame);
        self
    }
    pub fn with(mut self, details: Value) -> EngineError {
        self.details = details;
        self
    }
    pub fn terminal(&self) -> bool {
        TERMINAL.contains(&self.kind.as_str())
    }
    pub fn from_cdp(e: CdpError, frame: Option<i64>) -> EngineError {
        match e {
            CdpError::Protocol { message, data, .. } => {
                // The engine renders errors as "Kind: message".
                let kind = data
                    .as_ref()
                    .and_then(|d| d.get("kind"))
                    .and_then(Value::as_str)
                    .map(str::to_string)
                    .unwrap_or_else(|| message.split(':').next().unwrap_or("ProtocolError").trim().to_string());
                EngineError { kind, message, details: data.unwrap_or_else(|| json!({})), frame }
            }
            CdpError::Closed(why) => EngineError { kind: "TargetDetached".into(), message: why, details: json!({}), frame },
            CdpError::Timeout(method) => EngineError { kind: "Timeout".into(), message: format!("{method} timed out"), details: json!({}), frame },
        }
    }
    pub fn to_json(&self) -> Value {
        json!({"kind": self.kind, "message": self.message, "details": self.details, "frame": self.frame})
    }
}

impl std::fmt::Display for EngineError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.kind, self.message)?;
        if let Some(fr) = self.frame {
            write!(f, " (frame {fr})")?;
        }
        Ok(())
    }
}

impl std::error::Error for EngineError {}

#[derive(Debug, Clone)]
pub struct Capabilities {
    pub protocol_version: String,
    pub patch_version: String,
    pub features: BTreeMap<String, i64>,
    pub raw: Value,
}

impl Capabilities {
    pub fn parse(raw: Value) -> Capabilities {
        let mut features = BTreeMap::new();
        if let Some(list) = raw["features"].as_array() {
            for f in list {
                if let Some(name) = f["name"].as_str() {
                    features.insert(name.to_string(), f["version"].as_i64().unwrap_or(1));
                }
            }
        }
        Capabilities {
            protocol_version: raw["protocolVersion"].as_str().unwrap_or("").to_string(),
            patch_version: raw["patchVersion"].as_str().unwrap_or("").to_string(),
            features,
            raw,
        }
    }

    pub fn check(&self, required: &[&str]) -> Result<(), EngineError> {
        let major: u64 = self.protocol_version.split('.').next().and_then(|s| s.parse().ok()).unwrap_or(0);
        if major != PROTOCOL_MAJOR {
            return Err(EngineError::new(
                "VersionMismatch",
                format!("browser protocol {}, controller {}.x", self.protocol_version, PROTOCOL_MAJOR),
            ));
        }
        let missing: Vec<&str> = required.iter().copied().filter(|f| !self.features.contains_key(*f)).collect();
        if !missing.is_empty() {
            return Err(EngineError::new("UnknownFeature", format!("browser lacks features {missing:?}")));
        }
        Ok(())
    }
}

#[derive(Debug, Clone)]
pub struct FrameResult {
    pub frame_index: i64,
    pub virtual_time_ms: f64,
    pub has_damage: bool,
    pub reused_readback: bool,
    pub has_surface: bool,
    pub frame_hash: Option<String>,
    pub chain_hash: Option<String>,
    pub audio_sample_frames: i64,
    pub pcm_frame_hash: Option<String>,
    pub markers: Vec<Value>,
    pub media: Vec<Value>,
    pub timings: Value,
    pub warnings: Vec<String>,
}

impl FrameResult {
    pub fn parse(r: &Value) -> FrameResult {
        FrameResult {
            frame_index: r["frameIndex"].as_i64().unwrap_or(-1),
            virtual_time_ms: r["virtualTimeMs"].as_f64().unwrap_or(0.0),
            has_damage: r["hasDamage"].as_bool().unwrap_or(false),
            reused_readback: r["reusedReadback"].as_bool().unwrap_or(false),
            has_surface: r["hasSurface"].as_bool().unwrap_or(true),
            frame_hash: r["frameHash"].as_str().map(str::to_string),
            chain_hash: r["chainHash"].as_str().map(str::to_string),
            audio_sample_frames: r["audioSampleFrames"].as_i64().unwrap_or(0),
            pcm_frame_hash: r["pcmFrameHash"].as_str().map(str::to_string),
            markers: r["markers"].as_array().cloned().unwrap_or_default(),
            media: r["media"].as_array().cloned().unwrap_or_default(),
            timings: r["timings"].clone(),
            warnings: r["warnings"].as_array().map(|a| a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect()).unwrap_or_default(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct FinishResult {
    pub status: String,
    pub frames_written: i64,
    pub expected_frames: Option<i64>,
    pub reused_frames: i64,
    pub audio_sample_frames: i64,
    pub chain_hash: Option<String>,
    pub pcm_hash: Option<String>,
    pub histograms: Value,
    pub gpu_process_crashes: i64,
    pub first_sink_error: Option<Value>,
    pub raw: Value,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    Unconfigured,
    Configured,
    Capturing,
    Finished,
    Failed,
    Aborted,
}

/// One capturing target.
pub struct Engine {
    pub session: Session,
    pub state: State,
    pub next_frame: i64,
    pub effective_config: Value,
    pub last_error: Option<EngineError>,
    /// Frame results including hashes that arrived later through `completed`.
    pub completed: BTreeMap<i64, FrameResult>,
    last_response: Option<FrameResult>,
}

impl Engine {
    pub fn new(session: Session) -> Engine {
        Engine {
            session,
            state: State::Unconfigured,
            next_frame: 0,
            effective_config: json!({}),
            last_error: None,
            completed: BTreeMap::new(),
            last_response: None,
        }
    }

    pub fn get_capabilities(browser: &Session) -> Result<Capabilities, EngineError> {
        browser
            .send("DeterministicCapture.getCapabilities", json!({}))
            .map(Capabilities::parse)
            .map_err(|e| EngineError::from_cdp(e, None))
    }

    pub fn configure(&mut self, config: Value) -> Result<Value, EngineError> {
        if !matches!(self.state, State::Unconfigured | State::Configured) {
            return Err(EngineError::new("AlreadyStarted", format!("configure in state {:?}", self.state)));
        }
        let r = self
            .session
            .send("DeterministicCapture.configure", json!({"config": config}))
            .map_err(|e| EngineError::from_cdp(e, None))?;
        self.effective_config = r.get("effective").cloned().unwrap_or_else(|| json!({}));
        self.state = State::Configured;
        self.next_frame = 0;
        Ok(self.effective_config.clone())
    }

    fn fail(&mut self, err: EngineError) -> EngineError {
        if err.terminal() {
            self.state = State::Failed;
            self.last_error = Some(err.clone());
        }
        err
    }

    fn absorb(&mut self, r: &Value) {
        if let Some(late) = r["completed"].as_array() {
            for l in late {
                let fr = FrameResult::parse(l);
                let idx = fr.frame_index;
                match self.completed.get_mut(&idx) {
                    Some(existing) => {
                        if fr.frame_hash.is_some() {
                            existing.frame_hash = fr.frame_hash;
                            existing.chain_hash = fr.chain_hash;
                        }
                    }
                    None => {
                        self.completed.insert(idx, fr);
                    }
                }
            }
        }
    }

    pub fn advance(&mut self, capture_video: bool, capture_audio: bool, hash: bool, inputs: &[Value], timeout: Duration) -> Result<FrameResult, EngineError> {
        if self.state == State::Failed {
            if let Some(e) = &self.last_error {
                return Err(e.clone());
            }
        }
        if !matches!(self.state, State::Configured | State::Capturing) {
            return Err(EngineError::new("NotConfigured", format!("advance in state {:?}", self.state)));
        }
        let frame = self.next_frame;
        let mut params = json!({"frameIndex": frame, "captureVideo": capture_video, "captureAudio": capture_audio, "hash": hash});
        if !inputs.is_empty() {
            params["inputs"] = Value::Array(inputs.to_vec());
        }
        let raw = match self.session.send_timeout("DeterministicCapture.advance", params, timeout) {
            Ok(r) => r,
            Err(e) => {
                let err = EngineError::from_cdp(e, Some(frame));
                if err.kind == "FrameIndexMismatch" {
                    if let Some(last) = &self.last_response {
                        if last.frame_index == frame {
                            return Ok(last.clone()); // our response was lost; the engine replayed it
                        }
                    }
                }
                return Err(self.fail(err));
            }
        };
        let result = FrameResult::parse(&raw);
        if result.frame_index != frame {
            return Err(self.fail(EngineError::new("FrameIndexMismatch", format!("expected {frame}, got {}", result.frame_index))));
        }
        self.absorb(&raw);
        let entry = self.completed.entry(frame).or_insert_with(|| result.clone());
        if entry.frame_hash.is_none() {
            entry.frame_hash = result.frame_hash.clone();
        }
        entry.markers = result.markers.clone();
        entry.media = result.media.clone();
        self.last_response = Some(result.clone());
        self.next_frame += 1;
        self.state = State::Capturing;
        Ok(result)
    }

    /// Advances consecutive frames inside the browser; every frame still
    /// issues a real BeginFrame.
    pub fn advance_range(&mut self, to_frame_exclusive: i64, capture_audio: bool, hash: bool, timeout: Duration) -> Result<Vec<FrameResult>, EngineError> {
        if to_frame_exclusive <= self.next_frame {
            return Ok(Vec::new());
        }
        if !matches!(self.state, State::Configured | State::Capturing) {
            return Err(EngineError::new("NotConfigured", format!("advanceRange in state {:?}", self.state)));
        }
        let params = json!({"fromFrameIndex": self.next_frame, "toFrameIndex": to_frame_exclusive, "captureAudio": capture_audio, "hash": hash});
        let raw = match self.session.send_timeout("DeterministicCapture.advanceRange", params, timeout) {
            Ok(r) => r,
            Err(e) => {
                let err = EngineError::from_cdp(e, Some(self.next_frame));
                if let Some(last) = err.details.get("lastCompletedFrameIndex").and_then(Value::as_i64) {
                    self.next_frame = last + 1;
                }
                return Err(self.fail(err));
            }
        };
        let mut frames = Vec::new();
        if let Some(list) = raw["frames"].as_array() {
            for f in list {
                let fr = FrameResult::parse(f);
                self.absorb(f);
                self.completed.entry(fr.frame_index).or_insert_with(|| fr.clone());
                frames.push(fr);
            }
        }
        self.next_frame = raw["nextFrameIndex"].as_i64().unwrap_or(to_frame_exclusive);
        self.state = State::Capturing;
        Ok(frames)
    }

    pub fn finish(&mut self, timeout: Duration) -> Result<FinishResult, EngineError> {
        if matches!(self.state, State::Finished | State::Aborted) {
            return Err(EngineError::new("AlreadyFinished", format!("finish in state {:?}", self.state)));
        }
        let raw = self
            .session
            .send_timeout("DeterministicCapture.finish", json!({}), timeout)
            .map_err(|e| self.fail(EngineError::from_cdp(e, None)))?;
        self.absorb(&raw);
        let result = FinishResult {
            status: raw["status"].as_str().unwrap_or("complete").to_string(),
            frames_written: raw["framesWritten"].as_i64().unwrap_or(0),
            expected_frames: raw["expectedFrames"].as_i64(),
            reused_frames: raw["reusedFrames"].as_i64().unwrap_or(0),
            audio_sample_frames: raw["audioSampleFrames"].as_i64().unwrap_or(0),
            chain_hash: raw["chainHash"].as_str().map(str::to_string),
            pcm_hash: raw["pcmHash"].as_str().map(str::to_string),
            histograms: raw["histograms"].clone(),
            gpu_process_crashes: raw["gpuProcessCrashes"].as_i64().unwrap_or(0),
            first_sink_error: raw.get("firstSinkError").cloned(),
            raw: raw.clone(),
        };
        self.state = if result.status == "complete" { State::Finished } else { State::Failed };
        Ok(result)
    }

    pub fn abort(&mut self) {
        let _ = self.session.send_timeout("DeterministicCapture.abort", json!({}), Duration::from_secs(10));
        self.state = State::Aborted;
    }

    pub fn hash_of(&self, frame: i64) -> Option<&str> {
        self.completed.get(&frame).and_then(|f| f.frame_hash.as_deref())
    }
}
