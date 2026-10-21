// Jose Virtual Soundcard - C interface to Inferno (unofficial Dante implementation).
// Copyright (C) 2026 Jose Virtual Soundcard contributors
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See the LICENSE file.
//
// One `jvsd` handle = one Dante device on the network. Audio goes through two
// interleaved ring buffers of 32-bit samples that this crate owns:
//
//   index of media time T  =  (T - start_time) mod buffer_frames
//
// RX: the sample that a Dante transmitter stamped with time T appears at the
//     index of T + rx latency, so reading at "now" gives audio from now - latency.
// TX: write samples for time T (T >= now) at the index of T; Inferno sends them.
//
// Media time is the PTP clock in samples: floor(ptp_ns * sample_rate / 1e9).
// The caller provides the clock through a usrvclock socket (see engine/).

use std::collections::BTreeMap;
use std::ffi::{c_char, c_int, CStr};
use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::thread::JoinHandle;

use inferno_aoip::device_server::{AtomicSample, Clock, DeviceServer, ExternalBufferParameters, Settings};
use tokio::sync::{mpsc, oneshot};

enum Command {
  StartAudio {
    rx: Vec<ExternalBufferParameters<i32>>,
    tx: Vec<ExternalBufferParameters<i32>>,
    rx_start: oneshot::Receiver<Clock>,
    tx_start: oneshot::Receiver<Clock>,
  },
  Subscribe { local_channel: usize, tx_channel: String, tx_device: String },
  Unsubscribe { local_channel: usize },
  Shutdown,
}

pub struct Jvsd {
  commands: mpsc::Sender<Command>,
  thread: Option<JoinHandle<()>>,
  started: Arc<Mutex<Option<Result<(), String>>>>,
  rx_channels: usize,
  tx_channels: usize,
  buffer_frames: usize,
  rx_buf: Box<[AtomicSample]>,
  tx_buf: Box<[AtomicSample]>,
  buffers_valid: Arc<RwLock<bool>>,
  rx_timestamp: Arc<AtomicUsize>,
  tx_timestamp: Arc<AtomicUsize>,
  start_time: Option<u64>,
  name: String,
}

fn cstr(p: *const c_char) -> Option<String> {
  if p.is_null() {
    return None;
  }
  let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
  if s.is_empty() {
    None
  } else {
    Some(s)
  }
}

fn make_buffer(frames: usize, channels: usize) -> Box<[AtomicSample]> {
  (0..frames * channels.max(1)).map(|_| AtomicSample::new(0)).collect::<Vec<_>>().into_boxed_slice()
}

fn params(
  buf: &[AtomicSample],
  frames: usize,
  channels: usize,
  valid: &Arc<RwLock<bool>>,
) -> Vec<ExternalBufferParameters<i32>> {
  (0..channels)
    .map(|ch| unsafe {
      // Channel ch starts at sample ch and steps by `channels` (interleaved).
      ExternalBufferParameters::new(buf.as_ptr().add(ch), frames * channels - ch, channels, valid.clone(), None)
    })
    .collect()
}

/// Configuration for jvsd_create. All strings are UTF-8, may be NULL.
#[repr(C)]
pub struct JvsdConfig {
  /// Name shown in Dante Controller (max 31 characters).
  pub name: *const c_char,
  /// IPv4 address of the network port wired to Dante.
  pub bind_ip: *const c_char,
  pub rx_channels: c_int,
  pub tx_channels: c_int,
  pub sample_rate: c_int,
  pub rx_latency_us: c_int,
  pub tx_latency_us: c_int,
  /// Path of the usrvclock socket that provides the PTP clock.
  pub clock_socket: *const c_char,
  /// Ring buffer size in frames (power of two recommended, e.g. 32768).
  pub buffer_frames: c_int,
  /// Set to a value >= 1 to run several devices on one IP address.
  pub process_id: c_int,
  /// Base UDP port for the control protocols (0 = standard Dante ports).
  pub alt_port: c_int,
}

