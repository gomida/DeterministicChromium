//! One browser, one capturing target: the frame driver shared by the plan,
//! render and verify passes. Implements the frame sequence of DESIGN.md 5.2.3
//! on top of `engine::Engine`; the pass decides what is captured, this module
//! decides only the order of operations so every browser runs the same steps.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::browser::{self, LaunchPlan};
use crate::cdp::{EventStream, Session};
use crate::engine::{Engine, EngineError, FrameResult};
use crate::launch::{self, BrowserProcess, FD_FREE, FD_READY, FD_RING};
use crate::scenario::{Action, ActionLog};
use crate::shm::Ring;
use crate::spec::{parse_color, Spec};

pub const ISOLATED_WORLD: &str = "dcap-predicates";

/// What the engine writes frames to.
pub enum SinkKind {
    Null,
    File(PathBuf),
    Shm(Arc<Ring>),
}

#[derive(Debug, Default, Clone)]
pub struct RunStats {
    pub launch_s: f64,
    pub load_s: f64,
    pub frames: i64,
    pub advance_wall_ms: Vec<f64>,
    pub gpu_renderer: String,
    pub warnings: Vec<String>,
}

pub struct CaptureSession<'a> {
    pub spec: &'a Spec,
    pub name: String,
    pub work_dir: PathBuf,
    pub sink: SinkKind,
    pub init_scripts: Vec<String>,
    pub bp: Option<BrowserProcess>,
    pub page: Option<Session>,
    pub engine: Option<Engine>,
    pub stats: RunStats,
    pub virtual_time_ticks_base: f64,
    pub pending_inputs: Vec<Value>,
    pub markers: Vec<Value>,
    pub media_events: Vec<Value>,
    pub isolated_context_id: Option<i64>,
    pub launch_plan: Option<LaunchPlan>,
    dialogs: Option<EventStream>,
    fetch_paused: Option<EventStream>,
}

