//! Render pass (DESIGN.md 5.2.3 and 13.3): one browser per chunk replaying
//! the action log, frames delivered through the shared-memory ring to an
//! in-process encoder thread that writes the chunk container.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::encode::{self, AudioConfig, EncodeStats, Encoder, EncoderConfig, JoinInput};
use crate::engine::EngineError;
use crate::plan::{Chunk, Plan};
use crate::session::{CaptureSession, SinkKind};
use crate::shm::{Ring, RingEvent, RingGeometry, AUDIO_FORMAT_NONE};
use crate::spec::Spec;

pub const RETRYABLE: &[&str] = &["GpuProcessLost", "RendererCrashed", "TargetDetached", "EncoderDeviceLost", "LaunchFailed"];

#[derive(Debug, Clone, Default)]
pub struct Attempt {
    pub number: usize,
    pub status: String,
    pub error: Option<Value>,
    pub timings: BTreeMap<String, f64>,
    pub frames_written: i64,
    pub reused_frames: i64,
    pub encoder: Option<Value>,
    pub sink_error: Option<Value>,
    pub gpu_crashes: i64,
    pub renderer: String,
    pub histograms: Value,
}

impl Attempt {
    pub fn to_json(&self) -> Value {
        json!({
            "number": self.number, "status": self.status, "error": self.error, "timings": self.timings,
            "framesWritten": self.frames_written, "reusedFrames": self.reused_frames, "encoder": self.encoder,
            "sinkError": self.sink_error, "gpuCrashes": self.gpu_crashes, "renderer": self.renderer, "histograms": self.histograms,
        })
    }
}

#[derive(Debug, Clone)]
pub struct ChunkResult {
    pub chunk: Chunk,
    pub output: Option<PathBuf>,
    pub hashes: BTreeMap<i64, String>,
    pub overlap_hashes: BTreeMap<i64, String>,
    /// hashes the engine wrote into the ring slots, by frame
    pub slot_hashes: BTreeMap<i64, String>,
    /// pcmFrameHash per frame and the engine's PCM chain over the chunk
    pub pcm_hashes: BTreeMap<i64, String>,
    pub pcm_hash: Option<String>,
    pub audio_sample_frames: i64,
    pub markers: Vec<Value>,
    pub attempts: Vec<Attempt>,
    pub chain_hash: Option<String>,
    pub extradata_sha256: Option<String>,
    pub packets: Option<i64>,
}

impl ChunkResult {
    pub fn ok(&self) -> bool {
        self.output.is_some() && self.attempts.last().map(|a| a.status == "complete").unwrap_or(false)
    }
    fn failed(chunk: Chunk, attempts: Vec<Attempt>) -> ChunkResult {
        ChunkResult { chunk, output: None, hashes: BTreeMap::new(), overlap_hashes: BTreeMap::new(), slot_hashes: BTreeMap::new(), pcm_hashes: BTreeMap::new(), pcm_hash: None, audio_sample_frames: 0, markers: Vec::new(), attempts, chain_hash: None, extradata_sha256: None, packets: None }
    }
}

pub fn container_extension(container: &str) -> &'static str {
    match container {
        "mov" => "mov",
        "matroska" => "mkv",
        _ => "mp4",
    }
}

pub fn render_chunk(spec: &Spec, plan: &Plan, chunk: Chunk, executable: &Path, work_dir: &Path, init_scripts: &[String], is_last: bool) -> ChunkResult {
    let mut attempts: Vec<Attempt> = Vec::new();
    for n in 1..=3 {
        let mut attempt = Attempt { number: n, status: "running".into(), ..Default::default() };
        match render_once(spec, plan, chunk, executable, work_dir, init_scripts, is_last, &mut attempt) {
            Ok(mut result) => {
                attempt.status = "complete".into();
                attempts.push(attempt);
                result.attempts = attempts;
                return result;
            }
            Err(err) => {
                attempt.status = "failed".into();
                attempt.error = Some(err.to_json());
                let same_frame_twice = n >= 2 && attempts.last().and_then(|a| a.error.as_ref()).map(|e| e["frame"] == json!(err.frame)).unwrap_or(false);
                attempts.push(attempt);
                if !RETRYABLE.contains(&err.kind.as_str()) || n == 3 {
                    return ChunkResult::failed(chunk, attempts);
                }
                if same_frame_twice {
                    if let Some(e) = attempts.last_mut().and_then(|a| a.error.as_mut()) {
                        e["note"] = json!("content-deterministic failure: same frame twice");
                    }
                    return ChunkResult::failed(chunk, attempts);
                }
            }
        }
    }
    ChunkResult::failed(chunk, attempts)
}

