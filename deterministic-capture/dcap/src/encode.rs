//! Safe wrapper over the libav shim (`csrc/dcap_av.c`): codec parameters,
//! container choice, encoding of ring frames, chunk probing and the join.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::path::Path;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

#[repr(C)]
struct RawParams {
    codec: *const c_char,
    pix_fmt: *const c_char,
    width: c_int,
    height: c_int,
    fps_num: c_int,
    fps_den: c_int,
    threads: c_int,
    options: *const *const c_char,
    n_options: c_int,
    container: *const c_char,
    path: *const c_char,
    audio_codec: *const c_char,
    audio_rate: c_int,
    audio_channels: c_int,
    audio_sample_format: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawStats {
    video_frames_in: i64,
    video_packets: i64,
    audio_sample_frames: i64,
    audio_packets: i64,
    bytes_written: i64,
    extradata_len: c_int,
    extradata: [u8; 4096],
    encode_ms_total: f64,
    convert_ms_total: f64,
}

#[repr(C)]
struct RawProbe {
    codec: [c_char; 32],
    pix_fmt: [c_char; 32],
    width: c_int,
    height: c_int,
    video_packets: i64,
    video_first_keyframe: c_int,
    video_all_keyframes: c_int,
    audio_sample_frames: i64,
    has_audio: c_int,
    extradata_len: c_int,
    extradata: [u8; 4096],
    time_base_num: c_int,
    time_base_den: c_int,
    first_pts_delta: i64,
}

#[repr(C)]
struct RawJoinInput {
    path: *const c_char,
    first_frame: i64,
    frames: i64,
    audio_sample_frames: i64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct RawJoinStats {
    video_packets: i64,
    audio_sample_frames: i64,
    bytes_written: i64,
}

#[repr(C)]
struct DcapEnc {
    _private: [u8; 0],
}

extern "C" {
    fn dcap_enc_open(params: *const RawParams, err: *mut c_char, errlen: usize) -> *mut DcapEnc;
    fn dcap_enc_video(enc: *mut DcapEnc, bgra: *const u8, stride: c_int, pts: i64, err: *mut c_char, errlen: usize) -> c_int;
    fn dcap_enc_audio(enc: *mut DcapEnc, pcm: *const u8, sample_frames: c_int, pts: i64, err: *mut c_char, errlen: usize) -> c_int;
    fn dcap_enc_finish(enc: *mut DcapEnc, stats: *mut RawStats, err: *mut c_char, errlen: usize) -> c_int;
    fn dcap_enc_abort(enc: *mut DcapEnc);
    fn dcap_codec_probe(params: *const RawParams, err: *mut c_char, errlen: usize) -> c_int;
    fn dcap_probe(path: *const c_char, info: *mut RawProbe, err: *mut c_char, errlen: usize) -> c_int;
    fn dcap_join(
        inputs: *const RawJoinInput,
        n_inputs: c_int,
        start_frame: i64,
        out_path: *const c_char,
        container: *const c_char,
        fps_num: c_int,
        fps_den: c_int,
        audio_rate: c_int,
        stats: *mut RawJoinStats,
        err: *mut c_char,
        errlen: usize,
    ) -> c_int;
    fn dcap_av_versions(buf: *mut c_char, len: usize);
    fn dcap_av_encoders(buf: *mut c_char, len: usize);
}

const ERRLEN: usize = 512;

fn err_string(buf: &[c_char]) -> String {
    unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned()
}

#[derive(Debug, Clone)]
pub struct EncodeError {
    pub kind: String,
    pub message: String,
}

impl EncodeError {
    fn from_shim(msg: String) -> EncodeError {
        let (kind, rest) = match msg.split_once(':') {
            Some((k, r)) if !k.contains(' ') => (k.to_string(), r.trim().to_string()),
            _ => ("EncoderFailed".to_string(), msg.clone()),
        };
        EncodeError { kind, message: rest }
    }
}

impl std::fmt::Display for EncodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.kind, self.message)
    }
}

impl std::error::Error for EncodeError {}

