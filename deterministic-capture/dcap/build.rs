//! Compiles the libav shim and links the static ffmpeg built by
//! `ffmpeg/build.sh`. The prefix is `DCAP_FFMPEG_PREFIX` or `../ffmpeg/out/prefix`.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let prefix = std::env::var("DCAP_FFMPEG_PREFIX")
        .map(PathBuf::from)
        .unwrap_or_else(|_| manifest.join("../ffmpeg/out/prefix"));
    let include = prefix.join("include");
    let lib = prefix.join("lib");
    if !lib.join("libavcodec.a").exists() {
        panic!(
            "static ffmpeg not found at {}; run ffmpeg/build.sh or set DCAP_FFMPEG_PREFIX",
            prefix.display()
        );
    }
    println!("cargo:rerun-if-env-changed=DCAP_FFMPEG_PREFIX");
    println!("cargo:rerun-if-changed=csrc/dcap_av.c");
    println!("cargo:rerun-if-changed=csrc/dcap_av.h");
    cc::Build::new()
        .file("csrc/dcap_av.c")
        .include(&include)
        .include("csrc")
        .flag_if_supported("-Wno-deprecated-declarations")
        .opt_level(2)
        .compile("dcap_av");
    println!("cargo:rustc-link-search=native={}", lib.display());
    // Order matters for static archives: formats depend on codecs, both on util.
    for l in ["avformat", "avcodec", "swscale", "avutil"] {
        println!("cargo:rustc-link-lib=static={l}");
    }
    for l in ["m", "pthread", "dl", "atomic"] {
        println!("cargo:rustc-link-lib={l}");
    }
}
