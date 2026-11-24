//! `dcap smoke`: drives the real engine for a few frames through the shm ring
//! into an in-process encoder, twice, and compares hashes. The development
//! check for the browser patch and the encoder path together.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::json;

use crate::encode::{self, AudioConfig, Encoder, EncoderConfig};
use crate::session::{CaptureSession, SinkKind};
use crate::shm::{Ring, RingEvent, RingGeometry, AUDIO_FORMAT_NONE};
use crate::spec::load_spec;

struct Opts {
    browser: PathBuf,
    backend: String,
    frames: i64,
    width: u32,
    height: u32,
    codec: String,
    out: PathBuf,
    url: String,
}

fn parse(args: &[String]) -> Opts {
    let mut o = Opts {
        browser: PathBuf::new(),
        backend: "swiftshader".into(),
        frames: 10,
        width: 640,
        height: 360,
        codec: "ffv1".into(),
        out: std::env::temp_dir().join("dcap-smoke"),
        url: "data:text/html,<style>body{margin:0;background:%23123}div{width:200px;height:200px;background:%23f36;animation:m 2s linear infinite}@keyframes m{to{transform:translateX(400px)}}</style><div></div>".into(),
    };
    let mut i = 0;
    while i < args.len() {
        let next = || args.get(i + 1).cloned().unwrap_or_default();
        match args[i].as_str() {
            "--browser" => o.browser = PathBuf::from(next()),
            "--backend" => o.backend = next(),
            "--frames" => o.frames = next().parse().unwrap_or(10),
            "--size" => {
                let s = next();
                let (w, h) = s.split_once('x').unwrap_or(("640", "360"));
                o.width = w.parse().unwrap_or(640);
                o.height = h.parse().unwrap_or(360);
            }
            "--codec" => o.codec = next(),
            "--out" => o.out = PathBuf::from(next()),
            "--url" => o.url = next(),
            _ => {}
        }
        i += 2;
    }
    if o.browser.as_os_str().is_empty() {
        eprintln!("--browser is required");
        std::process::exit(2);
    }
    o
}

