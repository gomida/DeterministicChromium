//! Command line: validate, probe, render, join, codecs, smoke, versions.

use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::encode::{self, AudioConfig, EncoderConfig, JoinInput};
use crate::engine::{Engine, EngineError, REQUIRED_FEATURES};
use crate::plan::{needs_plan_pass, run_plan_pass, Plan};
use crate::render::{join_chunks, render_all, run_chain_hash, ChunkResult};
use crate::report::{build_report, ReportInputs};
use crate::scenario::load_scenario;
use crate::session::{CaptureSession, SinkKind};
use crate::spec::{load_spec, Spec, SpecError};
use crate::verify;

const USAGE: &str = "usage:
  dcap validate SPEC
  dcap probe SPEC --browser PATH
  dcap render SPEC --browser PATH [--run-id ID]
  dcap join --out FILE --container mp4|mov|matroska --fps NUM/DEN --start FRAME CHUNK:FIRST_FRAME:FRAMES...
  dcap codecs [--size WxH]
  dcap smoke --browser PATH [--backend B] [--frames N] [--size WxH] [--codec NAME] [--out DIR] [--url URL]
  dcap versions";

fn flag(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

fn load(path: &str) -> Result<Spec, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
    let raw: Value = serde_json::from_str(&text).map_err(|e| format!("{path}: {e}"))?;
    load_spec(raw).map_err(|e: SpecError| e.0)
}

fn refuse_drvfs(path: &Path) -> Result<(), String> {
    let Ok(mounts) = std::fs::read_to_string("/proc/mounts") else { return Ok(()) };
    let resolved = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    for line in mounts.lines() {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.len() > 2 && (f[2] == "9p" || f[2] == "drvfs") && resolved.starts_with(f[1]) {
            return Err(format!("work directory {} is on {}; use the Linux filesystem", resolved.display(), f[2]));
        }
    }
    Ok(())
}

pub struct ProbeInfo {
    pub capabilities: Value,
    pub renderer: String,
    pub preroll_ratio: f64,
    pub preroll_ms_per_frame: f64,
    pub hash_ms_per_frame: f64,
    pub launch: Value,
}

/// Capabilities, renderer check, encoder availability and the pre-roll cost
/// ratio on the spec's page.
pub fn probe(spec: &Spec, executable: &Path, work_dir: &Path) -> Result<ProbeInfo, EngineError> {
    let (w, h) = spec.viewport.output_size();
    let probe_path = work_dir.join("codec-probe");
    let cfg = EncoderConfig { spec: &spec.encoder, width: w, height: h, fps_num: spec.frame_rate.num as i32, fps_den: spec.frame_rate.den as i32, path: &probe_path, audio: None::<AudioConfig> };
    encode::probe_codec(&cfg).map_err(|e| EngineError::new(&e.kind, e.message))?;
    let mut required: Vec<&str> = REQUIRED_FEATURES.to_vec();
    if spec.audio.capture {
        required.push("audioRender");
    }
    if spec.timeline.stop.kind == "marker" || spec.timeline.start.kind == "marker" {
        required.push("markers");
    }
    let mut s = CaptureSession::new(spec, work_dir, "probe", SinkKind::Null, Vec::new());
    let result = (|| -> Result<ProbeInfo, EngineError> {
        s.start(executable)?;
        let caps = Engine::get_capabilities(&s.bp.as_ref().unwrap().browser)?;
        caps.check(&required)?;
        s.configure(None)?;
        s.navigate()?;
        let renderer = s.renderer_identity()?;
        crate::browser::renderer_is_acceptable(&spec.backend, &renderer).map_err(|why| EngineError::new("BackendMismatch", why))?;
        let t0 = Instant::now();
        s.advance_range(60, None, false, false, 600)?;
        let preroll = t0.elapsed().as_secs_f64() / 60.0;
        let t1 = Instant::now();
        for _ in 0..30 {
            s.advance(false, false, true)?;
        }
        let capture = t1.elapsed().as_secs_f64() / 30.0;
        s.engine.as_mut().unwrap().finish(Duration::from_secs(60))?;
        Ok(ProbeInfo {
            capabilities: caps.raw.clone(),
            renderer,
            preroll_ratio: (preroll / capture.max(1e-6) * 1000.0).round() / 1000.0,
            preroll_ms_per_frame: preroll * 1000.0,
            hash_ms_per_frame: capture * 1000.0,
            launch: s.launch_plan.as_ref().map(|p| p.as_report()).unwrap_or_else(|| json!({})),
        })
    })();
    s.close();
    result
}

