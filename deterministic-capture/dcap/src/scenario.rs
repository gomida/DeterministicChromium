//! Scenario plugins and the restricted page API (DESIGN.md 5.3).
//!
//! A scenario runs only in the plan pass. Everything it does through `Page`
//! is recorded in the action log; workers and verify passes replay the log
//! and never see the scenario. The API has no sleep and no wall clock.
//! Scenarios are compiled in and selected by name.

use serde_json::{json, Value};

use crate::engine::EngineError;

#[derive(Debug, Clone)]
pub struct Action {
    pub frame: i64,
    /// preNavigation | beforeGrant
    pub phase: String,
    /// initScript | evaluate | input | cdp
    pub kind: String,
    pub payload: Value,
    pub result_hash: Option<String>,
}

impl Action {
    pub fn new(frame: i64, phase: &str, kind: &str, payload: Value) -> Action {
        Action { frame, phase: phase.into(), kind: kind.into(), payload, result_hash: None }
    }
    pub fn to_json(&self) -> Value {
        json!({"frame": self.frame, "phase": self.phase, "kind": self.kind, "payload": self.payload, "resultHash": self.result_hash})
    }
    pub fn from_json(v: &Value) -> Action {
        Action {
            frame: v["frame"].as_i64().unwrap_or(0),
            phase: v["phase"].as_str().unwrap_or("").into(),
            kind: v["kind"].as_str().unwrap_or("").into(),
            payload: v["payload"].clone(),
            result_hash: v["resultHash"].as_str().map(str::to_string),
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct ActionLog {
    pub actions: Vec<Action>,
}

impl ActionLog {
    pub fn add(&mut self, action: Action) {
        self.actions.push(action);
    }
    pub fn at(&self, frame: i64, phase: &str) -> Vec<Action> {
        self.actions.iter().filter(|a| a.frame == frame && a.phase == phase).cloned().collect()
    }
    /// Frames with beforeGrant actions, ascending and unique.
    pub fn frames(&self) -> Vec<i64> {
        let mut f: Vec<i64> = self.actions.iter().filter(|a| a.phase == "beforeGrant").map(|a| a.frame).collect();
        f.sort_unstable();
        f.dedup();
        f
    }
    pub fn to_json(&self) -> Value {
        Value::Array(self.actions.iter().map(Action::to_json).collect())
    }
    pub fn from_json(v: &Value) -> ActionLog {
        ActionLog { actions: v.as_array().map(|a| a.iter().map(Action::from_json).collect()).unwrap_or_default() }
    }
}

pub fn result_hash(value: &Value) -> String {
    crate::encode::sha256_hex(crate::spec::canonical_json(value).as_bytes())[..16].to_string()
}

/// What a scenario may do between frames in the plan pass. `evaluate` runs
/// in the main world and is replayed; `predicate` runs in an isolated world
/// with side effects forbidden and is never replayed.
pub trait Page {
    fn frame(&self) -> i64;
    fn evaluate(&mut self, expression: &str) -> Result<Value, EngineError>;
    fn predicate(&mut self, expression: &str) -> Result<bool, EngineError>;
    fn dispatch_input(&mut self, event: Value);
    fn advance_one(&mut self) -> Result<(), EngineError>;
    fn markers(&self) -> Vec<Value>;

    /// Advances frames until the predicate holds or `max_frames` pass.
    fn advance_until(&mut self, predicate: &str, max_frames: i64) -> Result<bool, EngineError> {
        for _ in 0..max_frames {
            if self.predicate(predicate)? {
                return Ok(true);
            }
            self.advance_one()?;
        }
        self.predicate(predicate)
    }
}

pub struct ScenarioState<'a> {
    pub frame: i64,
    pub markers: &'a [Value],
    pub media: &'a [Value],
}

pub trait Scenario {
    fn init_scripts(&self, options: &Value) -> Vec<String> {
        let _ = options;
        Vec::new()
    }
    /// Maximum frame count known before the plan pass, if any.
    fn horizon(&mut self, options: &Value, fps: f64) -> Result<Option<i64>, EngineError> {
        let _ = (options, fps);
        Ok(None)
    }
    fn ready(&mut self, page: &mut dyn Page, options: &Value) -> Result<bool, EngineError> {
        let _ = (page, options);
        Ok(true)
    }
    fn prelude(&mut self, page: &mut dyn Page, options: &Value) -> Result<(), EngineError> {
        let _ = (page, options);
        Ok(())
    }
    fn should_stop(&mut self, state: &ScenarioState<'_>, options: &Value) -> bool {
        let _ = (state, options);
        false
    }
    /// Chunk boundaries (absolute frames) once markers are known.
    fn boundaries(&mut self, markers: &[Value], options: &Value) -> Option<Vec<i64>> {
        let _ = (markers, options);
        None
    }
}

pub struct NullScenario;

impl Scenario for NullScenario {}

pub fn load_scenario(name: Option<&str>) -> Result<Box<dyn Scenario>, EngineError> {
    match name {
        None => Ok(Box::new(NullScenario)),
        Some("presentation") => Ok(Box::new(crate::scenarios::presentation::Presentation::default())),
        Some(other) => Err(EngineError::new("InvalidParameters", format!("unknown scenario {other}"))),
    }
}
