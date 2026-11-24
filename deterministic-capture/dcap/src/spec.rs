//! Capture spec: schema, defaults, validation (DESIGN.md section 5.1).
//!
//! The required subset for a serial run is url, viewport, frameRate,
//! timeline.stop, render.backend and output.dir. Unknown keys are errors so a
//! typo cannot silently change a run.

use std::collections::BTreeMap;

use serde_json::{json, Map, Value};

use crate::encode::{codec_defaults, CodecSpec, ALLOWED_OPTIONS};

pub const BACKENDS: &[&str] = &["angle-gl-d3d12", "angle-vulkan", "swiftshader"];
pub const NETWORK_MODES: &[&str] = &["offline", "block-third-party", "allow"];
pub const VERIFY_MODES: &[&str] = &["none", "overlap", "boundaries", "full", "repeat"];
pub const STOP_TYPES: &[&str] = &["frame", "frames", "marker", "mediaEnded"];
pub const START_TYPES: &[&str] = &["frame", "load", "marker"];
pub const INPUT_TYPES: &[&str] = &["click", "mouseMove", "mouseDown", "mouseUp", "key", "keyDown", "keyUp", "wheel"];

#[derive(Debug, Clone)]
pub struct SpecError(pub String);

impl std::fmt::Display for SpecError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl std::error::Error for SpecError {}

fn err<T>(msg: impl Into<String>) -> Result<T, SpecError> {
    Err(SpecError(msg.into()))
}

fn only(d: &Map<String, Value>, allowed: &[&str], where_: &str) -> Result<(), SpecError> {
    let extra: Vec<&String> = d.keys().filter(|k| !allowed.contains(&k.as_str())).collect();
    if extra.is_empty() {
        Ok(())
    } else {
        err(format!("{where_}: unknown keys {extra:?}"))
    }
}

fn obj<'a>(v: &'a Value, where_: &str) -> Result<&'a Map<String, Value>, SpecError> {
    v.as_object().ok_or_else(|| SpecError(format!("{where_} must be an object")))
}

fn req<'a>(d: &'a Map<String, Value>, key: &str, where_: &str) -> Result<&'a Value, SpecError> {
    d.get(key).ok_or_else(|| SpecError(format!("{where_}.{key} is required")))
}

fn int(v: &Value, where_: &str) -> Result<i64, SpecError> {
    v.as_i64().ok_or_else(|| SpecError(format!("{where_} must be an integer")))
}

