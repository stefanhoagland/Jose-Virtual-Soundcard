#include "jvs/net.h"
#include "jvs/clock.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <map>

#if defined(__APPLE__)
#include <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif

namespace jvs {

std::vector<NetInterface> list_interfaces() {
    std::map<std::string, NetInterface> found;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return {};
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        auto& ni = found[p->ifa_name];
        ni.name = p->ifa_name;
        if (p->ifa_addr->sa_family == AF_INET) {
            if (ni.ipv4 == 0) ni.ipv4 = reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr;
        }
#if defined(__APPLE__)
        else if (p->ifa_addr->sa_family == AF_LINK) {
            auto* sdl = reinterpret_cast<sockaddr_dl*>(p->ifa_addr);
            if (sdl->sdl_alen == 6) {
                std::memcpy(ni.mac.data(), LLADDR(sdl), 6);
                ni.has_mac = true;
            }
        }
#else
        else if (p->ifa_addr->sa_family == AF_PACKET) {
            auto* sll = reinterpret_cast<sockaddr_ll*>(p->ifa_addr);
            if (sll->sll_halen == 6) {
                std::memcpy(ni.mac.data(), sll->sll_addr, 6);
                ni.has_mac = true;
            }
        }
#endif
    }
    freeifaddrs(ifa);
    std::vector<NetInterface> out;
    for (auto& [name, ni] : found)
        if (ni.ipv4 != 0) out.push_back(ni);
    return out;
}

bool find_interface(const std::string& name, NetInterface& out) {
    for (auto& ni : list_interfaces()) {
        if (ni.name == name) {
            out = ni;
            return true;
        }
    }
    return false;
}

std::string ipv4_to_string(uint32_t addr_be) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a;
    a.s_addr = addr_be;
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

bool parse_ipv4(const std::string& s, uint32_t& addr_be) {
    in_addr a;
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    addr_be = a.s_addr;
    return true;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool UdpSocket::open(uint16_t port, uint32_t bind_be) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
#ifdef IP_MULTICAST_ALL
    // Linux otherwise delivers every joined group on this port to every
    // socket bound to it. macOS already filters per socket.
    int zero = 0;
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero);
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = bind_be ? bind_be : htonl(INADDR_ANY);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        close();
        return false;
    }
    return true;
}

bool UdpSocket::join(uint32_t group_be, uint32_t iface_be) {
    ip_mreq m{};
    m.imr_multiaddr.s_addr = group_be;
    m.imr_interface.s_addr = iface_be;
    return setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0;
}

bool UdpSocket::set_multicast_out(uint32_t iface_be, int ttl, bool loop) {
    in_addr a;
    a.s_addr = iface_be;
    bool ok = setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a) == 0;
    unsigned char t = static_cast<unsigned char>(ttl);
    ok &= setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof t) == 0;
    unsigned char l = loop ? 1 : 0;
    ok &= setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &l, sizeof l) == 0;
    return ok;
}

bool UdpSocket::enable_rx_timestamps() {
    int one = 1;
    return setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof one) == 0;
}

void UdpSocket::set_recv_timeout_ms(int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

void UdpSocket::set_buffer_sizes(int bytes) {
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
}

void UdpSocket::set_dscp(int dscp) {
    int tos = dscp << 2;
    setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
}

int UdpSocket::recv(uint8_t* buf, int len, int64_t* rx_mono_ns, uint32_t* src_be) {
    sockaddr_in src{};
    iovec iov{buf, size_t(len)};
    alignas(cmsghdr) char ctrl[256];
    msghdr msg{};
    msg.msg_name = &src;
    msg.msg_namelen = sizeof src;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    ssize_t n = ::recvmsg(fd_, &msg, 0);
    int64_t fallback = mono_ns();
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    if (rx_mono_ns) {
        *rx_mono_ns = fallback;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP) {
                timeval tv;
                std::memcpy(&tv, CMSG_DATA(c), sizeof tv);
                int64_t rt = int64_t(tv.tv_sec) * 1000000000LL + int64_t(tv.tv_usec) * 1000;
                int64_t m = realtime_to_mono(rt);
                // Guard against a wall clock step between packet and now.
                if (m <= fallback && fallback - m < 50000000) *rx_mono_ns = m;
            }
        }
    }
    if (src_be) *src_be = src.sin_addr.s_addr;
    return int(n);
}

bool UdpSocket::send_to(const uint8_t* buf, int len, uint32_t dst_be, uint16_t port) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = dst_be;
    return ::sendto(fd_, buf, size_t(len), 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst) == len;
}

}  // namespace jvs