/// Codec parameters as they appear in the spec and in the report.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct CodecSpec {
    pub codec: String,
    #[serde(rename = "pixelFormat")]
    pub pixel_format: String,
    #[serde(default)]
    pub options: Vec<(String, String)>,
    #[serde(default)]
    pub threads: i32,
    pub container: String,
    #[serde(default)]
    pub audio_codec: Option<String>,
}

/// Per-codec allowlist of options the spec may set, with their defaults.
pub fn codec_defaults(codec: &str) -> Option<(&'static str, Vec<(&'static str, &'static str)>, &'static str)> {
    // (default pixel format, default options, default container)
    match codec {
        "ffv1" => Some(("bgr0", vec![("level", "3"), ("slices", "16"), ("slicecrc", "1"), ("g", "1"), ("context", "1"), ("coder", "range_def")], "mp4")),
        "utvideo" => Some(("gbrp", vec![("pred", "median")], "mov")),
        "magicyuv" => Some(("gbrp", vec![("pred", "median")], "mov")),
        // The median predictor refuses RGB input; plane prediction is the RGB-capable one.
        "ffvhuff" => Some(("bgra", vec![("pred", "plane")], "mov")),
        "rawvideo" => Some(("bgra", vec![], "mov")),
        // g=0 is libavcodec's intra-only mode for NVENC (frameIntervalP 0); g=1 is rejected by the
        // driver ("Gop Length should be greater than number of B frames + 1") whatever bf is.
        "h264_nvenc" => Some(("yuv444p", vec![("preset", "p4"), ("tune", "lossless"), ("profile", "high444p"), ("g", "0"), ("bf", "0")], "mp4")),
        "hevc_nvenc" => Some(("yuv444p", vec![("preset", "p4"), ("tune", "lossless"), ("profile", "rext"), ("g", "0"), ("bf", "0")], "mp4")),
        _ => None,
    }
}

pub const ALLOWED_OPTIONS: &[&str] = &[
    "level", "slices", "slicecrc", "g", "context", "coder", "pred", "preset", "tune", "profile", "rc", "cq", "qp", "bf", "b", "maxrate", "bufsize", "spatial_aq", "temporal_aq", "rc-lookahead", "multipass", "tier", "pix_fmt",
];

/// Pixel formats that preserve the canonical RGBA hash exactly.
pub fn lossless_to_hash(pixel_format: &str) -> bool {
    matches!(pixel_format, "bgr0" | "bgra" | "gbrp" | "gbrap" | "rgb24" | "bgr24" | "0rgb" | "rgb0")
}

pub struct EncoderConfig<'a> {
    pub spec: &'a CodecSpec,
    pub width: u32,
    pub height: u32,
    pub fps_num: i32,
    pub fps_den: i32,
    pub path: &'a Path,
    pub audio: Option<AudioConfig>,
}

#[derive(Debug, Clone, Copy)]
pub struct AudioConfig {
    pub rate: i32,
    pub channels: i32,
    /// 1 = s16, 2 = f32
    pub sample_format: i32,
}

struct OwnedParams {
    _strings: Vec<CString>,
    _opts: Vec<*const c_char>,
    raw: RawParams,
}

fn build_params(cfg: &EncoderConfig<'_>) -> OwnedParams {
    let mut strings = Vec::new();
    let mut push = |s: &str| -> *const c_char {
        let c = CString::new(s).unwrap_or_default();
        let p = c.as_ptr();
        strings.push(c);
        p
    };
    let codec = push(&cfg.spec.codec);
    let pix = push(&cfg.spec.pixel_format);
    let container = push(&cfg.spec.container);
    let path = push(&cfg.path.to_string_lossy());
    let audio_codec = match (&cfg.audio, &cfg.spec.audio_codec) {
        (Some(_), Some(c)) => push(c),
        _ => std::ptr::null(),
    };
    let mut opts = Vec::new();
    for (k, v) in &cfg.spec.options {
        opts.push(push(k));
        opts.push(push(v));
    }
    let raw = RawParams {
        codec,
        pix_fmt: pix,
        width: cfg.width as c_int,
        height: cfg.height as c_int,
        fps_num: cfg.fps_num,
        fps_den: cfg.fps_den,
        threads: cfg.spec.threads,
        options: if opts.is_empty() { std::ptr::null() } else { opts.as_ptr() },
        n_options: (opts.len() / 2) as c_int,
        container,
        path,
        audio_codec,
        audio_rate: cfg.audio.map(|a| a.rate).unwrap_or(0),
        audio_channels: cfg.audio.map(|a| a.channels).unwrap_or(0),
        audio_sample_format: cfg.audio.map(|a| a.sample_format).unwrap_or(0),
    };
    OwnedParams { _strings: strings, _opts: opts, raw }
}