pub fn render(spec: &Spec, executable: &Path, out_dir: &Path, run_id: &str) -> Result<i32, String> {
    let started = Instant::now();
    let work_dir = out_dir.join("run").join(run_id).join("work");
    std::fs::create_dir_all(&work_dir).map_err(|e| e.to_string())?;
    refuse_drvfs(&work_dir)?;
    let mut scenario = load_scenario(spec.scenario.as_deref()).map_err(|e| e.to_string())?;
    let options = Value::Object(spec.scenario_options.clone());
    let init_scripts = scenario.init_scripts(&options);

    let mut status = "failed";
    let mut failure: Option<Value> = None;
    let mut plan: Option<Plan> = None;
    let mut results: Vec<ChunkResult> = Vec::new();
    let mut verifies: Vec<verify::VerifyResult> = Vec::new();
    let mut join_info: Option<Value> = None;
    let mut chain: Option<String> = None;
    let mut probe_info: Option<ProbeInfo> = None;

    let outcome = (|| -> Result<(), EngineError> {
        let p = probe(spec, executable, &work_dir)?;
        let preroll_ratio = p.preroll_ratio;
        let renderer = p.renderer.clone();
        probe_info = Some(p);
        let pl = if needs_plan_pass(spec) {
            run_plan_pass(spec, executable, &work_dir, scenario.as_mut(), preroll_ratio, &init_scripts)?
        } else {
            Plan::direct(spec, &renderer, preroll_ratio)
        };
        plan = Some(pl);
        let pl = plan.as_ref().unwrap();
        results = render_all(spec, pl, executable, &work_dir, &init_scripts);
        let c = run_chain_hash(&results);
        chain = Some(c.clone());
        if matches!(spec.verify.mode.as_str(), "boundaries" | "overlap" | "full") {
            verifies.push(verify::verify(pl, &results, &spec.verify.mode));
        }
        if spec.verify.mode == "repeat" && results.iter().all(ChunkResult::ok) {
            for i in 0..spec.verify.repeat {
                let again = render_all(spec, pl, executable, &work_dir.join(format!("repeat{i}")), &init_scripts);
                verifies.push(verify::verify_repeat(&c, &run_chain_hash(&again)));
                for r in again {
                    if let Some(p) = r.output {
                        let _ = std::fs::remove_file(p);
                    }
                }
            }
        }
        if results.iter().all(ChunkResult::ok) && verifies.iter().all(verify::VerifyResult::ok) {
            join_info = Some(join_chunks(spec, pl, &results, out_dir)?);
            status = "complete";
        } else {
            failure = Some(json!({
                "primary": if results.iter().all(ChunkResult::ok) { "verify" } else { "render" },
                "chunks": results.iter().filter(|r| !r.ok()).map(|r| r.attempts.last().and_then(|a| a.error.clone())).collect::<Vec<_>>(),
            }));
        }
        Ok(())
    })();
    if let Err(e) = outcome {
        failure = Some(json!({"primary": e.kind, "message": e.message, "details": e.details, "frame": e.frame}));
    }
    let empty = json!({});
    let report = build_report(&ReportInputs {
        spec,
        status,
        browser_path: executable,
        capabilities: probe_info.as_ref().map(|p| &p.capabilities).unwrap_or(&empty),
        launch: probe_info.as_ref().map(|p| &p.launch).unwrap_or(&empty),
        plan: plan.as_ref(),
        results: &results,
        verify_results: &verifies,
        join_info: join_info.as_ref(),
        failure: failure.as_ref(),
        run_chain_hash: chain.as_deref(),
        work_dir: &work_dir,
        out_dir,
        started,
    })
    .map_err(|e| e.to_string())?;
    println!(
        "{}",
        json!({"status": status, "report": out_dir.join("capture-report.json"), "final": join_info.as_ref().map(|j| j["final"].clone()), "failure": report["failure"]})
    );
    Ok(if status == "complete" { 0 } else { 1 })
}

fn run_id() -> String {
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    format!("{secs}-{:06x}", std::process::id())
}

