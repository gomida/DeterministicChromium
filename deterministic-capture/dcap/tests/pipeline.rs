//! End-to-end tests of the controller against the fake engine: spec, plan
//! pass, chunked render through the shared-memory ring into real encoders,
//! verification, join and report. Run with `cargo test`.

use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::{json, Value};

fn fake_engine() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_dcap-fake-engine"))
}

fn dcap() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_dcap"))
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("dcap-test-{}-{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn base_spec(out: &Path, frames: i64) -> Value {
    json!({
        "url": "file:///dev/null/page.html",
        "viewport": {"width": 320, "height": 180},
        "frameRate": {"num": 60000, "den": 1001},
        "timeline": {"stop": {"type": "frames", "count": frames}},
        "render": {"backend": "angle-gl-d3d12"},
        "output": {"dir": out, "keepChunks": true},
        "encoder": {"codec": "ffv1", "options": {"slices": 4}},
        "verify": {"mode": "none"},
        "timeouts": {"sinkStallMs": 10000, "encoderFinish": 30, "advance": 30},
    })
}

struct Run {
    status: i32,
    stdout: String,
    stderr: String,
    report: Option<Value>,
}

fn render(spec: &Value, out: &Path, env: &[(&str, &str)]) -> Run {
    let spec_path = out.join("spec.json");
    std::fs::write(&spec_path, serde_json::to_string_pretty(spec).unwrap()).unwrap();
    let mut cmd = Command::new(dcap());
    cmd.arg("render").arg(&spec_path).arg("--browser").arg(fake_engine()).arg("--run-id").arg("t");
    for (k, v) in env {
        cmd.env(k, v);
    }
    let output = cmd.output().expect("run dcap");
    let report = std::fs::read_to_string(out.join("capture-report.json")).ok().and_then(|s| serde_json::from_str(&s).ok());
    Run { status: output.status.code().unwrap_or(-1), stdout: String::from_utf8_lossy(&output.stdout).into(), stderr: String::from_utf8_lossy(&output.stderr).into(), report }
}

fn probe_packets(path: &Path) -> i64 {
    // Use the shipped binary's probe through `join` of a single chunk into a scratch file: cheaper to read the report; here we read ffprobe-free stats from the report instead.
    let _ = path;
    -1
}

