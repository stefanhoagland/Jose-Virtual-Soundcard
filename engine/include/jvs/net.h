// Small UDP/multicast helpers over BSD sockets (macOS and Linux).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jvs {

struct NetInterface {
    std::string name;          // e.g. "en0"
    uint32_t ipv4 = 0;         // network byte order
    std::array<uint8_t, 6> mac{};
    bool has_mac = false;
};

// All interfaces that have an IPv4 address.
std::vector<NetInterface> list_interfaces();
// Look up one interface by name. Returns false if not found.
bool find_interface(const std::string& name, NetInterface& out);

std::string ipv4_to_string(uint32_t addr_be);
bool parse_ipv4(const std::string& s, uint32_t& addr_be);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Bind to port with address reuse, so several programs can listen to the
    // same multicast port. bind_be = 0 means any address; pass a multicast
    // group to only get that group's packets.
    bool open(uint16_t port, uint32_t bind_be = 0);
    // Join a multicast group on the given interface address.
    bool join(uint32_t group_be, uint32_t iface_be);
    // Outgoing multicast goes out of this interface, with this TTL.
    bool set_multicast_out(uint32_t iface_be, int ttl = 32, bool loop = true);
    // Ask the kernel to timestamp received packets.
    bool enable_rx_timestamps();
    void set_recv_timeout_ms(int ms);
    // Larger socket buffers for many channels.
    void set_buffer_sizes(int bytes);
    // DSCP value for outgoing packets (AES67: 46 for audio/PTP event, 34 also seen).
    void set_dscp(int dscp);

    // Returns bytes received, 0 on timeout, -1 on error.
    // rx_mono_ns is the receive time in the monotonic timebase (kernel
    // timestamp when available). src_be is the sender address.
    int recv(uint8_t* buf, int len, int64_t* rx_mono_ns = nullptr, uint32_t* src_be = nullptr);
    bool send_to(const uint8_t* buf, int len, uint32_t dst_be, uint16_t port);

    int fd() const { return fd_; }
    void close();

private:
    int fd_ = -1;
};

}  // namespace jvs
