//! Jose Virtual Soundcard addition: counts control traffic on each listening
//! port (e.g. 4440 ARC, 8800 CMC), so the app can show whether Dante
//! Controller is reaching the device and whether replies go out.

use std::{
  collections::BTreeMap,
  net::SocketAddr,
  sync::Mutex,
};

#[derive(Clone, Debug, Default)]
pub struct PortStats {
  pub received: u64,
  pub last_from: Option<SocketAddr>,
  pub sent: u64,
  pub send_errors: u64,
  pub last_send_error: Option<String>,
}

static STATS: Mutex<BTreeMap<u16, PortStats>> = Mutex::new(BTreeMap::new());

pub fn record_rx(port: u16, from: SocketAddr) {
  let mut m = STATS.lock().unwrap();
  let e = m.entry(port).or_default();
  e.received += 1;
  e.last_from = Some(from);
}

pub fn record_tx(port: u16, error: Option<String>) {
  let mut m = STATS.lock().unwrap();
  let e = m.entry(port).or_default();
  match error {
    None => e.sent += 1,
    Some(err) => {
      e.send_errors += 1;
      e.last_send_error = Some(err);
    }
  }
}

pub fn snapshot() -> BTreeMap<u16, PortStats> {
  STATS.lock().unwrap().clone()
}

pub fn reset() {
  STATS.lock().unwrap().clear();
}

/// One line per port, e.g. "4440: 12 in (last from 10.0.0.5:51234), 12 out".
pub fn summary() -> String {
  let mut out = String::new();
  for (port, s) in snapshot() {
    out += &format!("{port}: {} in", s.received);
    if let Some(f) = s.last_from {
      out += &format!(" (last from {f})");
    }
    out += &format!(", {} out", s.sent);
    if s.send_errors > 0 {
      out += &format!(", {} send errors ({})", s.send_errors, s.last_send_error.unwrap_or_default());
    }
    out += "\n";
  }
  out
}
