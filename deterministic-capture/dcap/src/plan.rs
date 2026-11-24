//! Plan pass (DESIGN.md 5.2.2): one serial browser that resolves the timeline
//! and records the action log. Produces a `Plan` that fully determines what
//! every worker does. Boundaries are fixed before the pass starts (from the
//! horizon) so verification windows can be hashed while passing them.

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;
use std::time::Duration;

use serde_json::{json, Value};

use crate::engine::EngineError;
use crate::scenario::{Action, ActionLog, Page, Scenario, ScenarioState};
use crate::session::{CaptureSession, SinkKind};
use crate::spec::{ChunkSpec, Spec};

pub const READY_JS: &[(&str, &str)] = &[
    ("fonts", "(!document.fonts || document.fonts.status === 'loaded')"),
    ("images", "Array.from(document.images || []).every(i => i.complete)"),
    ("media", "Array.from(document.querySelectorAll('video,audio')).every(m => !m.currentSrc || m.readyState >= 2)"),
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Chunk {
    pub index: usize,
    /// inclusive, absolute frame index
    pub start: i64,
    /// exclusive
    pub end: i64,
}

impl Chunk {
    pub fn frames(&self) -> i64 {
        self.end - self.start
    }
}

#[derive(Debug, Clone)]
pub struct Plan {
    pub horizon: i64,
    pub start_frame: i64,
    pub stop_frame: i64,
    pub initial_virtual_time_ms: i64,
    pub virtual_time_ticks_base: f64,
    pub chunks: Vec<Chunk>,
    /// frames hashed in the plan pass
    pub window_frames: Vec<i64>,
    /// frame -> canonical hash (window frames; all frames for `full`)
    pub hashes: BTreeMap<i64, String>,
    pub markers: Vec<Value>,
    pub media_events: Vec<Value>,
    pub action_log: ActionLog,
    pub renderer: String,
    pub stats: Value,
}

impl Plan {
    pub fn total_frames(&self) -> i64 {
        self.stop_frame - self.start_frame
    }

    pub fn to_json(&self) -> Value {
        json!({
            "horizon": self.horizon,
            "startFrame": self.start_frame,
            "stopFrame": self.stop_frame,
            "initialVirtualTimeMs": self.initial_virtual_time_ms,
            "virtualTimeTicksBase": self.virtual_time_ticks_base,
            "chunks": self.chunks.iter().map(|c| json!({"index": c.index, "start": c.start, "end": c.end})).collect::<Vec<_>>(),
            "windowFrames": self.window_frames,
            "hashes": self.hashes.iter().map(|(k, v)| (k.to_string(), json!(v))).collect::<serde_json::Map<_, _>>(),
            "markers": self.markers,
            "mediaEvents": self.media_events,
            "actionLog": self.action_log.to_json(),
            "renderer": self.renderer,
            "stats": self.stats,
        })
    }

    /// A plan without a plan pass: a fixed frame range, one chunk per worker.
    pub fn direct(spec: &Spec, renderer: &str, preroll_ratio: f64) -> Plan {
        let start = spec.timeline.start.index.unwrap_or(0);
        let stop = if spec.timeline.stop.kind == "frame" { spec.timeline.stop.index.unwrap_or(start) } else { start + spec.timeline.stop.count.unwrap_or(0) };
        let chunks = chunk_table(spec, start, stop, preroll_ratio, None);
        Plan {
            horizon: stop,
            start_frame: start,
            stop_frame: stop,
            initial_virtual_time_ms: spec.determinism.start_time_ms,
            virtual_time_ticks_base: 0.0,
            chunks,
            window_frames: Vec::new(),
            hashes: BTreeMap::new(),
            markers: Vec::new(),
            media_events: Vec::new(),
            action_log: spec_input_actions(spec),
            renderer: renderer.to_string(),
            stats: json!({}),
        }
    }
}

/// Chunk start frames such that r*start_k + n_k is equal across chunks
/// (r = pre-roll cost ratio).
pub fn balance_chunks(start: i64, stop: i64, count: usize, preroll_ratio: f64) -> Vec<i64> {
    let n = stop - start;
    if count <= 1 || n <= count as i64 {
        return vec![start];
    }
    // r >= 1 means pre-roll costs as much as capture; chunks then degenerate
    // to one, so cap the ratio and fall back to nearly equal chunks.
    let r = preroll_ratio.clamp(0.0, 0.9);
    let (mut lo, mut hi) = (1.0f64, n as f64);
    for _ in 0..60 {
        let w = (lo + hi) / 2.0;
        let mut b = 0.0;
        for _ in 0..count {
            b = w + (1.0 - r) * b;
        }
        if b >= n as f64 { hi = w } else { lo = w }
    }
    let w = hi;
    let mut bounds = vec![0.0f64];
    for _ in 0..count - 1 {
        let last = *bounds.last().unwrap();
        bounds.push(w + (1.0 - r) * last);
    }
    let mut starts: Vec<i64> = bounds.iter().map(|x| start + x.round() as i64).filter(|s| *s < stop).collect();
    starts.sort_unstable();
    starts.dedup();
    starts
}

pub fn chunk_table(spec: &Spec, start: i64, stop: i64, preroll_ratio: f64, scenario_boundaries: Option<&[i64]>) -> Vec<Chunk> {
    let mut starts: Vec<i64> = match (&spec.parallel.chunks, scenario_boundaries) {
        (ChunkSpec::Auto, _) if spec.parallel.workers == 1 => vec![start],
        (ChunkSpec::Starts(list), _) => {
            let mut s = vec![start];
            s.extend(list.iter().copied().filter(|b| start < *b && *b < stop));
            s
        }
        (_, Some(b)) if !b.is_empty() => {
            let mut s = vec![start];
            s.extend(b.iter().copied().filter(|b| start < *b && *b < stop));
            s
        }
        (ChunkSpec::Auto, _) => balance_chunks(start, stop, spec.parallel.workers, preroll_ratio),
        (ChunkSpec::Count(n), _) => balance_chunks(start, stop, *n, preroll_ratio),
    };
    starts.sort_unstable();
    starts.dedup();
    let mut chunks = Vec::new();
    for (i, s) in starts.iter().enumerate() {
        let e = starts.get(i + 1).copied().unwrap_or(stop);
        if e > *s {
            chunks.push(Chunk { index: chunks.len(), start: *s, end: e });
        }
    }
    chunks
}

pub fn window_frames(spec: &Spec, chunks: &[Chunk], start: i64, stop: i64) -> BTreeSet<i64> {
    let mut frames = BTreeSet::new();
    match spec.verify.mode.as_str() {
        "full" => frames.extend(start..stop),
        "boundaries" | "overlap" => {
            for c in chunks.iter().skip(1) {
                frames.extend((c.start - spec.verify.window_before).max(start)..(c.start + spec.verify.window_after).min(stop));
            }
            for c in chunks {
                frames.insert(c.end - 1);
            }
        }
        _ => {}
    }
    frames
}

pub fn needs_plan_pass(spec: &Spec) -> bool {
    let t = &spec.timeline;
    spec.scenario.is_some()
        || t.start.kind != "frame"
        || !matches!(t.stop.kind.as_str(), "frame" | "frames")
        || spec.audio.capture
        || spec.parallel.workers > 1
        || matches!(spec.verify.mode.as_str(), "boundaries" | "full" | "overlap")
        || !spec.inputs.is_empty()
}

pub fn spec_input_actions(spec: &Spec) -> ActionLog {
    let mut log = ActionLog::default();
    for ev in &spec.inputs {
        let mut payload = Value::Object(ev.params.clone());
        payload["type"] = json!(ev.kind);
        log.add(Action::new(ev.frame, "beforeGrant", "input", payload));
    }
    log
}

/// Adapter exposing a `CaptureSession` to a scenario as the restricted `Page`.
struct PlanPage<'s, 'a> {
    session: &'s mut CaptureSession<'a>,
    log: &'s mut ActionLog,
    capture_audio: bool,
}

impl Page for PlanPage<'_, '_> {
    fn frame(&self) -> i64 {
        self.session.frame()
    }
    fn evaluate(&mut self, expression: &str) -> Result<Value, EngineError> {
        let value = self.session.evaluate_main(expression)?;
        let mut action = Action::new(self.frame(), "beforeGrant", "evaluate", json!({"expression": expression}));
        action.result_hash = Some(crate::scenario::result_hash(&value));
        self.log.add(action);
        Ok(value)
    }
    fn predicate(&mut self, expression: &str) -> Result<bool, EngineError> {
        Ok(self.session.evaluate_isolated(expression)?.as_bool().unwrap_or(false))
    }
    fn dispatch_input(&mut self, event: Value) {
        self.log.add(Action::new(self.frame(), "beforeGrant", "input", event.clone()));
        self.session.queue_input(event);
    }
    fn advance_one(&mut self) -> Result<(), EngineError> {
        self.session.advance(false, self.capture_audio, false).map(|_| ())
    }
    fn markers(&self) -> Vec<Value> {
        self.session.markers.clone()
    }
}

pub fn run_plan_pass(spec: &Spec, executable: &Path, work_dir: &Path, scenario: &mut dyn Scenario, preroll_ratio: f64, init_scripts: &[String]) -> Result<Plan, EngineError> {
    let options = Value::Object(spec.scenario_options.clone());
    let horizon = scenario.horizon(&options, spec.frame_rate.fps())?.unwrap_or(spec.timeline.max_frames);
    let mut session = CaptureSession::new(spec, work_dir, "plan", SinkKind::Null, init_scripts.to_vec());
    let mut log = spec_input_actions(spec);
    session.record_init_scripts(&mut log);

    // Timeline resolution needs the boundaries up front; resolve start/stop
    // provisionally from the horizon.
    let provisional_start = if spec.timeline.start.kind == "frame" { spec.timeline.start.index.unwrap_or(0) } else { 0 };
    let provisional_stop = match spec.timeline.stop.kind.as_str() {
        "frame" => spec.timeline.stop.index.unwrap_or(horizon),
        "frames" => provisional_start + spec.timeline.stop.count.unwrap_or(0),
        _ => horizon,
    }
    .min(horizon);
    let mut chunks = chunk_table(spec, provisional_start, provisional_stop, preroll_ratio, None);
    let windows = window_frames(spec, &chunks, provisional_start, provisional_stop);

    let result = (|| -> Result<Plan, EngineError> {
        session.start(executable)?;
        let load_events = session.page.as_ref().unwrap().subscribe("Page.loadEventFired");
        session.configure(None)?;
        session.navigate()?;
        let renderer = session.renderer_identity()?;

        let mut load_frame: Option<i64> = None;
        let mut start_frame: Option<i64> = if spec.timeline.start.kind == "frame" { Some(provisional_start) } else { None };
        let mut stop_frame: Option<i64> = None;
        let mut ready_done = false;
        let mut prelude_done = false;
        let capture_audio = spec.audio.capture;

        while session.frame() < horizon {
            let f = session.frame();
            let spec_actions = spec_input_actions(spec);
            session.replay_actions(&spec_actions, f, "beforeGrant")?;
            if ready_done && !prelude_done {
                let mut page = PlanPage { session: &mut session, log: &mut log, capture_audio };
                scenario.prelude(&mut page, &options)?;
                prelude_done = true;
            }
            let f = session.frame();
            let result = session.advance(false, capture_audio, windows.contains(&f))?;
            if load_events.try_recv().is_some() && load_frame.is_none() {
                load_frame = Some(f);
            }
            if !ready_done {
                ready_done = true;
                for (key, expr) in READY_JS {
                    if spec.ready.get(*key).copied().unwrap_or(true) && !session.evaluate_isolated(expr)?.as_bool().unwrap_or(false) {
                        ready_done = false;
                        break;
                    }
                }
                if ready_done {
                    let mut page = PlanPage { session: &mut session, log: &mut log, capture_audio };
                    ready_done = scenario.ready(&mut page, &options)?;
                }
                if !ready_done && spec.frame_rate.frame_ms(f) > spec.timeouts.ready_virtual_ms {
                    return Err(EngineError::new("ReadyTimeout", "page did not become ready within the virtual-time budget").at(f));
                }
            }
            if start_frame.is_none() && ready_done {
                let st = &spec.timeline.start;
                if st.kind == "load" && load_frame.is_some() {
                    start_frame = Some(f + 1);
                } else if st.kind == "marker" && result.markers.iter().any(|m| m["name"].as_str() == st.name.as_deref()) {
                    start_frame = Some(f + 1);
                }
            }
            if let Some(sf) = start_frame {
                if f + 1 >= sf {
                    let sp = &spec.timeline.stop;
                    let state = ScenarioState { frame: f, markers: &session.markers, media: &result.media };
                    if sp.kind == "frame" && f + 1 >= sp.index.unwrap_or(i64::MAX) {
                        stop_frame = sp.index;
                    } else if sp.kind == "frames" && f + 1 >= sf + sp.count.unwrap_or(0) {
                        stop_frame = Some(sf + sp.count.unwrap_or(0));
                    } else if sp.kind == "marker" && result.markers.iter().any(|m| m["name"].as_str() == sp.name.as_deref()) {
                        stop_frame = Some(f + 1);
                    } else if sp.kind == "mediaEnded" && !result.media.is_empty() && result.media.iter().all(|m| m["state"] == "ended") {
                        stop_frame = Some(f + 1);
                    } else if scenario.should_stop(&state, &options) {
                        stop_frame = Some(f + 1);
                    }
                }
            }
            if stop_frame.is_some() {
                break;
            }
        }
        let (start_frame, stop_frame) = match (start_frame, stop_frame) {
            (Some(s), Some(e)) => (s, e),
            _ => {
                return Err(EngineError::new(
                    "StopNotReached",
                    format!("horizon {horizon} reached before start/stop resolved (start={start_frame:?}, stop={stop_frame:?})"),
                ))
            }
        };
        let finish = session.engine.as_mut().unwrap().finish(Duration::from_secs_f64(spec.timeouts.encoder_finish))?;
        let mut hashes = BTreeMap::new();
        for (idx, fr) in &session.engine.as_ref().unwrap().completed {
            if let Some(h) = &fr.frame_hash {
                hashes.insert(*idx, h.clone());
            }
        }
        if stop_frame != provisional_stop || start_frame != provisional_start || scenario.boundaries(&session.markers, &options).is_some() {
            let b = scenario.boundaries(&session.markers, &options);
            chunks = chunk_table(spec, start_frame, stop_frame, preroll_ratio, b.as_deref());
        }
        let stats = json!({
            "launchS": session.stats.launch_s, "loadS": session.stats.load_s, "frames": session.stats.frames,
            "advanceMsP50": percentile(&session.stats.advance_wall_ms, 50.0), "advanceMsP99": percentile(&session.stats.advance_wall_ms, 99.0),
            "warnings": session.stats.warnings, "finish": finish.raw,
        });
        Ok(Plan {
            horizon,
            start_frame,
            stop_frame,
            initial_virtual_time_ms: spec.determinism.start_time_ms,
            virtual_time_ticks_base: session.virtual_time_ticks_base,
            chunks: chunks.clone(),
            window_frames: windows.iter().copied().collect(),
            hashes,
            markers: session.markers.clone(),
            media_events: session.media_events.clone(),
            action_log: log.clone(),
            renderer,
            stats,
        })
    })();
    if result.is_err() {
        if let Some(e) = session.engine.as_mut() {
            if !matches!(e.state, crate::engine::State::Finished | crate::engine::State::Unconfigured) {
                e.abort();
            }
        }
    }
    session.close();
    let plan = result?;
    std::fs::write(work_dir.join("plan.json"), serde_json::to_string_pretty(&plan.to_json()).unwrap()).map_err(|e| EngineError::new("IoError", e.to_string()))?;
    Ok(plan)
}

pub fn percentile(values: &[f64], p: f64) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    let mut s = values.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    s[((s.len() as f64 * p / 100.0) as usize).min(s.len() - 1)]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn balanced_chunks_cover_the_range() {
        let starts = balance_chunks(0, 1000, 4, 0.3);
        assert_eq!(starts[0], 0);
        assert_eq!(starts.len(), 4);
        assert!(starts.windows(2).all(|w| w[0] < w[1]));
        // Later chunks are shorter because they pre-roll further.
        let lens: Vec<i64> = starts.windows(2).map(|w| w[1] - w[0]).chain(std::iter::once(1000 - starts[3])).collect();
        assert!(lens[0] > lens[3]);
    }
}
