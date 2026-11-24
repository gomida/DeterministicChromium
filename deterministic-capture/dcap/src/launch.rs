//! Launches `headless_shell --remote-debugging-pipe` with the CDP pipe ends on
//! fds 3 and 4 and any extra descriptors (the shared-memory ring and its
//! eventfds) at fixed numbers above them. The child starts a new session so
//! the whole group can be killed, and dies with the controller through
//! `PR_SET_PDEATHSIG`.

use std::collections::HashMap;
use std::fs::OpenOptions;
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd, OwnedFd, RawFd};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::cdp::{pipe2_cloexec, CdpError, Connection, Session};

/// Child-side descriptor numbers that the engine's `shm` sink expects.
pub const FD_RING: RawFd = 5;
pub const FD_READY: RawFd = 6;
pub const FD_FREE: RawFd = 7;

pub struct BrowserProcess {
    pub child: Child,
    pub conn: Arc<Connection>,
    pub browser: Session,
    pub log_path: std::path::PathBuf,
}

/// Moves `fd` above `min` so later dup2 calls cannot clobber it.
fn raise_fd(fd: OwnedFd, min: RawFd) -> std::io::Result<OwnedFd> {
    let mut current = fd;
    while current.as_raw_fd() < min {
        let dup = unsafe { libc::fcntl(current.as_raw_fd(), libc::F_DUPFD_CLOEXEC, min) };
        if dup < 0 {
            return Err(std::io::Error::last_os_error());
        }
        current = unsafe { OwnedFd::from_raw_fd(dup) };
    }
    Ok(current)
}

/// `extra_fds` maps a child fd number to the descriptor to install there.
pub fn launch(
    executable: &Path,
    args: &[String],
    env: &HashMap<String, String>,
    log_path: &Path,
    extra_fds: Vec<(RawFd, OwnedFd)>,
    default_timeout: Duration,
) -> std::io::Result<BrowserProcess> {
    let (a_r, a_w) = pipe2_cloexec()?; // controller -> browser (child fd 3)
    let (b_r, b_w) = pipe2_cloexec()?; // browser -> controller (child fd 4)
    let max_target = extra_fds.iter().map(|(n, _)| *n).max().unwrap_or(4).max(4);
    let a_r = raise_fd(a_r, max_target + 1)?;
    let b_w = raise_fd(b_w, max_target + 1)?;
    let mut mapping: Vec<(RawFd, RawFd)> = vec![(3, a_r.as_raw_fd()), (4, b_w.as_raw_fd())];
    let mut raised_extra = Vec::new();
    for (target, fd) in extra_fds {
        let fd = raise_fd(fd, max_target + 1)?;
        mapping.push((target, fd.as_raw_fd()));
        raised_extra.push(fd);
    }
    if let Some(parent) = log_path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let log = OpenOptions::new().create(true).append(true).open(log_path)?;
    let log_err = log.try_clone()?;
    let mut cmd = Command::new(executable);
    cmd.arg("--remote-debugging-pipe").args(args);
    cmd.env_clear().envs(env);
    cmd.stdin(Stdio::null()).stdout(Stdio::from(log)).stderr(Stdio::from(log_err));
    let mapping_for_child = mapping.clone();
    unsafe {
        cmd.pre_exec(move || {
            libc::setsid();
            for (target, source) in &mapping_for_child {
                if libc::dup2(*source, *target) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
            }
            libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL);
            Ok(())
        });
    }
    let child = cmd.spawn()?;
    drop(a_r);
    drop(b_w);
    drop(raised_extra);
    let conn = Connection::new(b_r, a_w);
    let browser = Session { conn: Arc::clone(&conn), id: None, default_timeout };
    Ok(BrowserProcess { child, conn, browser, log_path: log_path.to_path_buf() })
}

impl BrowserProcess {
    /// Attaches a flattened session to the first page target.
    pub fn attach_page(&self) -> Result<Session, CdpError> {
        let targets = self.browser.send("Target.getTargets", json!({}))?;
        let page = targets["targetInfos"]
            .as_array()
            .and_then(|a| a.iter().find(|t| t["type"] == "page"))
            .ok_or_else(|| CdpError::Closed("no page target".into()))?;
        let r = self
            .browser
            .send("Target.attachToTarget", json!({"targetId": page["targetId"], "flatten": true}))?;
        Ok(Session {
            conn: Arc::clone(&self.conn),
            id: r["sessionId"].as_str().map(str::to_string),
            default_timeout: self.browser.default_timeout,
        })
    }

    /// Asks the browser to close, then waits up to `grace`, then kills.
    pub fn close(&mut self, grace: Duration) -> i32 {
        if !self.conn.is_closed() {
            let _ = self.conn.send(None, "Browser.close", json!({}), grace);
        }
        self.conn.close();
        let deadline = Instant::now() + grace;
        loop {
            match self.child.try_wait() {
                Ok(Some(status)) => return status.code().unwrap_or(-1),
                Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
                _ => break,
            }
        }
        let _ = self.child.kill();
        let code = self.child.wait().ok().and_then(|s| s.code()).unwrap_or(-9);
        self.conn.join_reader();
        code
    }

    pub fn last_log_lines(&self, n: usize) -> Vec<String> {
        std::fs::read_to_string(&self.log_path)
            .map(|s| s.lines().rev().take(n).map(str::to_string).collect::<Vec<_>>().into_iter().rev().collect())
            .unwrap_or_default()
    }
}

/// Converts an OwnedFd into a raw fd that the caller now owns.
pub fn into_raw(fd: OwnedFd) -> RawFd {
    fd.into_raw_fd()
}

pub fn value_or_empty(v: Option<&Value>) -> Value {
    v.cloned().unwrap_or_else(|| json!({}))
}