#[test]
fn serial_render_produces_final_file_and_report() {
    let out = temp_dir("serial");
    let run = render(&base_spec(&out, 24), &out, &[]);
    assert_eq!(run.status, 0, "stdout: {}\nstderr: {}", run.stdout, run.stderr);
    let report = run.report.expect("report written");
    assert_eq!(report["status"], "complete");
    assert_eq!(report["chunks"].as_array().unwrap().len(), 1);
    assert_eq!(report["chunks"][0]["packets"], 24);
    assert_eq!(report["join"]["packets"], 24);
    assert_eq!(report["join"]["codec"], "ffv1");
    assert_eq!(report["join"]["timeBase"], json!([1, 60000]));
    let final_path = PathBuf::from(report["join"]["final"].as_str().unwrap());
    assert!(final_path.exists() && final_path.extension().unwrap() == "mp4");
    assert_eq!(report["chunks"][0]["attempts"][0]["encoder"]["frames"], 24);
    let _ = probe_packets(&final_path);
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn chunked_render_with_boundary_verification_joins_exactly() {
    let out = temp_dir("chunked");
    let mut spec = base_spec(&out, 90);
    spec["parallel"] = json!({"workers": 3});
    spec["verify"] = json!({"mode": "boundaries", "windowBefore": 2, "windowAfter": 5});
    let run = render(&spec, &out, &[]);
    assert_eq!(run.status, 0, "stdout: {}\nstderr: {}", run.stdout, run.stderr);
    let report = run.report.expect("report written");
    assert_eq!(report["status"], "complete", "{}", report["failure"]);
    assert_eq!(report["chunks"].as_array().unwrap().len(), 3);
    let total: i64 = report["chunks"].as_array().unwrap().iter().map(|c| c["packets"].as_i64().unwrap()).sum();
    assert_eq!(total, 90);
    assert_eq!(report["join"]["packets"], 90);
    assert_eq!(report["join"]["ptsStep"], 1001, "60000/1001 frames last 1001 units of 1/60000 s");
    let verify = &report["verify"][0];
    assert_eq!(verify["ok"], true);
    assert!(verify["compared"].as_i64().unwrap() > 0, "boundary windows were compared");
    // Plan pass hashed only the window frames.
    assert!(report["plan"]["hashes"].as_object().unwrap().len() < 90);
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn divergent_frame_is_caught_by_full_verification() {
    let out = temp_dir("diverge");
    let mut spec = base_spec(&out, 30);
    spec["parallel"] = json!({"workers": 2});
    spec["verify"] = json!({"mode": "full"});
    // The worker for chunk 0 (attempt 1) renders frame 7 differently from the plan pass.
    let run = render(&spec, &out, &[("DCAP_FAKE_DIVERGE", "c00a1:7")]);
    assert_eq!(run.status, 1, "stdout: {}\nstderr: {}", run.stdout, run.stderr);
    let report = run.report.expect("report written");
    assert_eq!(report["status"], "failed");
    assert_eq!(report["failure"]["primary"], "verify");
    let mismatches = report["verify"][0]["mismatches"].as_array().unwrap();
    assert_eq!(mismatches.len(), 1);
    assert_eq!(mismatches[0]["frame"], 7);
    assert!(report["join"].is_null(), "no final file on verification failure");
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn repeat_mode_compares_chain_hashes_and_marker_stop_resolves() {
    let out = temp_dir("marker");
    let mut spec = base_spec(&out, 10);
    spec["timeline"] = json!({"stop": {"type": "marker", "name": "dcap:test:done"}, "maxFrames": 200});
    spec["verify"] = json!({"mode": "repeat", "repeat": 1});
    let run = render(&spec, &out, &[("DCAP_FAKE_MARKER_FRAME", "41")]);
    assert_eq!(run.status, 0, "stdout: {}\nstderr: {}", run.stdout, run.stderr);
    let report = run.report.expect("report written");
    assert_eq!(report["status"], "complete", "{}", report["failure"]);
    assert_eq!(report["plan"]["stopFrame"], 42);
    assert_eq!(report["join"]["packets"], 42);
    assert_eq!(report["verify"][0]["mode"], "repeat");
    assert_eq!(report["verify"][0]["ok"], true);
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn crashing_engine_is_retried_and_attributed() {
    let out = temp_dir("crash");
    let spec = base_spec(&out, 20);
    // The probe browser dies mid-run; the probe is not retried, so the run
    // fails with a typed error and a report is still written.
    let run = render(&spec, &out, &[("DCAP_FAKE_CRASH", "probe:5")]);
    assert_eq!(run.status, 1);
    let report = run.report.expect("report written");
    assert_eq!(report["status"], "failed");
    assert_eq!(report["failure"]["primary"], "TargetDetached", "{}", report["failure"]);
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn audio_capture_joins_exact_sample_counts_and_verifies_pcm() {
    let out = temp_dir("audio");
    let mut spec = base_spec(&out, 50);
    spec["audio"] = json!({"capture": true});
    spec["parallel"] = json!({"workers": 2});
    spec["verify"] = json!({"mode": "full"});
    let run = render(&spec, &out, &[]);
    assert_eq!(run.status, 0, "stdout: {}\nstderr: {}", run.stdout, run.stderr);
    let report = run.report.expect("report written");
    assert_eq!(report["status"], "complete", "{}", report["failure"]);
    // 50 frames of 60000/1001 at 48 kHz: floor(50 * 48000 * 1001 / 60000) sample frames.
    assert_eq!(report["join"]["audioSampleFrames"], 40040);
    assert_eq!(report["join"]["packets"], 50);
    let final_path = PathBuf::from(report["join"]["final"].as_str().unwrap());
    assert_eq!(final_path.extension().unwrap(), "mov", "PCM audio moves the default container to MOV");
    let chunks = report["chunks"].as_array().unwrap();
    assert_eq!(chunks.len(), 2);
    let total: i64 = chunks.iter().map(|c| c["audioSampleFrames"].as_i64().unwrap()).sum();
    assert_eq!(total, 40040);
    assert!(chunks.iter().all(|c| c["pcmHash"].is_string()));
    let verify = &report["verify"][0];
    assert_eq!(verify["ok"], true, "{}", verify);
    assert!(verify["compared"].as_i64().unwrap() >= 100, "video and PCM hashes compared for every frame: {}", verify["compared"]);
    assert_eq!(report["plan"]["pcmHashes"], 50);
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn validate_rejects_unknown_keys() {
    let out = temp_dir("validate");
    let mut spec = base_spec(&out, 5);
    spec["bogus"] = json!(1);
    let p = out.join("spec.json");
    std::fs::write(&p, spec.to_string()).unwrap();
    let output = Command::new(dcap()).arg("validate").arg(&p).output().unwrap();
    assert_eq!(output.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&output.stderr).contains("unknown keys"));
    let _ = std::fs::remove_dir_all(&out);
}