fn one_run(o: &Opts, run: usize) -> Result<(BTreeMap<i64, String>, f64), Box<dyn std::error::Error>> {
    let raw = json!({
        "url": o.url,
        "viewport": {"width": o.width, "height": o.height},
        "frameRate": 60,
        "timeline": {"stop": {"type": "frames", "count": o.frames}},
        "render": {"backend": o.backend},
        "output": {"dir": o.out},
        "encoder": {"codec": o.codec},
    });
    let spec = load_spec(raw)?;
    let work = o.out.join(format!("run{run}"));
    std::fs::create_dir_all(&work)?;
    let (w, h) = spec.viewport.output_size();
    let ring = Arc::new(Ring::create(RingGeometry { width: w, height: h, slots: spec.max_queued_frames, audio_rate: 0, audio_channels: 0, audio_format: AUDIO_FORMAT_NONE, audio_frames_per_slot: 0 })?);
    let mut session = CaptureSession::new(&spec, &work, "smoke", SinkKind::Shm(Arc::clone(&ring)), vec![]);
    session.start(&o.browser)?;
    let caps = crate::engine::Engine::get_capabilities(&session.bp.as_ref().unwrap().browser)?;
    caps.check(crate::engine::REQUIRED_FEATURES)?;
    if run == 0 {
        println!("capabilities: protocol {} patch {} features {:?}", caps.protocol_version, caps.patch_version, caps.features.keys().collect::<Vec<_>>());
    }
    let effective = session.configure(Some(o.frames))?;
    if run == 0 {
        println!("configured: pixelFormat {} ticks base {}", effective["pixelFormat"], effective["virtualTimeTicksBase"]);
    }
    session.navigate()?;
    let renderer = session.renderer_identity()?;
    if run == 0 {
        println!("renderer: {renderer}");
        println!("gpu info: {}", caps.raw["gpu"]);
    }
    // Encoder thread: consumes the ring until the producer closes.
    let out_path = work.join(format!("smoke.{}", if spec.encoder.container == "matroska" { "mkv" } else { spec.encoder.container.as_str() }));
    let codec = spec.encoder.clone();
    let fps = spec.frame_rate;
    let ring_ref: Arc<Ring> = Arc::clone(&ring);
    let encoder_result = std::thread::scope(|scope| {
        let handle = scope.spawn(|| -> Result<(i64, encode::EncodeStats, f64), String> {
            let cfg = EncoderConfig { spec: &codec, width: w, height: h, fps_num: fps.num as i32, fps_den: fps.den as i32, path: &out_path, audio: None::<AudioConfig> };
            let mut enc = Encoder::open(&cfg).map_err(|e| e.to_string())?;
            let mut n = 0i64;
            let mut busy = 0.0;
            loop {
                match ring_ref.next(Duration::from_secs(60)) {
                    RingEvent::Frame(slot) => {
                        let t = Instant::now();
                        enc.video(slot.video(), w, n).map_err(|e| e.to_string())?;
                        busy += t.elapsed().as_secs_f64() * 1000.0;
                        n += 1;
                        ring_ref.release();
                    }
                    RingEvent::Closed { aborted } => {
                        if aborted {
                            return Err("producer aborted".into());
                        }
                        break;
                    }
                    RingEvent::Timeout => return Err("ring timeout".into()),
                }
            }
            let stats = enc.finish().map_err(|e| e.to_string())?;
            Ok((n, stats, busy))
        });
        let t0 = Instant::now();
        let mut first_timings = None;
        for i in 0..o.frames {
            let r = session.advance(true, false, true)?;
            if i == 0 {
                first_timings = Some(r.timings.clone());
            }
        }
        let fin = session.engine.as_mut().unwrap().finish(Duration::from_secs(60))?;
        let wall_ms = t0.elapsed().as_secs_f64() * 1000.0;
        let enc_result = handle.join().expect("encoder thread");
        Ok::<_, Box<dyn std::error::Error>>((fin, wall_ms, first_timings, enc_result))
    });
    let (fin, wall_ms, first_timings, enc_result) = encoder_result?;
    let (n, stats, busy) = enc_result.map_err(|e| format!("encoder: {e}"))?;
    let hashes: BTreeMap<i64, String> = session.engine.as_ref().unwrap().completed.iter().filter_map(|(k, v)| v.frame_hash.clone().map(|h| (*k, h))).collect();
    println!("run {run}: {} frames in {:.0} ms ({:.2} ms/frame); engine status {} written {}", o.frames, wall_ms, wall_ms / o.frames as f64, fin.status, fin.frames_written);
    println!("run {run}: encoder got {n} frames, {} packets, {} bytes, encode {:.1} ms, convert {:.1} ms, thread busy {:.1} ms", stats.video_packets, stats.bytes_written, stats.encode_ms_total, stats.convert_ms_total, busy);
    println!("run {run}: histograms {}", fin.histograms);
    if let Some(t) = first_timings {
        println!("run {run}: first frame timings {t}");
    }
    let probe = encode::probe_file(&out_path)?;
    println!("run {run}: output {} codec {} {} {}x{} packets {} keyframe {} tb {:?}", out_path.display(), probe.codec, probe.pixel_format, probe.width, probe.height, probe.video_packets, probe.first_keyframe, probe.time_base);
    session.close();
    Ok((hashes, wall_ms))
}

pub fn run(args: &[String]) -> i32 {
    let o = parse(args);
    let a = match one_run(&o, 0) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("run 0 failed: {e}");
            return 1;
        }
    };
    let b = match one_run(&o, 1) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("run 1 failed: {e}");
            return 1;
        }
    };
    let same = a.0 == b.0 && a.0.len() as i64 == o.frames;
    println!("hashes: {}", a.0.iter().map(|(k, v)| format!("{k}:{}", &v[..10])).collect::<Vec<_>>().join(" "));
    println!("run-to-run identical: {same}");
    let distinct: std::collections::BTreeSet<&String> = a.0.values().collect();
    println!("distinct frames: {} of {}", distinct.len(), a.0.len());
    if same { 0 } else { 1 }
}

#[allow(dead_code)]
fn _unused(_: &Path) {}
