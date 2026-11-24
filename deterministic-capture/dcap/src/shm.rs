//! Shared-memory frame ring between the engine (producer) and the encoder
//! (consumer). The layout mirrors `headless/lib/browser/capture/shm_ring.h`
//! in the browser tree; both sides must change together (bump `VERSION`).
//!
//! The controller creates the ring (memfd), writes the header and hands the
//! mapping plus two eventfds to the browser at launch. The engine copies each
//! canonical BGRA frame into the next free slot, publishes it by advancing
//! `produced` and signals `ready`; the consumer encodes in place, advances
//! `consumed` and signals `free`.

use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::time::{Duration, Instant};

pub const MAGIC: u64 = 0x3147_4E52_5041_4344; // "DCAPRNG1"
pub const VERSION: u32 = 1;
pub const HEADER_BYTES: usize = 4096;
pub const SLOT_HEADER_BYTES: usize = 256;

const OFF_MAGIC: usize = 0;
const OFF_VERSION: usize = 8;
const OFF_SLOT_COUNT: usize = 12;
const OFF_SLOT_BYTES: usize = 16;
const OFF_WIDTH: usize = 24;
const OFF_HEIGHT: usize = 28;
const OFF_STRIDE: usize = 32;
const OFF_PIXEL_FORMAT: usize = 36;
const OFF_AUDIO_RATE: usize = 40;
const OFF_AUDIO_CHANNELS: usize = 44;
const OFF_AUDIO_FORMAT: usize = 48;
const OFF_AUDIO_FRAMES_PER_SLOT: usize = 52;
const OFF_VIDEO_BYTES: usize = 56;
const OFF_AUDIO_BYTES: usize = 64;
const OFF_PRODUCED: usize = 128;
const OFF_CONSUMED: usize = 192;
const OFF_CLOSED: usize = 256;
const OFF_PRODUCER_ERROR: usize = 260;

const SLOT_FRAME_INDEX: usize = 0;
const SLOT_VIRTUAL_TIME_US: usize = 8;
const SLOT_FLAGS: usize = 16;
const SLOT_AUDIO_FRAMES: usize = 20;
const SLOT_VIDEO_BYTES: usize = 24;
const SLOT_HASH: usize = 32;
const SLOT_HASH_PRESENT: usize = 64;
const SLOT_VIDEO_DATA: usize = SLOT_HEADER_BYTES;

pub const FLAG_VIDEO: u32 = 1;
pub const FLAG_AUDIO: u32 = 2;
pub const PIXEL_FORMAT_BGRA8_PREMUL: u32 = 1;
pub const AUDIO_FORMAT_NONE: u32 = 0;
pub const AUDIO_FORMAT_S16: u32 = 1;
pub const AUDIO_FORMAT_F32: u32 = 2;

#[derive(Debug, Clone, Copy)]
pub struct RingGeometry {
    pub width: u32,
    pub height: u32,
    pub slots: u32,
    pub audio_rate: u32,
    pub audio_channels: u32,
    pub audio_format: u32,
    pub audio_frames_per_slot: u32,
}

impl RingGeometry {
    pub fn video_bytes(&self) -> u64 {
        self.width as u64 * self.height as u64 * 4
    }
    pub fn audio_bytes(&self) -> u64 {
        let bps = match self.audio_format {
            AUDIO_FORMAT_S16 => 2,
            AUDIO_FORMAT_F32 => 4,
            _ => return 0,
        };
        self.audio_frames_per_slot as u64 * self.audio_channels as u64 * bps
    }
    pub fn slot_bytes(&self) -> u64 {
        let raw = SLOT_HEADER_BYTES as u64 + self.video_bytes() + self.audio_bytes();
        (raw + 4095) / 4096 * 4096
    }
    pub fn total_bytes(&self) -> u64 {
        HEADER_BYTES as u64 + self.slots as u64 * self.slot_bytes()
    }
}

struct Mapping {
    base: *mut u8,
    len: usize,
}

unsafe impl Send for Mapping {}
unsafe impl Sync for Mapping {}

impl Drop for Mapping {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.base as *mut libc::c_void, self.len);
        }
    }
}

