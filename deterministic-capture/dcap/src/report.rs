//! capture-report.json (DESIGN.md section 10).

use std::path::Path;
use std::time::Instant;

use serde_json::{json, Value};

use crate::plan::Plan;
use crate::render::{file_sha256, ChunkResult};
use crate::spec::Spec;
use crate::verify::VerifyResult;

fn read_trim(path: &str) -> Option<String> {
    std::fs::read_to_string(path).ok().map(|s| s.trim().to_string())
}

/// Fingerprint of the installed fonts: sorted file names and sizes under the
/// usual font directories, hashed. No subprocess.
fn fonts_fingerprint() -> String {
    let mut entries: Vec<String> = Vec::new();
    for root in ["/usr/share/fonts", "/usr/local/share/fonts", &format!("{}/.fonts", std::env::var("HOME").unwrap_or_default())] {
        let mut stack = vec![std::path::PathBuf::from(root)];
        while let Some(dir) = stack.pop() {
            if let Ok(rd) = std::fs::read_dir(&dir) {
                for e in rd.flatten() {
                    let p = e.path();
                    if p.is_dir() {
                        stack.push(p);
                    } else if let Ok(md) = e.metadata() {
                        entries.push(format!("{}:{}", p.display(), md.len()));
                    }
                }
            }
        }
    }
    entries.sort();
    crate::encode::sha256_hex(entries.join("\n").as_bytes())[..16].to_string()
}

pub fn backend_identity(renderer: &str) -> Value {
    json!({
        "glRenderer": renderer,
        "kernel": read_trim("/proc/sys/kernel/osrelease"),
        "nvidiaDriver": read_trim("/proc/driver/nvidia/version"),
        "libav": crate::encode::library_versions(),
        "fontsFingerprint": fonts_fingerprint(),
    })
}

pub struct ReportInputs<'a> {
    pub spec: &'a Spec,
    pub status: &'a str,
    pub browser_path: &'a Path,
    pub capabilities: &'a Value,
    pub launch: &'a Value,
    pub plan: Option<&'a Plan>,
    pub results: &'a [ChunkResult],
    pub verify_results: &'a [VerifyResult],
    pub join_info: Option<&'a Value>,
    pub failure: Option<&'a Value>,
    pub run_chain_hash: Option<&'a str>,
    pub work_dir: &'a Path,
    pub out_dir: &'a Path,
    pub started: Instant,
}

pub fn write_hash_file(path: &Path, hashes: &std::collections::BTreeMap<i64, String>, start: i64, end: i64) -> std::io::Result<()> {
    let mut bytes = Vec::with_capacity(((end - start).max(0) as usize) * 8);
    for i in start..end {
        match hashes.get(&i) {
            Some(h) if h.len() >= 16 => {
                for k in 0..8 {
                    bytes.push(u8::from_str_radix(&h[2 * k..2 * k + 2], 16).unwrap_or(0));
                }
            }
            _ => bytes.extend_from_slice(&[0u8; 8]),
        }
    }
    std::fs::write(path, bytes)
}

pub fn build_report(r: &ReportInputs<'_>) -> std::io::Result<Value> {
    let hashes_dir = r.out_dir.join("hashes");
    std::fs::create_dir_all(&hashes_dir)?;
    let mut chunks = Vec::new();
    for c in r.results {
        let hash_path = hashes_dir.join(format!("chunk_{:02}.sha256x8", c.chunk.index));
        write_hash_file(&hash_path, &c.hashes, c.chunk.start, c.chunk.end)?;
        chunks.push(json!({
            "index": c.chunk.index, "start": c.chunk.start, "end": c.chunk.end, "frames": c.chunk.frames(), "ok": c.ok(),
            "output": c.output, "outputSha256": c.output.as_deref().and_then(file_sha256),
            "packets": c.packets, "extradataSha256": c.extradata_sha256, "chainHash": c.chain_hash, "hashFile": hash_path,
            "audioSampleFrames": c.audio_sample_frames, "pcmHash": c.pcm_hash,
            "attempts": c.attempts.iter().map(|a| a.to_json()).collect::<Vec<_>>(),
        }));
    }
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let report = json!({
        "status": r.status,
        "dcap": crate::VERSION,
        "generatedAtUnix": now,
        "elapsedSeconds": (r.started.elapsed().as_secs_f64() * 1000.0).round() / 1000.0,
        "spec": r.spec.raw, "specHash": r.spec.spec_hash(),
        "browser": {"path": r.browser_path, "sha256": file_sha256(r.browser_path), "capabilities": r.capabilities},
        "launch": r.launch,
        "backend": backend_identity(r.plan.map(|p| p.renderer.as_str()).unwrap_or("")),
        "plan": r.plan.map(Plan::to_json),
        "chunks": chunks,
        "runChainHash": r.run_chain_hash,
        "verify": r.verify_results.iter().map(VerifyResult::to_json).collect::<Vec<_>>(),
        "join": r.join_info,
        "failure": r.failure,
        "workDir": r.work_dir,
    });
    std::fs::create_dir_all(r.out_dir)?;
    std::fs::write(r.out_dir.join("capture-report.json"), serde_json::to_string_pretty(&report).unwrap())?;
    Ok(report)
}