pub fn main(args: &[String]) -> i32 {
    match args.first().map(String::as_str) {
        Some("validate") => {
            let Some(path) = args.get(1) else { eprintln!("{USAGE}"); return 2 };
            match load(path) {
                Ok(s) => {
                    println!("{}", serde_json::to_string_pretty(&json!({
                        "url": s.url, "output": s.viewport.output_size(), "frameRate": format!("{}/{}", s.frame_rate.num, s.frame_rate.den),
                        "stop": s.timeline.stop.to_json(), "backend": s.backend, "encoder": s.encoder, "needsPlanPass": needs_plan_pass(&s),
                        "specHash": s.spec_hash()})).unwrap());
                    0
                }
                Err(e) => {
                    eprintln!("spec error: {e}");
                    2
                }
            }
        }
        Some("probe") => {
            let (Some(path), Some(browser)) = (args.get(1), flag(args, "--browser")) else { eprintln!("{USAGE}"); return 2 };
            let spec = match load(path) { Ok(s) => s, Err(e) => { eprintln!("spec error: {e}"); return 2 } };
            let work = PathBuf::from(&spec.output_dir).join("run").join("probe").join("work");
            if let Err(e) = std::fs::create_dir_all(&work) { eprintln!("{e}"); return 1 }
            match probe(&spec, Path::new(&browser), &work) {
                Ok(p) => {
                    println!("{}", serde_json::to_string_pretty(&json!({"capabilities": p.capabilities, "renderer": p.renderer, "prerollRatio": p.preroll_ratio,
                        "prerollMsPerFrame": p.preroll_ms_per_frame, "hashMsPerFrame": p.hash_ms_per_frame, "launch": p.launch})).unwrap());
                    0
                }
                Err(e) => { eprintln!("probe failed: {e}"); 1 }
            }
        }
        Some("render") => {
            let (Some(path), Some(browser)) = (args.get(1), flag(args, "--browser")) else { eprintln!("{USAGE}"); return 2 };
            let spec = match load(path) { Ok(s) => s, Err(e) => { eprintln!("spec error: {e}"); return 2 } };
            let id = flag(args, "--run-id").unwrap_or_else(run_id);
            match render(&spec, Path::new(&browser), Path::new(&spec.output_dir), &id) {
                Ok(code) => code,
                Err(e) => { eprintln!("render failed: {e}"); 1 }
            }
        }
        Some("join") => join_cli(&args[1..]),
        Some("codecs") => codecs_cli(&args[1..]),
        Some("smoke") => crate::smoke::run(&args[1..]),
        Some("versions") => {
            println!("dcap {}", crate::VERSION);
            println!("{}", encode::library_versions());
            println!("encoders: {}", encode::available_encoders().join(","));
            0
        }
        _ => { eprintln!("{USAGE}"); 2 }
    }
}

fn join_cli(args: &[String]) -> i32 {
    let out = flag(args, "--out").unwrap_or_default();
    let container = flag(args, "--container").unwrap_or_else(|| "mp4".into());
    let fps = flag(args, "--fps").unwrap_or_else(|| "60/1".into());
    let (num, den) = fps.split_once('/').map(|(a, b)| (a.parse().unwrap_or(60), b.parse().unwrap_or(1))).unwrap_or((fps.parse().unwrap_or(60), 1));
    let start: i64 = flag(args, "--start").and_then(|s| s.parse().ok()).unwrap_or(0);
    let mut inputs = Vec::new();
    let mut i = 0;
    while i < args.len() {
        if args[i].starts_with("--") { i += 2; continue; }
        let parts: Vec<&str> = args[i].rsplitn(3, ':').collect();
        if parts.len() != 3 { eprintln!("chunk spec must be PATH:FIRST_FRAME:FRAMES, got {}", args[i]); return 2 }
        inputs.push((PathBuf::from(parts[2]), parts[1].parse::<i64>().unwrap_or(0), parts[0].parse::<i64>().unwrap_or(0)));
        i += 1;
    }
    if out.is_empty() || inputs.is_empty() { eprintln!("{USAGE}"); return 2 }
    inputs.sort_by_key(|(_, f, _)| *f);
    let refs: Vec<JoinInput<'_>> = inputs.iter().map(|(p, f, n)| JoinInput { path: p, first_frame: *f, frames: *n, audio_sample_frames: 0 }).collect();
    match encode::join(&refs, start, Path::new(&out), &container, num, den, 48000) {
        Ok(s) => { println!("{}", json!({"final": out, "packets": s.video_packets, "bytes": s.bytes_written})); 0 }
        Err(e) => { eprintln!("join failed: {e}"); 1 }
    }
}

fn codecs_cli(args: &[String]) -> i32 {
    let size = flag(args, "--size").unwrap_or_else(|| "1920x1080".into());
    let (w, h) = size.split_once('x').map(|(a, b)| (a.parse().unwrap_or(1920), b.parse().unwrap_or(1080))).unwrap_or((1920, 1080));
    let tmp = std::env::temp_dir().join(format!("dcap-codecs-{}", std::process::id()));
    let mut rows = Vec::new();
    for codec in ["ffv1", "utvideo", "magicyuv", "ffvhuff", "rawvideo", "h264_nvenc", "hevc_nvenc"] {
        let Some((pix, opts, container)) = encode::codec_defaults(codec) else { continue };
        let spec = encode::CodecSpec { codec: codec.into(), pixel_format: pix.into(), options: opts.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect(), threads: 0, container: container.into(), audio_codec: None };
        let cfg = EncoderConfig { spec: &spec, width: w, height: h, fps_num: 60, fps_den: 1, path: &tmp, audio: None::<AudioConfig> };
        let (available, reason) = match encode::probe_codec(&cfg) { Ok(()) => (true, Value::Null), Err(e) => (false, json!(e.to_string())) };
        rows.push(json!({"codec": codec, "pixelFormat": pix, "container": container, "losslessToHash": encode::lossless_to_hash(pix), "available": available, "reason": reason}));
    }
    println!("{}", serde_json::to_string_pretty(&rows).unwrap());
    0
}