impl Mapping {
    fn read<T: Copy>(&self, off: usize) -> T {
        debug_assert!(off + std::mem::size_of::<T>() <= self.len);
        unsafe { std::ptr::read_unaligned(self.base.add(off) as *const T) }
    }
    fn write<T: Copy>(&self, off: usize, v: T) {
        debug_assert!(off + std::mem::size_of::<T>() <= self.len);
        unsafe { std::ptr::write_unaligned(self.base.add(off) as *mut T, v) }
    }
    fn atomic_u64(&self, off: usize) -> &AtomicU64 {
        unsafe { AtomicU64::from_ptr(self.base.add(off) as *mut u64) }
    }
    fn atomic_u32(&self, off: usize) -> &AtomicU32 {
        unsafe { AtomicU32::from_ptr(self.base.add(off) as *mut u32) }
    }
    fn slice(&self, off: usize, len: usize) -> &[u8] {
        assert!(off + len <= self.len);
        unsafe { std::slice::from_raw_parts(self.base.add(off), len) }
    }
    fn slice_mut(&self, off: usize, len: usize) -> &mut [u8] {
        assert!(off + len <= self.len);
        unsafe { std::slice::from_raw_parts_mut(self.base.add(off), len) }
    }
}

fn eventfd() -> std::io::Result<OwnedFd> {
    let fd = unsafe { libc::eventfd(0, libc::EFD_CLOEXEC) };
    if fd < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

fn signal(fd: &OwnedFd) {
    let one: u64 = 1;
    unsafe {
        libc::write(fd.as_raw_fd(), &one as *const u64 as *const libc::c_void, 8);
    }
}

/// Waits until the eventfd is readable (consuming the counter) or the
/// deadline passes.
fn wait(fd: &OwnedFd, deadline: Instant) -> bool {
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return false;
        }
        let mut pfd = libc::pollfd { fd: fd.as_raw_fd(), events: libc::POLLIN, revents: 0 };
        let rc = unsafe { libc::poll(&mut pfd, 1, remaining.as_millis().min(i32::MAX as u128) as i32 + 1) };
        if rc < 0 {
            if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return false;
        }
        if rc == 0 {
            continue;
        }
        let mut count: u64 = 0;
        let n = unsafe { libc::read(fd.as_raw_fd(), &mut count as *mut u64 as *mut libc::c_void, 8) };
        if n == 8 {
            return true;
        }
    }
}

fn map_fd(fd: &OwnedFd, len: usize, create: bool) -> std::io::Result<Mapping> {
    if create {
        let rc = unsafe { libc::ftruncate(fd.as_raw_fd(), len as libc::off_t) };
        if rc != 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    let base = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            len,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            fd.as_raw_fd(),
            0,
        )
    };
    if base == libc::MAP_FAILED {
        return Err(std::io::Error::last_os_error());
    }
    Ok(Mapping { base: base as *mut u8, len })
}

/// One published slot, borrowed from the ring until `release`.
pub struct SlotView<'a> {
    map: &'a Mapping,
    off: usize,
    geometry: RingGeometry,
}

impl<'a> SlotView<'a> {
    pub fn frame_index(&self) -> i64 {
        self.map.read::<i64>(self.off + SLOT_FRAME_INDEX)
    }
    pub fn virtual_time_us(&self) -> i64 {
        self.map.read::<i64>(self.off + SLOT_VIRTUAL_TIME_US)
    }
    pub fn flags(&self) -> u32 {
        self.map.read::<u32>(self.off + SLOT_FLAGS)
    }
    pub fn audio_sample_frames(&self) -> u32 {
        self.map.read::<u32>(self.off + SLOT_AUDIO_FRAMES)
    }
    pub fn hash(&self) -> Option<[u8; 32]> {
        if self.map.read::<u32>(self.off + SLOT_HASH_PRESENT) == 0 {
            return None;
        }
        let mut h = [0u8; 32];
        h.copy_from_slice(self.map.slice(self.off + SLOT_HASH, 32));
        Some(h)
    }
    pub fn video(&self) -> &[u8] {
        let n = self.map.read::<u64>(self.off + SLOT_VIDEO_BYTES) as usize;
        self.map.slice(self.off + SLOT_VIDEO_DATA, n.min(self.geometry.video_bytes() as usize))
    }
    pub fn audio(&self) -> &[u8] {
        let frames = self.audio_sample_frames() as u64;
        let bps = match self.geometry.audio_format {
            AUDIO_FORMAT_S16 => 2,
            AUDIO_FORMAT_F32 => 4,
            _ => 0,
        };
        let n = (frames * self.geometry.audio_channels as u64 * bps).min(self.geometry.audio_bytes()) as usize;
        self.map.slice(self.off + SLOT_VIDEO_DATA + self.geometry.video_bytes() as usize, n)
    }
}