#[derive(Debug, Clone, Serialize)]
pub struct EncodeStats {
    pub video_frames_in: i64,
    pub video_packets: i64,
    pub audio_sample_frames: i64,
    pub audio_packets: i64,
    pub bytes_written: i64,
    pub extradata_sha256: Option<String>,
    pub encode_ms_total: f64,
    pub convert_ms_total: f64,
}

pub struct Encoder {
    handle: *mut DcapEnc,
}

unsafe impl Send for Encoder {}

impl Encoder {
    pub fn open(cfg: &EncoderConfig<'_>) -> Result<Encoder, EncodeError> {
        let params = build_params(cfg);
        let mut err = [0 as c_char; ERRLEN];
        let handle = unsafe { dcap_enc_open(&params.raw, err.as_mut_ptr(), ERRLEN) };
        if handle.is_null() {
            return Err(EncodeError::from_shim(err_string(&err)));
        }
        Ok(Encoder { handle })
    }

    pub fn video(&mut self, bgra: &[u8], width: u32, pts: i64) -> Result<(), EncodeError> {
        let mut err = [0 as c_char; ERRLEN];
        let rc = unsafe { dcap_enc_video(self.handle, bgra.as_ptr(), (width * 4) as c_int, pts, err.as_mut_ptr(), ERRLEN) };
        if rc != 0 {
            return Err(EncodeError::from_shim(err_string(&err)));
        }
        Ok(())
    }

    pub fn audio(&mut self, pcm: &[u8], sample_frames: i32, pts: i64) -> Result<(), EncodeError> {
        let mut err = [0 as c_char; ERRLEN];
        let rc = unsafe { dcap_enc_audio(self.handle, pcm.as_ptr(), sample_frames, pts, err.as_mut_ptr(), ERRLEN) };
        if rc != 0 {
            return Err(EncodeError::from_shim(err_string(&err)));
        }
        Ok(())
    }

    /// Flushes, writes the trailer and closes. Consumes the encoder.
    pub fn finish(self) -> Result<EncodeStats, EncodeError> {
        let mut err = [0 as c_char; ERRLEN];
        let mut stats: RawStats = unsafe { std::mem::zeroed() };
        let handle = self.handle;
        std::mem::forget(self);
        let rc = unsafe { dcap_enc_finish(handle, &mut stats, err.as_mut_ptr(), ERRLEN) };
        if rc != 0 {
            return Err(EncodeError::from_shim(err_string(&err)));
        }
        let extradata_sha256 = if stats.extradata_len > 0 {
            Some(hex(&Sha256::digest(&stats.extradata[..stats.extradata_len as usize])))
        } else {
            None
        };
        Ok(EncodeStats {
            video_frames_in: stats.video_frames_in,
            video_packets: stats.video_packets,
            audio_sample_frames: stats.audio_sample_frames,
            audio_packets: stats.audio_packets,
            bytes_written: stats.bytes_written,
            extradata_sha256,
            encode_ms_total: stats.encode_ms_total,
            convert_ms_total: stats.convert_ms_total,
        })
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { dcap_enc_abort(self.handle) };
    }
}

/// Opens and closes an encoder with these parameters to report availability.
pub fn probe_codec(cfg: &EncoderConfig<'_>) -> Result<(), EncodeError> {
    let params = build_params(cfg);
    let mut err = [0 as c_char; ERRLEN];
    let rc = unsafe { dcap_codec_probe(&params.raw, err.as_mut_ptr(), ERRLEN) };
    if rc != 0 {
        return Err(EncodeError::from_shim(err_string(&err)));
    }
    Ok(())
}