/// Starts the Dante device (discovery, control, clock). Returns NULL on bad
/// configuration. Audio doesn't flow until jvsd_start_audio.
#[no_mangle]
pub extern "C" fn jvsd_create(cfg: *const JvsdConfig) -> *mut Jvsd {
  if cfg.is_null() {
    return std::ptr::null_mut();
  }
  let cfg = unsafe { &*cfg };
  init_logging(None);

  let ip: Option<Ipv4Addr> = cstr(cfg.bind_ip).and_then(|s| s.parse().ok());
  if ip.is_none() {
    return std::ptr::null_mut();
  }
  let rx_channels = cfg.rx_channels.clamp(0, 512) as usize;
  let tx_channels = cfg.tx_channels.clamp(0, 512) as usize;
  let buffer_frames = (cfg.buffer_frames.max(4096)) as usize;
  let name = cstr(cfg.name).unwrap_or_else(|| "Jose Virtual Soundcard".to_owned());

  let mut conf = BTreeMap::new();
  conf.insert("NAME".to_owned(), name.clone());
  conf.insert("SAMPLE_RATE".to_owned(), cfg.sample_rate.max(8000).to_string());
  conf.insert("RX_CHANNELS".to_owned(), rx_channels.to_string());
  conf.insert("TX_CHANNELS".to_owned(), tx_channels.to_string());
  conf.insert("RX_LATENCY_NS".to_owned(), (cfg.rx_latency_us.max(250) as u64 * 1000).to_string());
  conf.insert("TX_LATENCY_NS".to_owned(), (cfg.tx_latency_us.max(250) as u64 * 1000).to_string());
  if let Some(p) = cstr(cfg.clock_socket) {
    conf.insert("CLOCK_PATH".to_owned(), p);
  }
  if cfg.process_id >= 1 {
    conf.insert("PROCESS_ID".to_owned(), cfg.process_id.to_string());
  }
  if cfg.alt_port > 0 {
    conf.insert("ALT_PORT".to_owned(), cfg.alt_port.to_string());
  }
  let settings = match std::panic::catch_unwind(|| Settings::new("Jose Virtual Soundcard", "JoseVSC", ip, &conf)) {
    Ok(s) => s,
    Err(_) => return std::ptr::null_mut(),
  };

  let (commands, mut commands_rx) = mpsc::channel::<Command>(8);
  let started: Arc<Mutex<Option<Result<(), String>>>> = Default::default();
  let started2 = started.clone();
  let thread = std::thread::Builder::new()
    .name("Inferno main".to_owned())
    .spawn(move || {
      let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
      rt.block_on(async move {
        let mut server = DeviceServer::start(settings).await;
        *started2.lock().unwrap() = Some(Ok(()));
        let mut audio = false;
        while let Some(cmd) = commands_rx.recv().await {
          match cmd {
            Command::StartAudio { rx, tx, rx_start, tx_start } => {
              if audio {
                server.stop_receiver().await;
                server.stop_transmitter().await;
              }
              server.receive_to_external_buffer(rx, rx_start, Default::default(), None).await;
              server.transmit_from_external_buffer(tx, tx_start, Default::default(), None).await;
              audio = true;
            }
            Command::Subscribe { local_channel, tx_channel, tx_device } => {
              server.subscribe(local_channel, &tx_channel, &tx_device).await;
            }
            Command::Unsubscribe { local_channel } => {
              server.unsubscribe(local_channel).await;
            }
            Command::Shutdown => break,
          }
        }
        if audio {
          server.stop_receiver().await;
          server.stop_transmitter().await;
        }
        server.shutdown().await;
      });
    });
  let thread = match thread {
    Ok(t) => t,
    Err(_) => return std::ptr::null_mut(),
  };

  let valid = Arc::new(RwLock::new(false));
  Box::into_raw(Box::new(Jvsd {
    commands,
    thread: Some(thread),
    started,
    rx_channels,
    tx_channels,
    buffer_frames,
    rx_buf: make_buffer(buffer_frames, rx_channels),
    tx_buf: make_buffer(buffer_frames, tx_channels),
    buffers_valid: valid,
    rx_timestamp: Default::default(),
    tx_timestamp: Default::default(),
    start_time: None,
    name,
  }))
}

/// 1 once the device is running on the network (it waits for the clock
/// first), 0 while starting, -1 if it failed.
#[no_mangle]
pub extern "C" fn jvsd_state(h: *mut Jvsd) -> c_int {
  let Some(h) = (unsafe { h.as_ref() }) else { return -1 };
  if h.thread.as_ref().map(|t| t.is_finished()).unwrap_or(true) {
    return -1;
  }
  match &*h.started.lock().unwrap() {
    Some(Ok(())) => 1,
    Some(Err(_)) => -1,
    None => 0,
  }
}

/// Starts audio. start_time is the current media time in samples (from the
/// same PTP clock). Call once the device is running.
#[no_mangle]
pub extern "C" fn jvsd_start_audio(h: *mut Jvsd, start_time: u64) -> c_int {
  let Some(h) = (unsafe { h.as_mut() }) else { return -1 };
  *h.buffers_valid.write().unwrap() = true;
  let rx = params(&h.rx_buf, h.buffer_frames, h.rx_channels, &h.buffers_valid);
  let tx = params(&h.tx_buf, h.buffer_frames, h.tx_channels, &h.buffers_valid);
  let (rs_tx, rs_rx) = oneshot::channel();
  let (ts_tx, ts_rx) = oneshot::channel();
  if h.commands.blocking_send(Command::StartAudio { rx, tx, rx_start: rs_rx, tx_start: ts_rx }).is_err() {
    return -1;
  }
  let _ = rs_tx.send(start_time as Clock);
  let _ = ts_tx.send(start_time as Clock);
  h.start_time = Some(start_time);
  0
}