/// The controller side: owns the memfd and the eventfds, consumes slots.
pub struct Ring {
    map: Mapping,
    pub geometry: RingGeometry,
    ring_fd: OwnedFd,
    ready_fd: OwnedFd,
    free_fd: OwnedFd,
    cancelled: AtomicBool,
}

#[derive(Debug)]
pub enum RingEvent<'a> {
    Frame(SlotView<'a>),
    /// Producer closed the stream; `aborted` when it marked an error.
    Closed { aborted: bool },
    Timeout,
}

impl std::fmt::Debug for SlotView<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Slot(frame {}, vt {} us)", self.frame_index(), self.virtual_time_us())
    }
}

impl Ring {
    pub fn create(geometry: RingGeometry) -> std::io::Result<Ring> {
        let name = std::ffi::CString::new("dcap-ring").unwrap();
        let fd = unsafe { libc::memfd_create(name.as_ptr(), libc::MFD_CLOEXEC) };
        if fd < 0 {
            return Err(std::io::Error::last_os_error());
        }
        let ring_fd = unsafe { OwnedFd::from_raw_fd(fd) };
        let total = geometry.total_bytes() as usize;
        let map = map_fd(&ring_fd, total, true)?;
        map.write::<u64>(OFF_MAGIC, MAGIC);
        map.write::<u32>(OFF_VERSION, VERSION);
        map.write::<u32>(OFF_SLOT_COUNT, geometry.slots);
        map.write::<u64>(OFF_SLOT_BYTES, geometry.slot_bytes());
        map.write::<u32>(OFF_WIDTH, geometry.width);
        map.write::<u32>(OFF_HEIGHT, geometry.height);
        map.write::<u32>(OFF_STRIDE, geometry.width * 4);
        map.write::<u32>(OFF_PIXEL_FORMAT, PIXEL_FORMAT_BGRA8_PREMUL);
        map.write::<u32>(OFF_AUDIO_RATE, geometry.audio_rate);
        map.write::<u32>(OFF_AUDIO_CHANNELS, geometry.audio_channels);
        map.write::<u32>(OFF_AUDIO_FORMAT, geometry.audio_format);
        map.write::<u32>(OFF_AUDIO_FRAMES_PER_SLOT, geometry.audio_frames_per_slot);
        map.write::<u64>(OFF_VIDEO_BYTES, geometry.video_bytes());
        map.write::<u64>(OFF_AUDIO_BYTES, geometry.audio_bytes());
        map.atomic_u64(OFF_PRODUCED).store(0, Ordering::Release);
        map.atomic_u64(OFF_CONSUMED).store(0, Ordering::Release);
        map.atomic_u32(OFF_CLOSED).store(0, Ordering::Release);
        map.atomic_u32(OFF_PRODUCER_ERROR).store(0, Ordering::Release);
        Ok(Ring { map, geometry, ring_fd, ready_fd: eventfd()?, free_fd: eventfd()?, cancelled: AtomicBool::new(false) })
    }

    /// Duplicates the three descriptors for the child (the ring keeps its own).
    pub fn child_fds(&self) -> std::io::Result<(OwnedFd, OwnedFd, OwnedFd)> {
        Ok((self.ring_fd.try_clone()?, self.ready_fd.try_clone()?, self.free_fd.try_clone()?))
    }

    pub fn produced(&self) -> u64 {
        self.map.atomic_u64(OFF_PRODUCED).load(Ordering::Acquire)
    }
    pub fn consumed(&self) -> u64 {
        self.map.atomic_u64(OFF_CONSUMED).load(Ordering::Acquire)
    }
    pub fn closed(&self) -> bool {
        self.map.atomic_u32(OFF_CLOSED).load(Ordering::Acquire) != 0
    }
    pub fn producer_error(&self) -> bool {
        self.map.atomic_u32(OFF_PRODUCER_ERROR).load(Ordering::Acquire) != 0
    }

    /// Blocks until a slot is published, the producer closes, or `timeout`.
    pub fn next(&self, timeout: Duration) -> RingEvent<'_> {
        let deadline = Instant::now() + timeout;
        loop {
            let consumed = self.consumed();
            if self.produced() > consumed {
                let off = HEADER_BYTES + (consumed % self.geometry.slots as u64) as usize * self.geometry.slot_bytes() as usize;
                return RingEvent::Frame(SlotView { map: &self.map, off, geometry: self.geometry });
            }
            if self.closed() {
                return RingEvent::Closed { aborted: self.producer_error() };
            }
            if self.cancelled.load(Ordering::Acquire) {
                return RingEvent::Closed { aborted: true };
            }
            if !wait(&self.ready_fd, deadline) {
                // Re-check once: the producer may have published right at the deadline.
                if self.produced() > consumed || self.closed() {
                    continue;
                }
                return RingEvent::Timeout;
            }
        }
    }

    /// Consumer-side cancellation (the producer died without closing): makes
    /// `next` return `Closed { aborted: true }`.
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
        signal(&self.ready_fd);
    }

    /// Releases the oldest published slot back to the producer.
    pub fn release(&self) {
        let consumed = self.consumed();
        self.map.atomic_u64(OFF_CONSUMED).store(consumed + 1, Ordering::Release);
        signal(&self.free_fd);
    }
}