impl<'a> CaptureSession<'a> {
    pub fn new(spec: &'a Spec, work_dir: &Path, name: &str, sink: SinkKind, init_scripts: Vec<String>) -> CaptureSession<'a> {
        CaptureSession {
            spec,
            name: name.to_string(),
            work_dir: work_dir.to_path_buf(),
            sink,
            init_scripts,
            bp: None,
            page: None,
            engine: None,
            stats: RunStats::default(),
            virtual_time_ticks_base: 0.0,
            pending_inputs: Vec::new(),
            markers: Vec::new(),
            media_events: Vec::new(),
            isolated_context_id: None,
            launch_plan: None,
            dialogs: None,
            fetch_paused: None,
        }
    }

    fn timeout(&self, seconds: f64) -> Duration {
        Duration::from_secs_f64(seconds.max(0.001))
    }

    // ---- lifecycle -------------------------------------------------------

    pub fn start(&mut self, executable: &Path) -> Result<(), EngineError> {
        let t0 = Instant::now();
        let plan = browser::launch_plan(self.spec, executable, &self.work_dir, &self.name).map_err(|e| EngineError::new("LaunchFailed", e.to_string()))?;
        let log_path = self.work_dir.join("logs").join(format!("browser-{}.log", self.name));
        let mut extra = Vec::new();
        if let SinkKind::Shm(ring) = &self.sink {
            let (ring_fd, ready_fd, free_fd) = ring.child_fds().map_err(|e| EngineError::new("LaunchFailed", e.to_string()))?;
            extra.push((FD_RING, ring_fd));
            extra.push((FD_READY, ready_fd));
            extra.push((FD_FREE, free_fd));
        }
        let bp = launch::launch(&plan.executable, &plan.args, &plan.env, &log_path, extra, self.timeout(self.spec.timeouts.cdp))
            .map_err(|e| EngineError::new("LaunchFailed", format!("{}: {e}", plan.executable.display())))?;
        let page = bp.attach_page().map_err(|e| EngineError::from_cdp(e, None).with(json!({"log": bp.last_log_lines(20)})))?;
        self.engine = Some(Engine::new(page.clone()));
        self.page = Some(page);
        self.bp = Some(bp);
        self.launch_plan = Some(plan);
        self.stats.launch_s = t0.elapsed().as_secs_f64();
        self.apply_overrides()
    }

    fn page(&self) -> &Session {
        self.page.as_ref().expect("session started")
    }

    fn cdp(&self, method: &str, params: Value) -> Result<Value, EngineError> {
        self.page().send(method, params).map_err(|e| EngineError::from_cdp(e, None))
    }

    fn apply_overrides(&mut self) -> Result<(), EngineError> {
        let s = self.spec;
        self.cdp("Page.enable", json!({}))?;
        self.cdp("Runtime.enable", json!({}))?;
        self.cdp(
            "Emulation.setDeviceMetricsOverride",
            json!({"width": s.viewport.width, "height": s.viewport.height, "deviceScaleFactor": s.viewport.device_scale_factor, "mobile": false}),
        )?;
        self.cdp("Emulation.setTimezoneOverride", json!({"timezoneId": s.determinism.timezone}))?;
        self.cdp("Emulation.setLocaleOverride", json!({"locale": s.determinism.locale}))?;
        if !s.determinism.media_features.is_empty() {
            let features: Vec<Value> = s.determinism.media_features.iter().map(|(k, v)| json!({"name": k, "value": v})).collect();
            self.cdp("Emulation.setEmulatedMedia", json!({"features": features}))?;
        }
        let color = parse_color(&s.background_color).map_err(|e| EngineError::new("InvalidParameters", e.0))?;
        self.cdp("Emulation.setDefaultBackgroundColorOverride", json!({"color": color}))?;
        self.cdp("Network.enable", json!({}))?;
        self.cdp("Network.setBypassServiceWorker", json!({"bypass": true}))?;
        if s.determinism.network == "block-third-party" {
            self.cdp("Fetch.enable", json!({"patterns": [{"urlPattern": "*", "requestStage": "Request"}]}))?;
            self.fetch_paused = Some(self.page().subscribe("Fetch.requestPaused"));
        }
        if let Some(ua) = &s.session_user_agent {
            self.cdp("Network.setUserAgentOverride", json!({"userAgent": ua}))?;
        }
        if !s.session_headers.is_empty() {
            self.cdp("Network.setExtraHTTPHeaders", json!({"headers": s.session_headers}))?;
        }
        for script in &self.init_scripts.clone() {
            self.cdp("Page.addScriptToEvaluateOnNewDocument", json!({"source": script}))?;
        }
        self.dialogs = Some(self.page().subscribe("Page.javascriptDialogOpening"));
        Ok(())
    }

    /// Handles events that arrived between frames: dialogs are accepted and
    /// paused third-party requests are failed.
    fn pump_events(&mut self) {
        if let Some(d) = &self.dialogs {
            while d.try_recv().is_some() {
                let _ = self.cdp("Page.handleJavaScriptDialog", json!({"accept": true}));
            }
        }
        if let Some(f) = &self.fetch_paused {
            let mut paused = Vec::new();
            while let Some(p) = f.try_recv() {
                paused.push(p);
            }
            for p in paused {
                let url = p["request"]["url"].as_str().unwrap_or("").to_string();
                let origin_ok = same_origin(&self.spec.url, &url) || url.starts_with("data:") || url.starts_with("blob:");
                if origin_ok {
                    let _ = self.cdp("Fetch.continueRequest", json!({"requestId": p["requestId"]}));
                } else {
                    self.stats.warnings.push(format!("blocked third-party request: {}", &url[..url.len().min(120)]));
                    let _ = self.cdp("Fetch.failRequest", json!({"requestId": p["requestId"], "errorReason": "BlockedByClient"}));
                }
            }
        }
    }

    pub fn configure(&mut self, expected_frames: Option<i64>) -> Result<Value, EngineError> {
        let s = self.spec;
        let mut frame_sink = match &self.sink {
            SinkKind::Null => json!({"type": "null"}),
            SinkKind::File(p) => json!({"type": "file", "path": p}),
            SinkKind::Shm(_) => json!({"type": "shm", "ringFd": FD_RING, "readyFd": FD_READY, "freeFd": FD_FREE}),
        };
        if let Some(n) = expected_frames {
            frame_sink["expectedFrames"] = json!(n);
        }
        frame_sink["maxQueuedFrames"] = json!(s.max_queued_frames);
        frame_sink["stallTimeoutMs"] = json!(s.timeouts.sink_stall_ms as i64);
        let config = json!({
            "requiredFeatures": crate::engine::REQUIRED_FEATURES,
            "frameSink": frame_sink,
            "audio": {"sink": {"type": "null"}, "deviceFormat": {"rate": s.audio.rate, "channels": s.audio.channels}, "stems": false},
            "virtualTime": {"initialVirtualTime": s.determinism.start_time_ms as f64, "maxTaskStarvationCount": 0},
            "frameRate": {"num": s.frame_rate.num, "den": s.frame_rate.den},
            "markers": {"prefix": "dcap:", "sources": ["performanceMark", "binding"]},
            "media": {"waitTimeoutMs": s.media_wait_timeout_ms, "failOnUnsupportedApi": s.fail_on_unsupported_api},
            "navigation": s.navigation,
            "backgroundColor": s.background_color,
        });
        let effective = self.engine.as_mut().expect("engine").configure(config)?;
        self.virtual_time_ticks_base = effective["virtualTimeTicksBase"].as_f64().unwrap_or(0.0);
        Ok(effective)
    }

    /// Navigates and waits for the main-frame commit with no virtual-time
    /// grant and no evaluate.
    pub fn navigate(&mut self) -> Result<(), EngineError> {
        let t0 = Instant::now();
        let committed = self.page().subscribe("Page.frameNavigated");
        self.cdp("Page.navigate", json!({"url": self.spec.url}))?;
        committed
            .wait_for(self.timeout(self.spec.timeouts.load), |p| p["frame"].get("parentId").is_none())
            .ok_or_else(|| EngineError::new("LoadTimeout", "main frame did not commit"))?;
        self.stats.load_s = t0.elapsed().as_secs_f64();
        let tree = self.cdp("Page.getFrameTree", json!({}))?;
        let frame_id = tree["frameTree"]["frame"]["id"].clone();
        let ctx = self.cdp("Page.createIsolatedWorld", json!({"frameId": frame_id, "worldName": ISOLATED_WORLD, "grantUniveralAccess": false}))?;
        self.isolated_context_id = ctx["executionContextId"].as_i64();
        Ok(())
    }

    pub fn close(&mut self) -> i32 {
        self.dialogs = None;
        self.fetch_paused = None;
        match self.bp.as_mut() {
            Some(bp) => {
                let code = bp.close(Duration::from_secs(10));
                self.bp = None;
                code
            }
            None => 0,
        }
    }

    // ---- frames ----------------------------------------------------------

    pub fn frame(&self) -> i64 {
        self.engine.as_ref().map(|e| e.next_frame).unwrap_or(0)
    }

    pub fn queue_input(&mut self, event: Value) {
        self.pending_inputs.push(event);
    }

    pub fn evaluate_main(&self, expression: &str) -> Result<Value, EngineError> {
        self.page().evaluate(expression).map_err(|e| EngineError::new("EvaluateFailed", e))
    }

    pub fn evaluate_isolated(&self, expression: &str) -> Result<Value, EngineError> {
        self.evaluate_in_isolated_world(expression, true)
    }

    /// Isolated-world evaluation that may create objects (WebGL probe). Still
    /// invisible to the page's main world and never replayed.
    fn evaluate_in_isolated_world(&self, expression: &str, throw_on_side_effect: bool) -> Result<Value, EngineError> {
        let ctx = self.isolated_context_id.ok_or_else(|| EngineError::new("NotConfigured", "no isolated world yet"))?;
        let r = self.cdp(
            "Runtime.evaluate",
            json!({"expression": expression, "contextId": ctx, "returnByValue": true, "throwOnSideEffect": throw_on_side_effect, "awaitPromise": false}),
        )?;
        if let Some(d) = r.get("exceptionDetails") {
            return Err(EngineError::new("PredicateFailed", d["text"].as_str().unwrap_or("predicate threw").to_string()));
        }
        Ok(r.pointer("/result/value").cloned().unwrap_or(Value::Null))
    }

    pub fn replay_actions(&mut self, log: &ActionLog, frame: i64, phase: &str) -> Result<(), EngineError> {
        for action in log.at(frame, phase) {
            match action.kind.as_str() {
                "evaluate" => {
                    let expr = action.payload["expression"].as_str().unwrap_or("").to_string();
                    let value = self.evaluate_main(&expr)?;
                    if let Some(expected) = &action.result_hash {
                        if &crate::scenario::result_hash(&value) != expected {
                            return Err(EngineError::new("ReplayDivergence", format!("evaluate at frame {frame} returned a different result"))
                                .with(json!({"expression": expr}))
                                .at(frame));
                        }
                    }
                }
                "input" => self.queue_input(action.payload.clone()),
                "cdp" => {
                    let method = action.payload["method"].as_str().unwrap_or("").to_string();
                    self.cdp(&method, action.payload.get("params").cloned().unwrap_or_else(|| json!({})))?;
                }
                _ => {}
            }
        }
        Ok(())
    }

    pub fn advance(&mut self, capture_video: bool, capture_audio: bool, hash: bool) -> Result<FrameResult, EngineError> {
        self.pump_events();
        let inputs = std::mem::take(&mut self.pending_inputs);
        let t0 = Instant::now();
        let timeout = self.timeout(self.spec.timeouts.advance);
        let result = self.engine.as_mut().expect("engine").advance(capture_video, capture_audio, hash, &inputs, timeout)?;
        self.stats.advance_wall_ms.push(t0.elapsed().as_secs_f64() * 1000.0);
        self.stats.frames += 1;
        self.markers.extend(result.markers.iter().cloned());
        for m in &result.media {
            let mut m = m.clone();
            m["frame"] = json!(result.frame_index);
            self.media_events.push(m);
        }
        Ok(result)
    }

    /// Pre-roll with `advanceRange`, splitting at action-log frames and every
    /// `max_chunk` frames so inputs are injected through `advance`.
    pub fn advance_range(&mut self, to_frame_exclusive: i64, log: Option<&ActionLog>, capture_audio: bool, hash: bool, max_chunk: i64) -> Result<(), EngineError> {
        let stops: Vec<i64> = log.map(|l| l.frames()).unwrap_or_default();
        while self.frame() < to_frame_exclusive {
            if let Some(l) = log {
                let f = self.frame();
                self.replay_actions(l, f, "beforeGrant")?;
            }
            if !self.pending_inputs.is_empty() {
                self.advance(false, capture_audio, hash)?;
                continue;
            }
            let here = self.frame();
            let next_stop = stops.iter().copied().filter(|f| *f > here).min().unwrap_or(i64::MAX).min(to_frame_exclusive).min(here + max_chunk);
            let factor = ((next_stop - here) / 60).max(1) as f64;
            let timeout = self.timeout(self.spec.timeouts.advance * factor);
            let frames = self.engine.as_mut().expect("engine").advance_range(next_stop, capture_audio, hash, timeout)?;
            for f in &frames {
                self.markers.extend(f.markers.iter().cloned());
                for m in &f.media {
                    let mut m = m.clone();
                    m["frame"] = json!(f.frame_index);
                    self.media_events.push(m);
                }
            }
            self.stats.frames += frames.len() as i64;
        }
        Ok(())
    }

    /// The backend identity every browser of a run must share: the GPU
    /// process's GL renderer string from `getCapabilities`, with the WebGL
    /// probe as the fallback when the GPU info is not collected yet.
    pub fn renderer_identity(&mut self) -> Result<String, EngineError> {
        let caps = Engine::get_capabilities(&self.bp.as_ref().expect("session started").browser)?;
        let gl = caps.raw["gpu"]["glRenderer"].as_str().unwrap_or("").to_string();
        let renderer = if gl.is_empty() { self.probe_renderer()? } else { gl };
        self.stats.gpu_renderer = renderer.clone();
        Ok(renderer)
    }

    /// Reads the GL renderer string through WebGL on the live page.
    pub fn probe_renderer(&mut self) -> Result<String, EngineError> {
        let expr = "(()=>{const c=document.createElement('canvas');const g=c.getContext('webgl');if(!g)return 'no webgl';\
                    const d=g.getExtension('WEBGL_debug_renderer_info');return d?g.getParameter(d.UNMASKED_RENDERER_WEBGL):g.getParameter(g.RENDERER)})()";
        let renderer = self.evaluate_in_isolated_world(expr, false)?.as_str().unwrap_or("").to_string();
        self.stats.gpu_renderer = renderer.clone();
        Ok(renderer)
    }

    pub fn record_init_scripts(&self, log: &mut ActionLog) {
        for script in &self.init_scripts {
            log.add(Action::new(0, "preNavigation", "initScript", json!({"source": script})));
        }
    }
}

fn same_origin(spec_url: &str, url: &str) -> bool {
    if spec_url.starts_with("file:") {
        return url.starts_with("file:");
    }
    let origin = |u: &str| -> Option<String> {
        let (scheme, rest) = u.split_once("://")?;
        let host = rest.split('/').next()?;
        Some(format!("{scheme}://{host}"))
    };
    match (origin(spec_url), origin(url)) {
        (Some(a), Some(b)) => a == b,
        _ => false,
    }
}