/// Interleaved RX buffer: buffer_frames * rx_channels samples (24-bit audio
/// in the top bits of an i32, i.e. full-scale is i32::MAX).
#[no_mangle]
pub extern "C" fn jvsd_rx_buffer(h: *mut Jvsd) -> *const i32 {
  match unsafe { h.as_ref() } {
    Some(h) => h.rx_buf.as_ptr() as *const i32,
    None => std::ptr::null(),
  }
}

/// Interleaved TX buffer, same layout as RX.
#[no_mangle]
pub extern "C" fn jvsd_tx_buffer(h: *mut Jvsd) -> *mut i32 {
  match unsafe { h.as_ref() } {
    Some(h) => h.tx_buf.as_ptr() as *mut i32,
    None => std::ptr::null_mut(),
  }
}

#[no_mangle]
pub extern "C" fn jvsd_buffer_frames(h: *mut Jvsd) -> c_int {
  unsafe { h.as_ref() }.map(|h| h.buffer_frames as c_int).unwrap_or(0)
}

/// The start_time passed to jvsd_start_audio, or u64::MAX if not started.
#[no_mangle]
pub extern "C" fn jvsd_start_time(h: *mut Jvsd) -> u64 {
  unsafe { h.as_ref() }.and_then(|h| h.start_time).unwrap_or(u64::MAX)
}

/// The name shown in Dante Controller (NUL-terminated, copied into out).
#[no_mangle]
pub extern "C" fn jvsd_name(h: *mut Jvsd, out: *mut c_char, len: c_int) {
  let Some(h) = (unsafe { h.as_ref() }) else { return };
  if out.is_null() || len <= 0 {
    return;
  }
  let bytes = h.name.as_bytes();
  let n = bytes.len().min(len as usize - 1);
  unsafe {
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), out as *mut u8, n);
    *out.add(n) = 0;
  }
}

/// Routes a channel of another Dante device to one of our receive channels
/// (0-based), like clicking a cell in Dante Controller's routing grid.
/// Needs audio started. Returns 0 if the request was queued.
#[no_mangle]
pub extern "C" fn jvsd_subscribe(
  h: *mut Jvsd,
  local_channel: c_int,
  tx_channel: *const c_char,
  tx_device: *const c_char,
) -> c_int {
  let Some(h) = (unsafe { h.as_ref() }) else { return -1 };
  let (Some(ch), Some(dev)) = (cstr(tx_channel), cstr(tx_device)) else { return -1 };
  if local_channel < 0 || local_channel as usize >= h.rx_channels {
    return -1;
  }
  let cmd = Command::Subscribe { local_channel: local_channel as usize, tx_channel: ch, tx_device: dev };
  if h.commands.blocking_send(cmd).is_err() {
    return -1;
  }
  0
}

#[no_mangle]
pub extern "C" fn jvsd_unsubscribe(h: *mut Jvsd, local_channel: c_int) -> c_int {
  let Some(h) = (unsafe { h.as_ref() }) else { return -1 };
  if local_channel < 0 {
    return -1;
  }
  if h.commands.blocking_send(Command::Unsubscribe { local_channel: local_channel as usize }).is_err() {
    return -1;
  }
  0
}

/// Stops the device and frees the handle.
#[no_mangle]
pub extern "C" fn jvsd_destroy(h: *mut Jvsd) {
  if h.is_null() {
    return;
  }
  let mut h = unsafe { Box::from_raw(h) };
  let _ = h.commands.blocking_send(Command::Shutdown);
  if let Some(t) = h.thread.take() {
    let _ = t.join();
  }
  *h.buffers_valid.write().unwrap() = false;
  let _ = (&h.rx_timestamp, &h.tx_timestamp);
}

static LOG_INIT: std::sync::Once = std::sync::Once::new();

fn init_logging(file: Option<std::fs::File>) {
  LOG_INIT.call_once(|| {
    let mut b = env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"));
    if let Some(f) = file {
      b.target(env_logger::Target::Pipe(Box::new(f)));
    }
    let _ = b.try_init();
  });
}

/// Sends Inferno's log to a file (appending). Call before jvsd_create; only
/// the first call in a process takes effect.
#[no_mangle]
pub extern "C" fn jvsd_set_log_file(path: *const c_char) -> c_int {
  let Some(p) = cstr(path) else { return -1 };
  match std::fs::OpenOptions::new().create(true).append(true).open(&p) {
    Ok(f) => {
      init_logging(Some(f));
      0
    }
    Err(_) => -1,
  }
}

/// Control traffic per listening port, one line each, for diagnostics.
#[no_mangle]
pub extern "C" fn jvsd_net_summary(out: *mut c_char, len: c_int) {
  if out.is_null() || len <= 0 {
    return;
  }
  let s = inferno_aoip::jvs_stats::summary();
  let bytes = s.as_bytes();
  let n = bytes.len().min(len as usize - 1);
  unsafe {
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), out as *mut u8, n);
    *out.add(n) = 0;
  }
}