fn gcd(a: i64, b: i64) -> i64 {
    if b == 0 { a } else { gcd(b, a % b) }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameRate {
    pub num: i64,
    pub den: i64,
}

impl FrameRate {
    pub fn fps(&self) -> f64 {
        self.num as f64 / self.den as f64
    }

    /// Absolute offset of frame `index` from the virtual-time base in integer
    /// microseconds, rounded half down, computed from the index (never
    /// accumulated). 60000/1001 yields the 16683/16683/16684 pattern.
    pub fn frame_time_us(&self, index: i64) -> i64 {
        let numerator = index as i128 * self.den as i128 * 1_000_000;
        let num = self.num as i128;
        let q = numerator.div_euclid(num);
        let r = numerator.rem_euclid(num);
        (q + if 2 * r > num { 1 } else { 0 }) as i64
    }

    /// Cumulative audio sample frames belonging to frames [0, index).
    pub fn samples_through(&self, index: i64, rate: i64) -> i64 {
        ((index as i128 * rate as i128 * self.den as i128) / self.num as i128) as i64
    }

    pub fn samples_for(&self, index: i64, rate: i64) -> i64 {
        self.samples_through(index + 1, rate) - self.samples_through(index, rate)
    }

    pub fn frame_ms(&self, index: i64) -> f64 {
        index as f64 * 1000.0 * self.den as f64 / self.num as f64
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Viewport {
    pub width: u32,
    pub height: u32,
    pub device_scale_factor: u32,
}

impl Viewport {
    pub fn output_size(&self) -> (u32, u32) {
        (self.width * self.device_scale_factor, self.height * self.device_scale_factor)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Condition {
    pub kind: String,
    pub index: Option<i64>,
    pub count: Option<i64>,
    pub name: Option<String>,
    pub players: String,
}

impl Condition {
    pub fn to_json(&self) -> Value {
        let mut m = json!({"type": self.kind});
        if let Some(i) = self.index { m["index"] = json!(i); }
        if let Some(c) = self.count { m["count"] = json!(c); }
        if let Some(n) = &self.name { m["name"] = json!(n); }
        if self.kind == "mediaEnded" { m["players"] = json!(self.players); }
        m
    }
}

#[derive(Debug, Clone)]
pub struct Timeline {
    pub start: Condition,
    pub stop: Condition,
    pub max_frames: i64,
}

#[derive(Debug, Clone)]
pub struct Determinism {
    pub seed: i64,
    pub start_time_ms: i64,
    pub timezone: String,
    pub locale: String,
    pub network: String,
    pub media_features: BTreeMap<String, String>,
    pub predictable_gc: bool,
}

#[derive(Debug, Clone, Copy)]
pub struct AudioSpec {
    pub capture: bool,
    pub rate: i64,
    pub channels: i64,
    /// "s16" or "f32"
    pub sample_format_s16: bool,
}

#[derive(Debug, Clone)]
pub struct ParallelSpec {
    pub workers: usize,
    /// "auto", a count, or explicit chunk start frames
    pub chunks: ChunkSpec,
}

#[derive(Debug, Clone)]
pub enum ChunkSpec {
    Auto,
    Count(usize),
    Starts(Vec<i64>),
}

#[derive(Debug, Clone)]
pub struct VerifySpec {
    pub mode: String,
    pub window_before: i64,
    pub window_after: i64,
    pub overlap_frames: i64,
    pub repeat: usize,
}

#[derive(Debug, Clone)]
pub struct InputEvent {
    pub frame: i64,
    pub kind: String,
    pub params: Map<String, Value>,
}

#[derive(Debug, Clone)]
pub struct Timeouts {
    pub launch: f64,
    pub load: f64,
    pub ready_virtual_ms: f64,
    pub advance: f64,
    pub sink_stall_ms: f64,
    pub encoder_finish: f64,
    pub cdp: f64,
    pub chunk_factor: f64,
}

#[derive(Debug, Clone)]
pub struct Spec {
    pub url: String,
    pub viewport: Viewport,
    pub frame_rate: FrameRate,
    pub timeline: Timeline,
    pub backend: String,
    pub output_dir: String,
    pub ready: BTreeMap<String, bool>,
    pub session_user_agent: Option<String>,
    pub session_headers: BTreeMap<String, String>,
    pub determinism: Determinism,
    pub inputs: Vec<InputEvent>,
    pub audio: AudioSpec,
    pub encoder: CodecSpec,
    pub parallel: ParallelSpec,
    pub verify: VerifySpec,
    pub scenario: Option<String>,
    pub scenario_options: Map<String, Value>,
    pub keep_chunks: bool,
    pub trusted: bool,
    pub background_color: String,
    pub timeouts: Timeouts,
    pub media_wait_timeout_ms: i64,
    pub fail_on_unsupported_api: bool,
    pub navigation: String,
    pub max_queued_frames: u32,
    pub raw: Value,
}

impl Spec {
    pub fn spec_hash(&self) -> String {
        crate::encode::sha256_hex(canonical_json(&self.raw).as_bytes())
    }
}

/// Serializes with sorted keys and no whitespace so the hash is stable.
pub fn canonical_json(v: &Value) -> String {
    fn walk(v: &Value, out: &mut String) {
        match v {
            Value::Object(m) => {
                let mut keys: Vec<&String> = m.keys().collect();
                keys.sort();
                out.push('{');
                for (i, k) in keys.iter().enumerate() {
                    if i > 0 { out.push(','); }
                    out.push_str(&serde_json::to_string(k).unwrap());
                    out.push(':');
                    walk(&m[*k], out);
                }
                out.push('}');
            }
            Value::Array(a) => {
                out.push('[');
                for (i, x) in a.iter().enumerate() {
                    if i > 0 { out.push(','); }
                    walk(x, out);
                }
                out.push(']');
            }
            other => out.push_str(&serde_json::to_string(other).unwrap()),
        }
    }
    let mut s = String::new();
    walk(v, &mut s);
    s
}

fn condition(v: &Value, where_: &str, allowed: &[&str]) -> Result<Condition, SpecError> {
    let d = obj(v, where_)?;
    only(d, &["type", "index", "count", "name", "players"], where_)?;
    let kind = req(d, "type", where_)?.as_str().ok_or_else(|| SpecError(format!("{where_}.type must be a string")))?.to_string();
    if !allowed.contains(&kind.as_str()) {
        return err(format!("{where_}.type must be one of {allowed:?}"));
    }
    let mut c = Condition { kind: kind.clone(), index: None, count: None, name: None, players: "all".into() };
    match kind.as_str() {
        "frame" => c.index = Some(int(req(d, "index", where_)?, where_)?),
        "frames" => {
            let n = int(req(d, "count", where_)?, where_)?;
            if n <= 0 {
                return err(format!("{where_}.count must be positive"));
            }
            c.count = Some(n);
        }
        "marker" => c.name = Some(req(d, "name", where_)?.as_str().unwrap_or("").to_string()),
        "mediaEnded" => c.players = d.get("players").and_then(Value::as_str).unwrap_or("all").to_string(),
        _ => {}
    }
    Ok(c)
}

fn frame_rate(v: &Value) -> Result<FrameRate, SpecError> {
    if let Some(n) = v.as_i64() {
        if n <= 0 {
            return err("frameRate must be positive");
        }
        return Ok(FrameRate { num: n, den: 1 });
    }
    let d = obj(v, "frameRate").map_err(|_| SpecError("frameRate must be an integer or {num, den}".into()))?;
    only(d, &["num", "den"], "frameRate")?;
    let num = int(req(d, "num", "frameRate")?, "frameRate.num")?;
    let den = d.get("den").map(|x| int(x, "frameRate.den")).transpose()?.unwrap_or(1);
    if num <= 0 || den <= 0 {
        return err("frameRate must be positive");
    }
    let g = gcd(num, den);
    Ok(FrameRate { num: num / g, den: den / g })
}

fn encoder_spec(v: Option<&Value>) -> Result<CodecSpec, SpecError> {
    let empty = Map::new();
    let d = match v {
        Some(v) => obj(v, "encoder")?,
        None => &empty,
    };
    only(d, &["codec", "pixelFormat", "options", "threads", "container", "audioCodec"], "encoder")?;
    let codec = d.get("codec").and_then(Value::as_str).unwrap_or("ffv1").to_string();
    let (default_pix, default_opts, default_container) =
        codec_defaults(&codec).ok_or_else(|| SpecError(format!("encoder.codec {codec} is not supported")))?;
    let pixel_format = d.get("pixelFormat").and_then(Value::as_str).unwrap_or(default_pix).to_string();
    let container = d.get("container").and_then(Value::as_str).unwrap_or(default_container).to_string();
    if !["mp4", "mov", "matroska"].contains(&container.as_str()) {
        return err("encoder.container must be mp4, mov or matroska");
    }
    let mut options: Vec<(String, String)> = default_opts.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect();
    if let Some(o) = d.get("options") {
        let om = obj(o, "encoder.options")?;
        for (k, v) in om {
            if !ALLOWED_OPTIONS.contains(&k.as_str()) {
                return err(format!("encoder.options.{k} is not an allowed option"));
            }
            let s = match v {
                Value::String(s) => s.clone(),
                Value::Number(n) => n.to_string(),
                Value::Bool(b) => if *b { "1".into() } else { "0".into() },
                _ => return err(format!("encoder.options.{k} must be a scalar")),
            };
            if let Some(slot) = options.iter_mut().find(|(kk, _)| kk == k) {
                slot.1 = s;
            } else {
                options.push((k.clone(), s));
            }
        }
    }
    let threads = d.get("threads").and_then(Value::as_i64).unwrap_or(0) as i32;
    let audio_codec = d.get("audioCodec").and_then(Value::as_str).map(str::to_string).or_else(|| Some("pcm_s16le".to_string()));
    if let Some(ac) = &audio_codec {
        if !["pcm_s16le", "pcm_f32le", "flac"].contains(&ac.as_str()) {
            return err("encoder.audioCodec must be pcm_s16le, pcm_f32le or flac");
        }
    }
    Ok(CodecSpec { codec, pixel_format, options, threads, container, audio_codec })
}

pub fn load_spec(raw: Value) -> Result<Spec, SpecError> {
    let d = obj(&raw, "spec")?;
    only(
        d,
        &["url", "viewport", "frameRate", "timeline", "ready", "session", "determinism", "inputs", "render", "audio", "encoder", "parallel", "verify", "scenario", "output", "trusted", "media", "navigation", "timeouts", "engine"],
        "spec",
    )?;
    let url = req(d, "url", "spec")?.as_str().ok_or_else(|| SpecError("url must be a string".into()))?.to_string();

    let vp = obj(req(d, "viewport", "spec")?, "viewport")?;
    only(vp, &["width", "height", "deviceScaleFactor"], "viewport")?;
    let viewport = Viewport {
        width: int(req(vp, "width", "viewport")?, "viewport.width")? as u32,
        height: int(req(vp, "height", "viewport")?, "viewport.height")? as u32,
        device_scale_factor: vp.get("deviceScaleFactor").map(|x| int(x, "viewport.deviceScaleFactor")).transpose()?.unwrap_or(1) as u32,
    };
    let (ow, oh) = viewport.output_size();
    if ow == 0 || oh == 0 || ow % 2 == 1 || oh % 2 == 1 {
        return err("viewport output dimensions must be positive and even");
    }

    let fr = frame_rate(req(d, "frameRate", "spec")?)?;

    let tl = obj(req(d, "timeline", "spec")?, "timeline")?;
    only(tl, &["start", "stop", "maxFrames"], "timeline")?;
    let default_start = json!({"type": "frame", "index": 0});
    let start = condition(tl.get("start").unwrap_or(&default_start), "timeline.start", START_TYPES)?;
    let stop = condition(req(tl, "stop", "timeline")?, "timeline.stop", STOP_TYPES)?;
    let max_frames = match tl.get("maxFrames") {
        Some(v) => int(v, "timeline.maxFrames")?,
        None => stop.count.unwrap_or(36_000),
    };
    if stop.kind == "frames" && max_frames < stop.count.unwrap_or(0) {
        return err("timeline.maxFrames is below stop.count");
    }

    let mut ready: BTreeMap<String, bool> = [("fonts", true), ("images", true), ("media", true)].iter().map(|(k, v)| (k.to_string(), *v)).collect();
    if let Some(r) = d.get("ready") {
        let rm = obj(r, "ready")?;
        only(rm, &["fonts", "images", "media"], "ready")?;
        for (k, v) in rm {
            ready.insert(k.clone(), v.as_bool().unwrap_or(true));
        }
    }

    let mut session_user_agent = None;
    let mut session_headers = BTreeMap::new();
    if let Some(s) = d.get("session") {
        let sm = obj(s, "session")?;
        only(sm, &["profile", "storageState", "userAgent", "headers"], "session")?;
        session_user_agent = sm.get("userAgent").and_then(Value::as_str).map(str::to_string);
        if let Some(h) = sm.get("headers").and_then(Value::as_object) {
            for (k, v) in h {
                session_headers.insert(k.clone(), v.as_str().unwrap_or("").to_string());
            }
        }
    }

    let empty = Map::new();
    let det = d.get("determinism").map(|v| obj(v, "determinism")).transpose()?.unwrap_or(&empty);
    only(det, &["seed", "startTime", "timezone", "locale", "network", "archive", "mediaFeatures", "predictableGc"], "determinism")?;
    let determinism = Determinism {
        seed: det.get("seed").map(|x| int(x, "determinism.seed")).transpose()?.unwrap_or(20260703),
        start_time_ms: det.get("startTime").map(|x| int(x, "determinism.startTime")).transpose()?.unwrap_or(1_700_000_000_000),
        timezone: det.get("timezone").and_then(Value::as_str).unwrap_or("UTC").to_string(),
        locale: det.get("locale").and_then(Value::as_str).unwrap_or("en-US").to_string(),
        network: det.get("network").and_then(Value::as_str).unwrap_or("offline").to_string(),
        media_features: det
            .get("mediaFeatures")
            .and_then(Value::as_object)
            .map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string())).collect())
            .unwrap_or_default(),
        predictable_gc: det.get("predictableGc").and_then(Value::as_bool).unwrap_or(false),
    };
    if !NETWORK_MODES.contains(&determinism.network.as_str()) {
        return err(format!("determinism.network must be one of {NETWORK_MODES:?}"));
    }

    let mut inputs = Vec::new();
    if let Some(list) = d.get("inputs").and_then(Value::as_array) {
        for (i, ev) in list.iter().enumerate() {
            let where_ = format!("inputs[{i}]");
            let em = obj(ev, &where_)?;
            let kind = req(em, "type", &where_)?.as_str().unwrap_or("").to_string();
            if !INPUT_TYPES.contains(&kind.as_str()) {
                return err(format!("{where_}.type must be one of {INPUT_TYPES:?}"));
            }
            let frame = int(req(em, "frame", &where_)?, &where_)?;
            let params: Map<String, Value> = em.iter().filter(|(k, _)| k.as_str() != "frame" && k.as_str() != "type").map(|(k, v)| (k.clone(), v.clone())).collect();
            inputs.push(InputEvent { frame, kind, params });
        }
    }
    inputs.sort_by_key(|e| e.frame);

    let render = obj(req(d, "render", "spec")?, "render")?;
    only(render, &["backend", "backgroundColor"], "render")?;
    let backend = req(render, "backend", "render")?.as_str().unwrap_or("").to_string();
    if !BACKENDS.contains(&backend.as_str()) {
        return err(format!("render.backend must be one of {BACKENDS:?}"));
    }
    let background_color = render.get("backgroundColor").and_then(Value::as_str).unwrap_or("#ffffff").to_string();
    parse_color(&background_color)?;

    let au = d.get("audio").map(|v| obj(v, "audio")).transpose()?.unwrap_or(&empty);
    only(au, &["capture", "deviceFormat", "sampleFormat"], "audio")?;
    let df = au.get("deviceFormat").and_then(Value::as_object);
    let sample_format = au.get("sampleFormat").and_then(Value::as_str).unwrap_or("s16");
    if !["s16", "f32"].contains(&sample_format) {
        return err("audio.sampleFormat must be s16 or f32");
    }
    let audio = AudioSpec {
        capture: au.get("capture").and_then(Value::as_bool).unwrap_or(false),
        rate: df.and_then(|m| m.get("rate")).and_then(Value::as_i64).unwrap_or(48000),
        channels: df.and_then(|m| m.get("channels")).and_then(Value::as_i64).unwrap_or(2),
        sample_format_s16: sample_format == "s16",
    };

    let mut encoder = encoder_spec(d.get("encoder"))?;
    // PCM in MP4 is non-standard and FLAC in MP4 is experimental in ffmpeg;
    // MOV carries both with the same exact timestamps (DESIGN.md 13.4).
    if audio.capture && encoder.container == "mp4" {
        encoder.container = "mov".to_string();
    }

    let par = d.get("parallel").map(|v| obj(v, "parallel")).transpose()?.unwrap_or(&empty);
    only(par, &["workers", "chunks"], "parallel")?;
    let workers = par.get("workers").map(|x| int(x, "parallel.workers")).transpose()?.unwrap_or(1);
    if workers < 1 {
        return err("parallel.workers must be >= 1");
    }
    let chunks = match par.get("chunks") {
        None => ChunkSpec::Auto,
        Some(Value::String(s)) if s == "auto" => ChunkSpec::Auto,
        Some(Value::Number(n)) => ChunkSpec::Count(n.as_u64().unwrap_or(1).max(1) as usize),
        Some(Value::Array(a)) => ChunkSpec::Starts(a.iter().filter_map(Value::as_i64).collect()),
        Some(_) => return err("parallel.chunks must be \"auto\", a count or a list of frame indices"),
    };
    let parallel = ParallelSpec { workers: workers as usize, chunks };

    let ver = d.get("verify").map(|v| obj(v, "verify")).transpose()?.unwrap_or(&empty);
    only(ver, &["mode", "windowBefore", "windowAfter", "overlapFrames", "repeat"], "verify")?;
    let verify = VerifySpec {
        mode: ver.get("mode").and_then(Value::as_str).unwrap_or("repeat").to_string(),
        window_before: ver.get("windowBefore").and_then(Value::as_i64).unwrap_or(2),
        window_after: ver.get("windowAfter").and_then(Value::as_i64).unwrap_or(60),
        overlap_frames: ver.get("overlapFrames").and_then(Value::as_i64).unwrap_or(30),
        repeat: ver.get("repeat").and_then(Value::as_u64).unwrap_or(1) as usize,
    };
    if !VERIFY_MODES.contains(&verify.mode.as_str()) {
        return err(format!("verify.mode must be one of {VERIFY_MODES:?}"));
    }

    let sc = d.get("scenario").map(|v| obj(v, "scenario")).transpose()?.unwrap_or(&empty);
    only(sc, &["name", "options"], "scenario")?;
    let scenario = sc.get("name").and_then(Value::as_str).map(str::to_string);
    let scenario_options = sc.get("options").and_then(Value::as_object).cloned().unwrap_or_default();

    let out = obj(req(d, "output", "spec")?, "output")?;
    only(out, &["dir", "keepChunks"], "output")?;
    let output_dir = req(out, "dir", "output")?.as_str().ok_or_else(|| SpecError("output.dir must be a string".into()))?.to_string();

    let media = d.get("media").map(|v| obj(v, "media")).transpose()?.unwrap_or(&empty);
    only(media, &["waitTimeoutMs", "failOnUnsupportedApi"], "media")?;
    let navigation = d.get("navigation").and_then(Value::as_str).unwrap_or("fail").to_string();
    if !["fail", "warn"].contains(&navigation.as_str()) {
        return err("navigation must be fail or warn");
    }

    let engine = d.get("engine").map(|v| obj(v, "engine")).transpose()?.unwrap_or(&empty);
    only(engine, &["maxQueuedFrames"], "engine")?;
    let max_queued_frames = engine.get("maxQueuedFrames").and_then(Value::as_u64).unwrap_or(4).clamp(1, 64) as u32;

    let mut timeouts = Timeouts { launch: 30.0, load: 60.0, ready_virtual_ms: 30_000.0, advance: 120.0, sink_stall_ms: 60_000.0, encoder_finish: 120.0, cdp: 30.0, chunk_factor: 5.0 };
    if let Some(t) = d.get("timeouts") {
        let tm = obj(t, "timeouts")?;
        only(tm, &["launch", "load", "readyVirtualMs", "advance", "sinkStallMs", "encoderFinish", "cdp", "chunkFactor"], "timeouts")?;
        let f = |k: &str, dflt: f64| tm.get(k).and_then(Value::as_f64).unwrap_or(dflt);
        timeouts = Timeouts {
            launch: f("launch", timeouts.launch),
            load: f("load", timeouts.load),
            ready_virtual_ms: f("readyVirtualMs", timeouts.ready_virtual_ms),
            advance: f("advance", timeouts.advance),
            sink_stall_ms: f("sinkStallMs", timeouts.sink_stall_ms),
            encoder_finish: f("encoderFinish", timeouts.encoder_finish),
            cdp: f("cdp", timeouts.cdp),
            chunk_factor: f("chunkFactor", timeouts.chunk_factor),
        };
    }

    let trusted = d.get("trusted").and_then(Value::as_bool).unwrap_or(false);
    if determinism.network == "allow" && !trusted && !(url.starts_with("file:") || url.starts_with("http://localhost") || url.starts_with("http://127.")) {
        return err("determinism.network=allow with a remote URL requires trusted=true (DESIGN.md 1.3)");
    }

    Ok(Spec {
        url,
        viewport,
        frame_rate: fr,
        timeline: Timeline { start, stop, max_frames },
        backend,
        output_dir,
        ready,
        session_user_agent,
        session_headers,
        determinism,
        inputs,
        audio,
        encoder,
        parallel,
        verify,
        scenario,
        scenario_options,
        keep_chunks: out.get("keepChunks").and_then(Value::as_bool).unwrap_or(false),
        trusted,
        background_color,
        timeouts,
        media_wait_timeout_ms: media.get("waitTimeoutMs").and_then(Value::as_i64).unwrap_or(30_000),
        fail_on_unsupported_api: media.get("failOnUnsupportedApi").and_then(Value::as_bool).unwrap_or(true),
        navigation,
        max_queued_frames,
        raw,
    })
}

/// `#rrggbb` or `#rrggbbaa` into CDP's color object.
pub fn parse_color(css: &str) -> Result<Value, SpecError> {
    let s = css.trim();
    if !s.starts_with('#') || !(s.len() == 7 || s.len() == 9) {
        return err(format!("unsupported background color {css:?}; use #rrggbb or #rrggbbaa"));
    }
    let h = |i: usize| u8::from_str_radix(&s[i..i + 2], 16).map_err(|_| SpecError(format!("bad color {css:?}")));
    let (r, g, b) = (h(1)?, h(3)?, h(5)?);
    let a = if s.len() == 9 { h(7)? as f64 / 255.0 } else { 1.0 };
    Ok(json!({"r": r, "g": g, "b": b, "a": a}))
}

pub fn minimal_example() -> Value {
    json!({
        "url": "file:///path/to/index.html",
        "viewport": {"width": 1920, "height": 1080},
        "frameRate": 60,
        "timeline": {"stop": {"type": "frames", "count": 600}},
        "render": {"backend": "angle-gl-d3d12"},
        "output": {"dir": "./run"}
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_times_do_not_drift() {
        let fr = FrameRate { num: 60000, den: 1001 };
        let deltas: Vec<i64> = (0..6).map(|i| fr.frame_time_us(i + 1) - fr.frame_time_us(i)).collect();
        assert_eq!(deltas, vec![16683, 16684, 16683, 16683, 16684, 16683]);
        assert_eq!(fr.frame_time_us(60000), 1001_000_000);
    }

    #[test]
    fn samples_are_exact() {
        let fr = FrameRate { num: 60000, den: 1001 };
        assert_eq!(fr.samples_through(60000, 48000), 48_048_000);
        let total: i64 = (0..1000).map(|i| fr.samples_for(i, 48000)).sum();
        assert_eq!(total, fr.samples_through(1000, 48000));
    }

    #[test]
    fn minimal_spec_loads_with_defaults() {
        let s = load_spec(minimal_example()).unwrap();
        assert_eq!(s.encoder.codec, "ffv1");
        assert_eq!(s.encoder.pixel_format, "bgr0");
        assert_eq!(s.encoder.container, "mp4");
        assert_eq!(s.verify.mode, "repeat");
        assert!(load_spec(json!({"url": "x"})).is_err());
    }

    #[test]
    fn unknown_keys_are_errors() {
        let mut v = minimal_example();
        v["encoder"] = json!({"codec": "ffv1", "options": {"crf": 1}});
        assert!(load_spec(v).is_err());
    }
}
