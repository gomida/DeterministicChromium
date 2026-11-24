//! Browser launch configuration (DESIGN.md 4.1, 5.5, 6). Everything that
//! reaches the command line or the environment is built here from the spec so
//! the report can record it and two runs can be diffed.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::spec::Spec;

pub const ENGINE_SWITCH: &str = "--deterministic-capture";

const BASE_FLAGS: &[&str] = &[
    ENGINE_SWITCH,
    "--no-sandbox",
    "--disable-dev-shm-usage",
    "--force-color-profile=srgb",
    "--font-render-hinting=none",
    "--disable-lcd-text",
    "--hide-scrollbars",
    "--disable-software-rasterizer",
    "--ignore-gpu-blocklist",
    "--enable-gpu-rasterization",
    "--force-effective-connection-type=4G",
    "--disable-background-networking",
    "--disable-component-update",
    "--disable-default-apps",
    "--disable-sync",
    "--no-first-run",
    "--enable-logging=stderr",
    "--v=0",
];

fn backend_flags(backend: &str) -> &'static [&'static str] {
    match backend {
        "angle-gl-d3d12" => &["--use-angle=gles-egl", "--enable-gpu", "--disable-gpu-sandbox"],
        "angle-vulkan" => &["--use-angle=vulkan", "--enable-features=Vulkan", "--disable-vulkan-surface", "--enable-gpu", "--disable-gpu-sandbox"],
        _ => &["--use-angle=swiftshader"],
    }
}

fn backend_env(backend: &str) -> Vec<(&'static str, &'static str)> {
    match backend {
        "angle-gl-d3d12" => vec![("GALLIUM_DRIVER", "d3d12"), ("MESA_LOADER_DRIVER_OVERRIDE", "d3d12")],
        _ => vec![],
    }
}

const SOFTWARE_MARKERS: &[&str] = &["llvmpipe", "lavapipe", "SwiftShader", "softpipe"];

#[derive(Debug, Clone)]
pub struct LaunchPlan {
    pub executable: PathBuf,
    pub args: Vec<String>,
    pub env: HashMap<String, String>,
    pub profile_dir: PathBuf,
}

impl LaunchPlan {
    pub fn as_report(&self) -> Value {
        let keep = ["GALLIUM_DRIVER", "MESA_LOADER_DRIVER_OVERRIDE", "LD_LIBRARY_PATH", "__EGL_VENDOR_LIBRARY_FILENAMES", "TZ"];
        let env: serde_json::Map<String, Value> = self.env.iter().filter(|(k, _)| keep.contains(&k.as_str())).map(|(k, v)| (k.clone(), json!(v))).collect();
        json!({"executable": self.executable, "args": self.args, "env": env})
    }
}

pub fn launch_plan(spec: &Spec, executable: &Path, work_dir: &Path, worker: &str) -> std::io::Result<LaunchPlan> {
    let profile = work_dir.join(format!("profile-{worker}"));
    std::fs::create_dir_all(&profile)?;
    let tmp = work_dir.join("tmp");
    std::fs::create_dir_all(&tmp)?;
    let mut js_flags = vec![format!("--random-seed={}", spec.determinism.seed)];
    if spec.determinism.predictable_gc {
        js_flags.push("--predictable".into());
        js_flags.push("--single-threaded-gc".into());
    }
    let mut args: Vec<String> = BASE_FLAGS.iter().map(|s| s.to_string()).collect();
    args.extend(backend_flags(&spec.backend).iter().map(|s| s.to_string()));
    args.push(format!("--js-flags={}", js_flags.join(" ")));
    args.push(format!("--user-data-dir={}", profile.display()));
    args.push(format!("--window-size={},{}", spec.viewport.width, spec.viewport.height));
    args.push(format!("--lang={}", spec.determinism.locale));
    args.push("about:blank".into());
    let mut env: HashMap<String, String> = std::env::vars().collect();
    for (k, v) in backend_env(&spec.backend) {
        env.insert(k.to_string(), v.to_string());
    }
    env.insert("TMPDIR".into(), tmp.display().to_string());
    env.insert("TZ".into(), spec.determinism.timezone.clone());
    env.remove("VK_ICD_FILENAMES");
    Ok(LaunchPlan { executable: executable.to_path_buf(), args, env, profile_dir: profile })
}

/// Checks the GL renderer string the browser reports against the backend.
pub fn renderer_is_acceptable(backend: &str, gl_renderer: &str) -> Result<(), String> {
    if backend != "swiftshader" && SOFTWARE_MARKERS.iter().any(|m| gl_renderer.contains(m)) {
        return Err(format!("software renderer selected: {gl_renderer}"));
    }
    let markers: &[&str] = match backend {
        "angle-gl-d3d12" => &["D3D12 ("],
        "angle-vulkan" => &["Vulkan 1."],
        _ => &["SwiftShader"],
    };
    if !markers.iter().any(|m| gl_renderer.contains(m)) {
        return Err(format!("renderer does not match backend {backend}: {gl_renderer}"));
    }
    Ok(())
}
