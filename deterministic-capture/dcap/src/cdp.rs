//! Chrome DevTools Protocol client over `--remote-debugging-pipe`.
//!
//! The browser reads NUL-terminated JSON messages from fd 3 and writes replies
//! and events to fd 4. One connection speaks for the browser session and any
//! number of flattened target sessions. A reader thread dispatches responses
//! to waiting callers and events to subscribers; callers block with a timeout.

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::os::fd::{FromRawFd, OwnedFd};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

#[derive(Debug, Clone)]
pub enum CdpError {
    /// The browser answered with an error object.
    Protocol { method: String, code: i64, message: String, data: Option<Value> },
    /// The pipe closed.
    Closed(String),
    /// No answer within the timeout.
    Timeout(String),
}

impl std::fmt::Display for CdpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            CdpError::Protocol { method, code, message, data } => {
                write!(f, "{method}: [{code}] {message}")?;
                if let Some(d) = data {
                    write!(f, " {d}")?;
                }
                Ok(())
            }
            CdpError::Closed(why) => write!(f, "pipe closed: {why}"),
            CdpError::Timeout(method) => write!(f, "{method} timed out"),
        }
    }
}

impl std::error::Error for CdpError {}

struct Subscription {
    id: u64,
    session: Option<String>,
    method: String,
    tx: Sender<Value>,
}

struct Inner {
    writer: Mutex<File>,
    next_id: AtomicU64,
    pending: Mutex<HashMap<u64, (String, Sender<Result<Value, CdpError>>)>>,
    subs: Mutex<Vec<Subscription>>,
    next_sub: AtomicU64,
    closed: AtomicBool,
    rtt_us: Mutex<Vec<u64>>,
}

/// Low-level transport shared by all sessions.
pub struct Connection {
    inner: Arc<Inner>,
    reader: Mutex<Option<std::thread::JoinHandle<()>>>,
}

impl Connection {
    /// `from_browser` is the read end (browser's fd 4), `to_browser` the write
    /// end (browser's fd 3).
    pub fn new(from_browser: OwnedFd, to_browser: OwnedFd) -> Arc<Connection> {
        let inner = Arc::new(Inner {
            writer: Mutex::new(File::from(to_browser)),
            next_id: AtomicU64::new(1),
            pending: Mutex::new(HashMap::new()),
            subs: Mutex::new(Vec::new()),
            next_sub: AtomicU64::new(1),
            closed: AtomicBool::new(false),
            rtt_us: Mutex::new(Vec::new()),
        });
        let reader_inner = Arc::clone(&inner);
        let reader = std::thread::Builder::new()
            .name("cdp-reader".into())
            .spawn(move || read_loop(reader_inner, File::from(from_browser)))
            .expect("spawn cdp reader");
        Arc::new(Connection { inner, reader: Mutex::new(Some(reader)) })
    }

    pub fn is_closed(&self) -> bool {
        self.inner.closed.load(Ordering::Acquire)
    }

    pub fn send(&self, session: Option<&str>, method: &str, params: Value, timeout: Duration) -> Result<Value, CdpError> {
        if self.is_closed() {
            return Err(CdpError::Closed("connection closed".into()));
        }
        let id = self.inner.next_id.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = mpsc::channel();
        self.inner.pending.lock().unwrap().insert(id, (method.to_string(), tx));
        let mut msg = json!({"id": id, "method": method, "params": params});
        if let Some(s) = session {
            msg["sessionId"] = Value::String(s.to_string());
        }
        let started = Instant::now();
        {
            let mut w = self.inner.writer.lock().unwrap();
            let mut bytes = serde_json::to_vec(&msg).expect("serialize");
            bytes.push(0);
            if let Err(e) = w.write_all(&bytes).and_then(|_| w.flush()) {
                self.inner.pending.lock().unwrap().remove(&id);
                return Err(CdpError::Closed(format!("write failed: {e}")));
            }
        }
        match rx.recv_timeout(timeout) {
            Ok(result) => {
                self.inner.rtt_us.lock().unwrap().push(started.elapsed().as_micros() as u64);
                result
            }
            Err(RecvTimeoutError::Timeout) => {
                self.inner.pending.lock().unwrap().remove(&id);
                Err(CdpError::Timeout(method.to_string()))
            }
            Err(RecvTimeoutError::Disconnected) => Err(CdpError::Closed("reader stopped".into())),
        }
    }

    /// Subscribes to events of `method` on `session` (None = browser session).
    pub fn subscribe(&self, session: Option<&str>, method: &str) -> EventStream {
        let (tx, rx) = mpsc::channel();
        let id = self.inner.next_sub.fetch_add(1, Ordering::Relaxed);
        self.inner.subs.lock().unwrap().push(Subscription {
            id,
            session: session.map(str::to_string),
            method: method.to_string(),
            tx,
        });
        EventStream { inner: Arc::clone(&self.inner), id, rx }
    }

    pub fn rtt_samples_us(&self) -> Vec<u64> {
        self.inner.rtt_us.lock().unwrap().clone()
    }

    pub fn close(&self) {
        self.inner.closed.store(true, Ordering::Release);
        // Closing the writer makes the browser exit (pipe mode), which closes
        // the read end and ends the reader thread.
        if let Ok(w) = self.inner.writer.lock() {
            let _ = w.sync_all();
        }
        fail_all(&self.inner, CdpError::Closed("client closed".into()));
    }

