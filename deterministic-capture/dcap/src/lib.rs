//! dcap: deterministic web-to-video capture. Controller, in-process encoder
//! and chunk join in one binary (DESIGN.md sections 5 and 13).

pub mod browser;
pub mod cdp;
pub mod cli;
pub mod encode;
pub mod engine;
pub mod launch;
pub mod plan;
pub mod render;
pub mod report;
pub mod scenario;
pub mod scenarios;
pub mod session;
pub mod shm;
pub mod smoke;
pub mod spec;
pub mod verify;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