struct EncoderOutcome {
    frames: i64,
    stats: EncodeStats,
    slot_hashes: BTreeMap<i64, String>,
    busy_ms: f64,
}

fn encoder_thread(ring: &Ring, cfg: &EncoderConfig<'_>, chunk: Chunk, stall: Duration, cancel: &Mutex<bool>) -> Result<EncoderOutcome, EngineError> {
    let mut enc = Encoder::open(cfg).map_err(|e| EngineError::new(&e.kind, e.message))?;
    let mut n = 0i64;
    let mut busy = 0.0;
    let mut slot_hashes = BTreeMap::new();
    loop {
        match ring.next(stall) {
            RingEvent::Frame(slot) => {
                let t = Instant::now();
                let expected = chunk.start + n;
                if slot.frame_index() != expected {
                    return Err(EngineError::new("FrameIndexMismatch", format!("ring delivered frame {} where {expected} was expected", slot.frame_index())).at(expected));
                }
                if let Some(h) = slot.hash() {
                    slot_hashes.insert(slot.frame_index(), encode::hex(&h));
                }
                enc.video(slot.video(), cfg.width, n).map_err(|e| EngineError::new(&e.kind, e.message).at(expected))?;
                if let Some(audio) = cfg.audio {
                    let fps = crate::spec::FrameRate { num: cfg.fps_num as i64, den: cfg.fps_den as i64 };
                    let want = fps.samples_for(expected, audio.rate as i64);
                    if slot.audio_sample_frames() as i64 != want {
                        return Err(EngineError::new("AudioFrameCount", format!("ring slot for frame {expected} holds {} sample frames, expected {want}", slot.audio_sample_frames())).at(expected));
                    }
                    // Chunk-local pts from absolute sample positions: the per-frame
                    // counts of 60000/1001 differ (800 or 801), so S(start+n) - S(start),
                    // not S(n), keeps consecutive packets contiguous.
                    let pts = fps.samples_through(expected, audio.rate as i64) - fps.samples_through(chunk.start, audio.rate as i64);
                    enc.audio(slot.audio(), want as i32, pts).map_err(|e| EngineError::new(&e.kind, e.message).at(expected))?;
                }
                busy += t.elapsed().as_secs_f64() * 1000.0;
                n += 1;
                ring.release();
            }
            RingEvent::Closed { aborted } => {
                if aborted {
                    return Err(EngineError::new("SinkClosed", "engine aborted the frame stream"));
                }
                break;
            }
            RingEvent::Timeout => {
                if *cancel.lock().unwrap() {
                    return Err(EngineError::new("SinkClosed", "render cancelled"));
                }
                return Err(EngineError::new("SinkStalled", "engine published no frame within the stall timeout").at(chunk.start + n));
            }
        }
    }
    let stats = enc.finish().map_err(|e| EngineError::new(&e.kind, e.message))?;
    Ok(EncoderOutcome { frames: n, stats, slot_hashes, busy_ms: busy })
}