#[derive(Debug, Clone, Serialize)]
pub struct ProbeInfo {
    pub codec: String,
    pub pixel_format: String,
    pub width: i32,
    pub height: i32,
    pub video_packets: i64,
    pub first_keyframe: bool,
    pub all_keyframes: bool,
    pub audio_sample_frames: i64,
    pub has_audio: bool,
    pub extradata_sha256: Option<String>,
    pub time_base: (i32, i32),
    /// pts difference between the first two video packets in time-base units
    pub first_pts_delta: i64,
}

pub fn probe_file(path: &Path) -> Result<ProbeInfo, EncodeError> {
    let c_path = CString::new(path.to_string_lossy().as_bytes()).unwrap_or_default();
    let mut info: RawProbe = unsafe { std::mem::zeroed() };
    let mut err = [0 as c_char; ERRLEN];
    let rc = unsafe { dcap_probe(c_path.as_ptr(), &mut info, err.as_mut_ptr(), ERRLEN) };
    if rc != 0 {
        return Err(EncodeError::from_shim(err_string(&err)));
    }
    Ok(ProbeInfo {
        codec: err_string(&info.codec),
        pixel_format: err_string(&info.pix_fmt),
        width: info.width,
        height: info.height,
        video_packets: info.video_packets,
        first_keyframe: info.video_first_keyframe != 0,
        all_keyframes: info.video_all_keyframes != 0,
        audio_sample_frames: info.audio_sample_frames,
        has_audio: info.has_audio != 0,
        extradata_sha256: if info.extradata_len > 0 { Some(hex(&Sha256::digest(&info.extradata[..info.extradata_len as usize]))) } else { None },
        time_base: (info.time_base_num, info.time_base_den),
        first_pts_delta: info.first_pts_delta,
    })
}

pub struct JoinInput<'a> {
    pub path: &'a Path,
    pub first_frame: i64,
    pub frames: i64,
    pub audio_sample_frames: i64,
}

#[derive(Debug, Clone, Serialize)]
pub struct JoinStats {
    pub video_packets: i64,
    pub audio_sample_frames: i64,
    pub bytes_written: i64,
}

pub fn join(inputs: &[JoinInput<'_>], start_frame: i64, out: &Path, container: &str, fps_num: i32, fps_den: i32, audio_rate: i32) -> Result<JoinStats, EncodeError> {
    let paths: Vec<CString> = inputs.iter().map(|i| CString::new(i.path.to_string_lossy().as_bytes()).unwrap_or_default()).collect();
    let raw: Vec<RawJoinInput> = inputs
        .iter()
        .zip(&paths)
        .map(|(i, p)| RawJoinInput { path: p.as_ptr(), first_frame: i.first_frame, frames: i.frames, audio_sample_frames: i.audio_sample_frames })
        .collect();
    let c_out = CString::new(out.to_string_lossy().as_bytes()).unwrap_or_default();
    let c_container = CString::new(container).unwrap_or_default();
    let mut stats = RawJoinStats::default();
    let mut err = [0 as c_char; ERRLEN];
    let rc = unsafe {
        dcap_join(raw.as_ptr(), raw.len() as c_int, start_frame, c_out.as_ptr(), c_container.as_ptr(), fps_num, fps_den, audio_rate, &mut stats, err.as_mut_ptr(), ERRLEN)
    };
    if rc != 0 {
        return Err(EncodeError::from_shim(err_string(&err)));
    }
    Ok(JoinStats { video_packets: stats.video_packets, audio_sample_frames: stats.audio_sample_frames, bytes_written: stats.bytes_written })
}

pub fn library_versions() -> String {
    let mut buf = [0 as c_char; 256];
    unsafe { dcap_av_versions(buf.as_mut_ptr(), buf.len()) };
    err_string(&buf)
}

pub fn available_encoders() -> Vec<String> {
    let mut buf = [0 as c_char; 4096];
    unsafe { dcap_av_encoders(buf.as_mut_ptr(), buf.len()) };
    err_string(&buf).split(',').filter(|s| !s.is_empty()).map(str::to_string).collect()
}

pub fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    hex(&Sha256::digest(bytes))
}
