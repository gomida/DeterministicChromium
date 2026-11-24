//! Verification (DESIGN.md 5.2.4): hash comparisons between the plan pass,
//! the workers and repeat runs.

use std::collections::BTreeMap;

use serde_json::{json, Value};

use crate::plan::Plan;
use crate::render::ChunkResult;

#[derive(Debug, Clone)]
pub struct Mismatch {
    pub frame: i64,
    /// boundaries | overlap | full | markers | repeat
    pub kind: String,
    pub expected: Option<String>,
    pub actual: Option<String>,
    pub chunk: Option<usize>,
    pub detail: Value,
}

impl Mismatch {
    pub fn to_json(&self) -> Value {
        json!({"frame": self.frame, "kind": self.kind, "expected": self.expected, "actual": self.actual, "chunk": self.chunk, "detail": self.detail})
    }
}

#[derive(Debug, Clone)]
pub struct VerifyResult {
    pub mode: String,
    pub compared: usize,
    pub mismatches: Vec<Mismatch>,
}

impl VerifyResult {
    pub fn ok(&self) -> bool {
        self.mismatches.is_empty()
    }
    pub fn to_json(&self) -> Value {
        json!({"mode": self.mode, "compared": self.compared, "ok": self.ok(), "mismatches": self.mismatches.iter().take(50).map(Mismatch::to_json).collect::<Vec<_>>()})
    }
}

fn compare(frames: impl Iterator<Item = i64>, expected: &BTreeMap<i64, String>, actual: &BTreeMap<i64, String>, kind: &str, chunk: Option<usize>, out: &mut Vec<Mismatch>) -> usize {
    let mut n = 0;
    for f in frames {
        if let (Some(e), Some(a)) = (expected.get(&f), actual.get(&f)) {
            n += 1;
            if e != a {
                out.push(Mismatch { frame: f, kind: kind.into(), expected: Some(e.clone()), actual: Some(a.clone()), chunk, detail: json!({}) });
            }
        }
    }
    n
}

pub fn verify(plan: &Plan, results: &[ChunkResult], mode: &str) -> VerifyResult {
    let mut compared = 0;
    let mut mismatches = Vec::new();
    let by_start: BTreeMap<i64, &ChunkResult> = results.iter().map(|r| (r.chunk.start, r)).collect();
    for r in results.iter().filter(|r| r.ok()) {
        if mode == "boundaries" || mode == "full" {
            compared += compare(plan.hashes.keys().copied().filter(|f| r.chunk.start <= *f && *f < r.chunk.end), &plan.hashes, &r.hashes, mode, Some(r.chunk.index), &mut mismatches);
            // The plan pass hashes PCM for every frame, so audio verification is complete in both modes.
            compared += compare(plan.pcm_hashes.keys().copied().filter(|f| r.chunk.start <= *f && *f < r.chunk.end), &plan.pcm_hashes, &r.pcm_hashes, "pcm", Some(r.chunk.index), &mut mismatches);
        }
        if mode == "overlap" && !r.overlap_hashes.is_empty() {
            if let Some(next) = by_start.get(&r.chunk.end).filter(|n| n.ok()) {
                compared += compare(r.overlap_hashes.keys().copied(), &next.hashes, &r.overlap_hashes, "overlap", Some(r.chunk.index), &mut mismatches);
            }
        }
        let marker_set = |ms: &[Value]| -> std::collections::BTreeSet<(i64, String)> {
            ms.iter()
                .filter_map(|m| Some((m["frameIndex"].as_i64()?, m["name"].as_str()?.to_string())))
                .filter(|(f, _)| r.chunk.start <= *f && *f < r.chunk.end)
                .collect()
        };
        let plan_markers = marker_set(&plan.markers);
        let worker_markers = marker_set(&r.markers);
        for (f, name) in plan_markers.symmetric_difference(&worker_markers) {
            mismatches.push(Mismatch {
                frame: *f,
                kind: "markers".into(),
                expected: plan_markers.contains(&(*f, name.clone())).then(|| name.clone()),
                actual: worker_markers.contains(&(*f, name.clone())).then(|| name.clone()),
                chunk: Some(r.chunk.index),
                detail: json!({}),
            });
        }
    }
    VerifyResult { mode: mode.into(), compared, mismatches }
}

pub fn verify_repeat(reference_chain: &str, chain: &str) -> VerifyResult {
    let mut mismatches = Vec::new();
    if chain != reference_chain {
        mismatches.push(Mismatch { frame: -1, kind: "repeat".into(), expected: Some(reference_chain.into()), actual: Some(chain.into()), chunk: None, detail: json!({"what": "runChainHash"}) });
    }
    VerifyResult { mode: "repeat".into(), compared: 1, mismatches }
}