#[allow(clippy::too_many_arguments)]
fn render_once(spec: &Spec, plan: &Plan, chunk: Chunk, executable: &Path, work_dir: &Path, init_scripts: &[String], is_last: bool, attempt: &mut Attempt) -> Result<ChunkResult, EngineError> {
    let name = format!("c{:02}a{}", chunk.index, attempt.number);
    let chunks_dir = work_dir.join("chunks");
    std::fs::create_dir_all(&chunks_dir).map_err(|e| EngineError::new("IoError", e.to_string()))?;
    let ext = container_extension(&spec.encoder.container);
    let partial = chunks_dir.join(format!("chunk_{:02}.partial.{ext}", chunk.index));
    let final_path = chunks_dir.join(format!("chunk_{:02}.{ext}", chunk.index));
    let (w, h) = spec.viewport.output_size();
    let audio_cfg = spec.audio.capture.then(|| AudioConfig { rate: spec.audio.rate as i32, channels: spec.audio.channels as i32, sample_format: if spec.audio.sample_format_s16 { 1 } else { 2 } });
    let geometry = if spec.audio.capture {
        let fps = spec.frame_rate;
        // The longest interval plus one frame of slack.
        let per_slot = (fps.den * spec.audio.rate + fps.num - 1) / fps.num + 1;
        RingGeometry {
            width: w,
            height: h,
            slots: spec.max_queued_frames,
            audio_rate: spec.audio.rate as u32,
            audio_channels: spec.audio.channels as u32,
            audio_format: if spec.audio.sample_format_s16 { crate::shm::AUDIO_FORMAT_S16 } else { crate::shm::AUDIO_FORMAT_F32 },
            audio_frames_per_slot: per_slot as u32,
        }
    } else {
        RingGeometry { width: w, height: h, slots: spec.max_queued_frames, audio_rate: 0, audio_channels: 0, audio_format: AUDIO_FORMAT_NONE, audio_frames_per_slot: 0 }
    };
    let ring = Arc::new(Ring::create(geometry).map_err(|e| EngineError::new("LaunchFailed", format!("ring: {e}")))?);
    let mut session = CaptureSession::new(spec, work_dir, &name, SinkKind::Shm(Arc::clone(&ring)), init_scripts.to_vec());
    let t0 = Instant::now();
    let stall = Duration::from_secs_f64(spec.timeouts.sink_stall_ms / 1000.0);
    let cancel = Mutex::new(false);
    let codec = spec.encoder.clone();
    let fps = spec.frame_rate;
    let cfg = EncoderConfig { spec: &codec, width: w, height: h, fps_num: fps.num as i32, fps_den: fps.den as i32, path: &partial, audio: audio_cfg };

    let outcome: Result<(ChunkResult, EncoderOutcome), EngineError> = std::thread::scope(|scope| {
        let ring_for_encoder = Arc::clone(&ring);
        let cancel_ref = &cancel;
        let cfg_ref = &cfg;
        let encoder = scope.spawn(move || encoder_thread(&ring_for_encoder, cfg_ref, chunk, stall, cancel_ref));

        let mut drive = || -> Result<ChunkResult, EngineError> {
            session.start(executable)?;
            attempt.timings.insert("launchS".into(), session.stats.launch_s);
            session.configure(Some(chunk.frames()))?;
            session.navigate()?;
            attempt.timings.insert("loadS".into(), session.stats.load_s);
            attempt.renderer = session.renderer_identity()?;
            if attempt.renderer != plan.renderer {
                return Err(EngineError::new("BackendMismatch", format!("worker renderer {:?} differs from plan {:?}", attempt.renderer, plan.renderer)));
            }
            let t_pre = Instant::now();
            session.advance_range(chunk.start, Some(&plan.action_log), false, false, 600)?;
            attempt.timings.insert("prerollS".into(), t_pre.elapsed().as_secs_f64());
            let t_cap = Instant::now();
            for f in chunk.start..chunk.end {
                session.replay_actions(&plan.action_log, f, "beforeGrant")?;
                if encoder.is_finished() {
                    return Err(EngineError::new("EncoderFailed", "encoder thread stopped before the chunk ended").at(f));
                }
                session.advance(true, spec.audio.capture, true)?;
            }
            attempt.timings.insert("captureS".into(), t_cap.elapsed().as_secs_f64());
            let overlap_end = if spec.verify.mode == "overlap" && !is_last { (chunk.end + spec.verify.overlap_frames).min(plan.stop_frame) } else { chunk.end };
            for f in chunk.end..overlap_end {
                session.replay_actions(&plan.action_log, f, "beforeGrant")?;
                session.advance(false, false, true)?;
            }
            let finish = session.engine.as_mut().unwrap().finish(Duration::from_secs_f64(spec.timeouts.encoder_finish))?;
            attempt.frames_written = finish.frames_written;
            attempt.reused_frames = finish.reused_frames;
            attempt.gpu_crashes = finish.gpu_process_crashes;
            attempt.sink_error = finish.first_sink_error.clone();
            attempt.histograms = finish.histograms.clone();
            if finish.status != "complete" || finish.frames_written != chunk.frames() {
                return Err(EngineError::new("IncompleteOutput", format!("engine wrote {} of {} frames", finish.frames_written, chunk.frames())).with(finish.raw.clone()));
            }
            let eng = session.engine.as_ref().unwrap();
            let hashes: BTreeMap<i64, String> = (chunk.start..chunk.end).filter_map(|i| eng.hash_of(i).map(|h| (i, h.to_string()))).collect();
            let overlap: BTreeMap<i64, String> = (chunk.end..overlap_end).filter_map(|i| eng.hash_of(i).map(|h| (i, h.to_string()))).collect();
            let pcm_hashes: BTreeMap<i64, String> = (chunk.start..chunk.end).filter_map(|i| eng.pcm_hash_of(i).map(|h| (i, h.to_string()))).collect();
            if spec.audio.capture {
                let want = spec.frame_rate.samples_through(chunk.end, spec.audio.rate) - spec.frame_rate.samples_through(chunk.start, spec.audio.rate);
                if finish.audio_sample_frames != want {
                    return Err(EngineError::new("AudioFrameCount", format!("engine wrote {} audio sample frames for the chunk, expected {want}", finish.audio_sample_frames)).with(finish.raw.clone()));
                }
            }
            Ok(ChunkResult {
                chunk,
                output: None,
                hashes,
                overlap_hashes: overlap,
                slot_hashes: BTreeMap::new(),
                pcm_hashes,
                pcm_hash: finish.pcm_hash.clone(),
                audio_sample_frames: finish.audio_sample_frames,
                markers: session.markers.clone(),
                attempts: Vec::new(),
                chain_hash: finish.chain_hash.clone(),
                extradata_sha256: None,
                packets: None,
            })
        };
        let driven = drive();
        if driven.is_err() {
            if let Some(e) = session.engine.as_mut() {
                if !matches!(e.state, crate::engine::State::Finished | crate::engine::State::Unconfigured) {
                    e.abort(); // closes the ring so the encoder thread ends
                }
            }
            *cancel.lock().unwrap() = true;
            ring.cancel();
        }
        let enc_outcome = encoder.join().unwrap_or_else(|_| Err(EngineError::new("EncoderFailed", "encoder thread panicked")));
        let code = session.close();
        if code != 0 {
            attempt.timings.insert("browserExit".into(), code as f64);
        }
        match (driven, enc_outcome) {
            (Ok(r), Ok(e)) => Ok((r, e)),
            // An encoder failure stalls the ring and surfaces in the driver as a
            // timeout; the encoder's error is the cause, so it wins.
            (Err(drive_err), Err(enc_err)) => Err(enc_err.with(serde_json::json!({"driver": drive_err.to_json()}))),
            (Err(e), Ok(_)) => Err(e),
            (Ok(_), Err(e)) => Err(e),
        }
    });

    let (mut result, enc) = match outcome {
        Ok(v) => v,
        Err(e) => {
            let _ = std::fs::remove_file(&partial);
            return Err(e);
        }
    };
    attempt.encoder = Some(json!({
        "frames": enc.frames, "packets": enc.stats.video_packets, "bytes": enc.stats.bytes_written, "extradataSha256": enc.stats.extradata_sha256,
        "encodeMs": enc.stats.encode_ms_total, "convertMs": enc.stats.convert_ms_total, "threadBusyMs": enc.busy_ms,
    }));
    if enc.frames != chunk.frames() || enc.stats.video_packets != chunk.frames() {
        let _ = std::fs::remove_file(&partial);
        return Err(EngineError::new("EncoderFrameCount", format!("encoder saw {} frames and wrote {} packets for {} frames", enc.frames, enc.stats.video_packets, chunk.frames())));
    }
    if spec.audio.capture && enc.stats.audio_sample_frames != result.audio_sample_frames {
        let _ = std::fs::remove_file(&partial);
        return Err(EngineError::new("AudioFrameCount", format!("encoder received {} audio sample frames, engine reported {}", enc.stats.audio_sample_frames, result.audio_sample_frames)));
    }
    for (f, h) in &enc.slot_hashes {
        if let Some(eh) = result.hashes.get(f) {
            if eh != h {
                let _ = std::fs::remove_file(&partial);
                return Err(EngineError::new("HashMismatch", format!("engine hash and ring hash differ for frame {f}")).at(*f));
            }
        }
    }
    std::fs::rename(&partial, &final_path).map_err(|e| EngineError::new("IoError", e.to_string()))?;
    result.output = Some(final_path);
    result.slot_hashes = enc.slot_hashes;
    result.extradata_sha256 = enc.stats.extradata_sha256;
    result.packets = Some(enc.stats.video_packets);
    attempt.timings.insert("totalS".into(), t0.elapsed().as_secs_f64());
    Ok(result)
}

