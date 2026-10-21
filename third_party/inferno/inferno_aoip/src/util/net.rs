use log::{error, info};

use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use tokio::{net::UdpSocket, select, sync::broadcast::Receiver};

pub const MTU: usize = 1500;
const PACKET_BUFFER_SIZE: usize = MTU;
pub const MAX_PAYLOAD_BYTES: usize = 1400; // ???

pub struct ReceiveBuffer {
  buff: [u8; PACKET_BUFFER_SIZE],
}

impl ReceiveBuffer {
  pub fn new() -> Self {
    Self { buff: [0u8; PACKET_BUFFER_SIZE] }
  }
}

pub struct UdpSocketWrapper {
  socket: Option<UdpSocket>,
  listen_addr: Ipv4Addr,
  listen_port: u16,
  shutdown: Receiver<()>,
  dowork: bool,
  recv_buff: [u8; PACKET_BUFFER_SIZE],
}

impl UdpSocketWrapper {
  pub async fn new(
    listen_addr: Option<Ipv4Addr>,
    listen_port: u16,
    shutdown: Receiver<()>,
  ) -> UdpSocketWrapper {
    let listen_addr = listen_addr.unwrap_or(Ipv4Addr::new(0, 0, 0, 0));
    let socket_opt =
      UdpSocket::bind(SocketAddr::new(std::net::IpAddr::V4(listen_addr), listen_port)).await;
    let socket = socket_opt.expect("error starting really needed listener");
    if !listen_addr.is_unspecified() {
      set_multicast_interface(&socket, listen_addr);
    }
    // TODO MAY PANIC: this error should be non-fatal because some apps may use Inferno as an optional audio I/O
    let listen_port = socket.local_addr().unwrap().port();
    UdpSocketWrapper {
      listen_addr,
      socket: Some(socket),
      listen_port,
      shutdown,
      dowork: true,
      recv_buff: [0; PACKET_BUFFER_SIZE],
    }
  }

  pub fn should_work(&self) -> bool {
    self.dowork
  }

  pub fn port(&self) -> u16 {
    self.listen_port
  }

  pub async fn recv<'a>(&mut self, recv_buff: &'a mut ReceiveBuffer) -> Option<(SocketAddr, &'a [u8])> {
    let socket = match &self.socket {
      Some(s) => s,
      None => {
        return None;
      }
    };
    select! {
      r = socket.recv_from(&mut recv_buff.buff) => {
        match r {
          Ok((len_recv, src)) => {
            crate::jvs_stats::record_rx(self.listen_port, src);
            return Some((src, &recv_buff.buff[..len_recv]));
          },
          Err(e) => {
            error!("error receiving from socket: {e:?}");
            return None;
          }
        }
      },
      _ = self.shutdown.recv() => {
        self.dowork = false;
        return None;
      }
    };
  }

  pub async fn send(&self, dst: &SocketAddr, packet: &[u8]) {
    let socket = match &self.socket {
      Some(s) => s,
      None => {
        info!("shutting down, discarding message to send");
        return;
      }
    };
    match socket.send_to(packet, dst).await {
      Ok(_) => crate::jvs_stats::record_tx(self.listen_port, None),
      Err(e) => {
        error!("send error (ignoring): {e:?}");
        crate::jvs_stats::record_tx(self.listen_port, Some(e.to_string()));
      }
    }
  }
}

pub async fn create_tokio_udp_socket(self_ip: Ipv4Addr) -> tokio::io::Result<(UdpSocket, u16)> {
  let socket = UdpSocket::bind(SocketAddr::new(IpAddr::V4(self_ip), 0)).await?;
  set_multicast_interface(&socket, self_ip);
  let port = socket.local_addr()?.port();
  return Ok((socket, port));
}

pub fn create_mio_udp_socket(self_ip: Ipv4Addr) -> std::io::Result<(mio::net::UdpSocket, u16)> {
  let socket = mio::net::UdpSocket::bind(SocketAddr::new(IpAddr::V4(self_ip), 0))?;
  set_multicast_interface(&socket, self_ip);
  let port = socket.local_addr()?.port();
  return Ok((socket, port));
}

/// Jose Virtual Soundcard addition: send multicast out of the interface that
/// owns `ip`. On macOS, binding a socket to an address doesn't choose the
/// interface for multicast; without this, multicast (device info, heartbeats,
/// multicast flows) leaves through the default-route interface, which on a
/// Mac with Wi-Fi plus a wired Dante port is the wrong one.
#[cfg(unix)]
pub fn set_multicast_interface<S: std::os::fd::AsFd>(socket: &S, ip: Ipv4Addr) {
  if let Err(e) = socket2::SockRef::from(socket).set_multicast_if_v4(&ip) {
    log::warn!("couldn't set multicast interface to {ip}: {e:?}");
  }
}