    pub fn join_reader(&self) {
        if let Some(h) = self.reader.lock().unwrap().take() {
            let _ = h.join();
        }
    }
}

fn fail_all(inner: &Inner, err: CdpError) {
    let pending: Vec<_> = inner.pending.lock().unwrap().drain().collect();
    for (_, (_, tx)) in pending {
        let _ = tx.send(Err(err.clone()));
    }
    inner.subs.lock().unwrap().clear();
}

fn read_loop(inner: Arc<Inner>, file: File) {
    let mut reader = BufReader::with_capacity(1 << 20, file);
    let mut buf = Vec::with_capacity(1 << 16);
    loop {
        buf.clear();
        match reader.read_until(0, &mut buf) {
            Ok(0) => {
                inner.closed.store(true, Ordering::Release);
                fail_all(&inner, CdpError::Closed("EOF".into()));
                return;
            }
            Ok(_) => {}
            Err(e) => {
                inner.closed.store(true, Ordering::Release);
                fail_all(&inner, CdpError::Closed(format!("read error: {e}")));
                return;
            }
        }
        if buf.last() == Some(&0) {
            buf.pop();
        }
        let msg: Value = match serde_json::from_slice(&buf) {
            Ok(v) => v,
            Err(_) => continue,
        };
        dispatch(&inner, msg);
    }
}

fn dispatch(inner: &Inner, mut msg: Value) {
    if let Some(id) = msg.get("id").and_then(Value::as_u64) {
        let pending = inner.pending.lock().unwrap().remove(&id);
        if let Some((method, tx)) = pending {
            let result = if let Some(err) = msg.get("error") {
                Err(CdpError::Protocol {
                    method,
                    code: err.get("code").and_then(Value::as_i64).unwrap_or(0),
                    message: err.get("message").and_then(Value::as_str).unwrap_or("").to_string(),
                    data: err.get("data").cloned(),
                })
            } else {
                Ok(msg.get_mut("result").map(Value::take).unwrap_or_else(|| json!({})))
            };
            let _ = tx.send(result);
        }
        return;
    }
    let method = match msg.get("method").and_then(Value::as_str) {
        Some(m) => m.to_string(),
        None => return,
    };
    let session = msg.get("sessionId").and_then(Value::as_str).map(str::to_string);
    let params = msg.get_mut("params").map(Value::take).unwrap_or_else(|| json!({}));
    let mut subs = inner.subs.lock().unwrap();
    subs.retain(|s| {
        if s.method != method || (s.session.is_some() && s.session != session) {
            return true;
        }
        s.tx.send(params.clone()).is_ok()
    });
}

/// Receiver of one event type; unsubscribes when dropped.
pub struct EventStream {
    inner: Arc<Inner>,
    id: u64,
    rx: Receiver<Value>,
}

impl EventStream {
    pub fn recv_timeout(&self, timeout: Duration) -> Option<Value> {
        self.rx.recv_timeout(timeout).ok()
    }

    pub fn try_recv(&self) -> Option<Value> {
        self.rx.try_recv().ok()
    }

    /// Waits for an event matching `pred`, discarding others.
    pub fn wait_for(&self, timeout: Duration, mut pred: impl FnMut(&Value) -> bool) -> Option<Value> {
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return None;
            }
            match self.rx.recv_timeout(remaining) {
                Ok(v) if pred(&v) => return Some(v),
                Ok(_) => continue,
                Err(_) => return None,
            }
        }
    }
}

impl Drop for EventStream {
    fn drop(&mut self) {
        self.inner.subs.lock().unwrap().retain(|s| s.id != self.id);
    }
}

/// A flattened target session, or the browser session when `id` is None.
#[derive(Clone)]
pub struct Session {
    pub conn: Arc<Connection>,
    pub id: Option<String>,
    pub default_timeout: Duration,
}

impl Session {
    pub fn send(&self, method: &str, params: Value) -> Result<Value, CdpError> {
        self.conn.send(self.id.as_deref(), method, params, self.default_timeout)
    }

    pub fn send_timeout(&self, method: &str, params: Value, timeout: Duration) -> Result<Value, CdpError> {
        self.conn.send(self.id.as_deref(), method, params, timeout)
    }

    pub fn subscribe(&self, method: &str) -> EventStream {
        self.conn.subscribe(self.id.as_deref(), method)
    }

    /// `Runtime.evaluate` in the main world, awaiting promises, by value.
    pub fn evaluate(&self, expression: &str) -> Result<Value, String> {
        let r = self
            .send("Runtime.evaluate", json!({"expression": expression, "awaitPromise": true, "returnByValue": true}))
            .map_err(|e| e.to_string())?;
        if let Some(details) = r.get("exceptionDetails") {
            return Err(format!(
                "evaluate failed: {} {}",
                details.get("text").and_then(Value::as_str).unwrap_or(""),
                details.pointer("/exception/description").and_then(Value::as_str).unwrap_or("")
            ));
        }
        Ok(r.pointer("/result/value").cloned().unwrap_or(Value::Null))
    }
}

/// Creates a pipe pair with close-on-exec set on both ends.
pub fn pipe2_cloexec() -> std::io::Result<(OwnedFd, OwnedFd)> {
    let mut fds = [0i32; 2];
    let rc = unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC) };
    if rc != 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(unsafe { (OwnedFd::from_raw_fd(fds[0]), OwnedFd::from_raw_fd(fds[1])) })
}