/// Renders all chunks with at most `workers` browsers at a time.
pub fn render_all(spec: &Spec, plan: &Plan, executable: &Path, work_dir: &Path, init_scripts: &[String]) -> Vec<ChunkResult> {
    let last = plan.chunks.len().saturating_sub(1);
    let queue = Mutex::new(plan.chunks.clone().into_iter().collect::<std::collections::VecDeque<_>>());
    let results: Mutex<Vec<ChunkResult>> = Mutex::new(Vec::new());
    std::thread::scope(|scope| {
        for _ in 0..spec.parallel.workers.min(plan.chunks.len().max(1)) {
            scope.spawn(|| loop {
                let next = queue.lock().unwrap().pop_front();
                let Some(c) = next else { break };
                let r = render_chunk(spec, plan, c, executable, work_dir, init_scripts, c.index == last);
                results.lock().unwrap().push(r);
            });
        }
    });
    let mut out = results.into_inner().unwrap();
    out.sort_by_key(|r| r.chunk.index);
    out
}

pub fn run_chain_hash(results: &[ChunkResult]) -> String {
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    for r in results {
        for i in r.chunk.start..r.chunk.end {
            h.update(r.hashes.get(&i).map(String::as_bytes).unwrap_or(b""));
        }
    }
    encode::hex(&h.finalize())
}