/// The producer side, used by the fake engine in tests (the real producer is
/// the browser's `RawFrameSink`).
pub struct Producer {
    map: Mapping,
    pub geometry: RingGeometry,
    ready_fd: OwnedFd,
    free_fd: OwnedFd,
}

impl Producer {
    pub fn open(ring_fd: OwnedFd, ready_fd: OwnedFd, free_fd: OwnedFd) -> std::io::Result<Producer> {
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(ring_fd.as_raw_fd(), &mut st) } != 0 {
            return Err(std::io::Error::last_os_error());
        }
        let map = map_fd(&ring_fd, st.st_size as usize, false)?;
        if map.read::<u64>(OFF_MAGIC) != MAGIC || map.read::<u32>(OFF_VERSION) != VERSION {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidData, "ring magic/version mismatch"));
        }
        let geometry = RingGeometry {
            width: map.read(OFF_WIDTH),
            height: map.read(OFF_HEIGHT),
            slots: map.read(OFF_SLOT_COUNT),
            audio_rate: map.read(OFF_AUDIO_RATE),
            audio_channels: map.read(OFF_AUDIO_CHANNELS),
            audio_format: map.read(OFF_AUDIO_FORMAT),
            audio_frames_per_slot: map.read(OFF_AUDIO_FRAMES_PER_SLOT),
        };
        Ok(Producer { map, geometry, ready_fd, free_fd })
    }

    /// Publishes one frame; blocks while the ring is full, up to `timeout`.
    pub fn publish(&self, frame_index: i64, virtual_time_us: i64, bgra: &[u8], hash: Option<&[u8; 32]>, audio: &[u8], audio_frames: u32, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        let produced = self.map.atomic_u64(OFF_PRODUCED).load(Ordering::Acquire);
        loop {
            let consumed = self.map.atomic_u64(OFF_CONSUMED).load(Ordering::Acquire);
            if produced - consumed < self.geometry.slots as u64 {
                break;
            }
            if !wait(&self.free_fd, deadline) {
                return false;
            }
        }
        let off = HEADER_BYTES + (produced % self.geometry.slots as u64) as usize * self.geometry.slot_bytes() as usize;
        self.map.write::<i64>(off + SLOT_FRAME_INDEX, frame_index);
        self.map.write::<i64>(off + SLOT_VIRTUAL_TIME_US, virtual_time_us);
        let mut flags = 0;
        if !bgra.is_empty() {
            flags |= FLAG_VIDEO;
        }
        if audio_frames > 0 {
            flags |= FLAG_AUDIO;
        }
        self.map.write::<u32>(off + SLOT_FLAGS, flags);
        self.map.write::<u32>(off + SLOT_AUDIO_FRAMES, audio_frames);
        self.map.write::<u64>(off + SLOT_VIDEO_BYTES, bgra.len() as u64);
        match hash {
            Some(h) => {
                self.map.slice_mut(off + SLOT_HASH, 32).copy_from_slice(h);
                self.map.write::<u32>(off + SLOT_HASH_PRESENT, 1);
            }
            None => self.map.write::<u32>(off + SLOT_HASH_PRESENT, 0),
        }
        self.map.slice_mut(off + SLOT_VIDEO_DATA, bgra.len()).copy_from_slice(bgra);
        if !audio.is_empty() {
            self.map.slice_mut(off + SLOT_VIDEO_DATA + self.geometry.video_bytes() as usize, audio.len()).copy_from_slice(audio);
        }
        self.map.atomic_u64(OFF_PRODUCED).store(produced + 1, Ordering::Release);
        signal(&self.ready_fd);
        true
    }

    pub fn close(&self, aborted: bool) {
        if aborted {
            self.map.atomic_u32(OFF_PRODUCER_ERROR).store(1, Ordering::Release);
        }
        self.map.atomic_u32(OFF_CLOSED).store(1, Ordering::Release);
        signal(&self.ready_fd);
    }
}
