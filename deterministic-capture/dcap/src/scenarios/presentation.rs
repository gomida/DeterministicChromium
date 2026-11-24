//! The V0.22 presentation scenario (DESIGN.md 5.3): a page that plays one
//! HTMLAudioElement per slide and advances on `ended`, started by a click,
//! with `audio_manifest.json` beside the page giving per-page durations. The
//! scenario only records what it does; workers replay the log.
//!
//! Options: {"manifestPath": "/path/to/audio_manifest.json", "subchunks": 2}
//! The manifest is read from the local filesystem (the page is a file: URL).

use serde_json::Value;

use crate::engine::EngineError;
use crate::scenario::{Page, Scenario, ScenarioState};

const INIT_SCRIPT: &str = r#"
(() => {
  const nativePlay = HTMLMediaElement.prototype.play;
  let index = 0;
  HTMLMediaElement.prototype.play = function (...args) {
    if (this instanceof HTMLAudioElement && !this.__dcapIndex) {
      this.__dcapIndex = ++index;
      performance.mark('dcap:presentation:playing:' + this.__dcapIndex);
      this.addEventListener('ended', () => performance.mark('dcap:presentation:ended:' + this.__dcapIndex), { once: true });
      globalThis.__dcapAudio = this;
    }
    return Reflect.apply(nativePlay, this, args);
  };
})();
"#;

#[derive(Default)]
pub struct Presentation {
    durations_ms: Option<Vec<i64>>,
}

impl Presentation {
    fn manifest(&mut self, options: &Value) -> Result<&[i64], EngineError> {
        if self.durations_ms.is_none() {
            let path = options["manifestPath"].as_str().ok_or_else(|| EngineError::new("InvalidParameters", "scenario.options.manifestPath is required"))?;
            let text = std::fs::read_to_string(path).map_err(|e| EngineError::new("InvalidParameters", format!("manifest {path}: {e}")))?;
            let v: Value = serde_json::from_str(&text).map_err(|e| EngineError::new("InvalidParameters", format!("manifest {path}: {e}")))?;
            let durations: Vec<i64> = v["pages"].as_array().map(|a| a.iter().map(|p| p["duration_ms"].as_i64().unwrap_or(0)).collect()).unwrap_or_default();
            if durations.is_empty() || durations.iter().any(|d| *d <= 0) {
                return Err(EngineError::new("InvalidParameters", format!("audio manifest at {path} has no positive durations")));
            }
            self.durations_ms = Some(durations);
        }
        Ok(self.durations_ms.as_deref().unwrap())
    }
}

impl Scenario for Presentation {
    fn init_scripts(&self, _options: &Value) -> Vec<String> {
        vec![INIT_SCRIPT.to_string()]
    }

    fn horizon(&mut self, options: &Value, fps: f64) -> Result<Option<i64>, EngineError> {
        let durations = self.manifest(options)?;
        let total_ms: i64 = durations.iter().sum();
        let pages = durations.len() as i64;
        Ok(Some((total_ms as f64 / 1000.0 * fps).ceil() as i64 + pages * fps as i64 * 2))
    }

    fn prelude(&mut self, page: &mut dyn Page, _options: &Value) -> Result<(), EngineError> {
        page.dispatch_input(serde_json::json!({"type": "mouseDown", "x": 1, "y": 1, "button": "left", "clickCount": 1}));
        page.dispatch_input(serde_json::json!({"type": "mouseUp", "x": 1, "y": 1, "button": "left", "clickCount": 1}));
        page.advance_until("Boolean(globalThis.__dcapAudio && !globalThis.__dcapAudio.paused)", 600)?;
        Ok(())
    }

    fn should_stop(&mut self, state: &ScenarioState<'_>, _options: &Value) -> bool {
        let n = self.durations_ms.as_ref().map(Vec::len).unwrap_or(0);
        let target = format!("dcap:presentation:ended:{n}");
        state.markers.iter().any(|m| m["name"].as_str() == Some(target.as_str()))
    }

    fn boundaries(&mut self, markers: &[Value], options: &Value) -> Option<Vec<i64>> {
        let mut ended: Vec<i64> = markers
            .iter()
            .filter(|m| m["name"].as_str().map(|n| n.starts_with("dcap:presentation:ended:")).unwrap_or(false))
            .filter_map(|m| m["frameIndex"].as_i64())
            .collect();
        if ended.is_empty() {
            return None;
        }
        ended.sort_unstable();
        let sub = options["subchunks"].as_i64().unwrap_or(2).max(1);
        let mut bounds = Vec::new();
        let mut prev = 0;
        for e in ended {
            let seg = e - prev;
            for k in 1..sub {
                bounds.push(prev + seg * k / sub);
            }
            bounds.push(e);
            prev = e;
        }
        Some(bounds)
    }
}