/// Joins the chunk files into the final container (stream copy, computed timestamps).
pub fn join_chunks(spec: &Spec, plan: &Plan, results: &[ChunkResult], out_dir: &Path) -> Result<Value, EngineError> {
    let ext = container_extension(&spec.encoder.container);
    let final_path = out_dir.join(format!("capture.{ext}"));
    let inputs: Vec<JoinInput<'_>> = results
        .iter()
        .map(|r| JoinInput {
            path: r.output.as_deref().expect("ok chunk"),
            first_frame: r.chunk.start,
            frames: r.chunk.frames(),
            audio_sample_frames: if spec.audio.capture { spec.frame_rate.samples_through(r.chunk.end, spec.audio.rate) - spec.frame_rate.samples_through(r.chunk.start, spec.audio.rate) } else { 0 },
        })
        .collect();
    let expected_audio = if spec.audio.capture { spec.frame_rate.samples_through(plan.stop_frame, spec.audio.rate) - spec.frame_rate.samples_through(plan.start_frame, spec.audio.rate) } else { 0 };
    let stats = encode::join(&inputs, plan.start_frame, &final_path, &spec.encoder.container, spec.frame_rate.num as i32, spec.frame_rate.den as i32, spec.audio.rate as i32)
        .map_err(|e| EngineError::new(&e.kind, e.message))?;
    if stats.video_packets != plan.total_frames() {
        return Err(EngineError::new("JoinMismatch", format!("joined {} packets for {} frames", stats.video_packets, plan.total_frames())));
    }
    if stats.audio_sample_frames != expected_audio {
        return Err(EngineError::new("JoinMismatch", format!("joined {} audio sample frames, expected {expected_audio}", stats.audio_sample_frames)));
    }
    let probe = encode::probe_file(&final_path).map_err(|e| EngineError::new(&e.kind, e.message))?;
    if spec.audio.capture && probe.audio_sample_frames != expected_audio {
        return Err(EngineError::new("JoinMismatch", format!("final file holds {} audio sample frames, expected {expected_audio}", probe.audio_sample_frames)));
    }
    if probe.video_packets != plan.total_frames() {
        return Err(EngineError::new("JoinMismatch", format!("final file holds {} packets for {} frames", probe.video_packets, plan.total_frames())));
    }
    // One frame must last exactly den/num seconds in the stream's time base.
    let expected_step = spec.frame_rate.den as i128 * probe.time_base.1 as i128;
    let actual_step = probe.first_pts_delta as i128 * spec.frame_rate.num as i128 * probe.time_base.0 as i128;
    if plan.total_frames() > 1 && expected_step != actual_step {
        return Err(EngineError::new("JoinMismatch", format!("frame duration in the final file is {} units of {}/{}, not {}/{} s", probe.first_pts_delta, probe.time_base.0, probe.time_base.1, spec.frame_rate.den, spec.frame_rate.num)));
    }
    if !spec.keep_chunks {
        for r in results {
            if let Some(p) = &r.output {
                let _ = std::fs::remove_file(p);
            }
        }
    }
    Ok(json!({
        "final": final_path, "finalSha256": file_sha256(&final_path), "bytes": stats.bytes_written, "packets": stats.video_packets,
        "audioSampleFrames": stats.audio_sample_frames, "codec": probe.codec, "pixelFormat": probe.pixel_format, "timeBase": probe.time_base, "ptsStep": probe.first_pts_delta,
        "extradataSha256": probe.extradata_sha256,
    }))
}

pub fn file_sha256(path: &Path) -> Option<String> {
    use sha2::{Digest, Sha256};
    use std::io::Read;
    let mut f = std::fs::File::open(path).ok()?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).ok()?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Some(encode::hex(&h.finalize()))
}
