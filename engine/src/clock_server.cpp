#include "jvs/clock_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <vector>

#include "jvs/clock.h"
#include "../../dante/jvs_dante.h"

namespace jvs {

namespace {
constexpr int kOverlayBytes = 40;
constexpr uint16_t kMajor = 1, kMinor = 0;
}  // namespace

ClockServer::~ClockServer() { stop(); }

void ClockServer::encode(uint8_t* b, int64_t clock_id, int64_t last_sync, int64_t shift, double freq_scale) {
    uint16_t flags = 1;
    b[0] = 'V';
    b[1] = 'C';
    std::memcpy(b + 2, &kMajor, 2);  // native byte order, like the reference
    std::memcpy(b + 4, &kMinor, 2);
    std::memcpy(b + 6, &flags, 2);
    std::memcpy(b + 8, &clock_id, 8);
    std::memcpy(b + 16, &last_sync, 8);
    std::memcpy(b + 24, &shift, 8);
    std::memcpy(b + 32, &freq_scale, 8);
}

bool ClockServer::start(const std::string& path, const PtpFollower* ptp) {
    stop();
    path_ = path;
    ptp_ = ptp;
    if (path.rfind("inproc:", 0) == 0) {
        // Same process: hand the overlay to Inferno directly, no socket.
        run_ = true;
        th_ = std::thread([this] {
            set_thread_realtime(50000, 1000);
            std::string name = path_.substr(7);
            while (run_) {
                int64_t last_sync, shift;
                double freq;
                if (ptp_ && ptp_->overlay(last_sync, shift, freq))
                    jvsd_publish_clock(name.c_str(), int64_t(CLOCK_MONOTONIC), last_sync, shift, freq);
                sleep_until_mono(mono_ns() + 50000000);  // 20 updates a second
            }
        });
        return true;
    }
    fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path) return false;
    std::strcpy(sa.sun_path, path.c_str());
    ::unlink(path.c_str());
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    timeval tv{0, 100000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void ClockServer::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        ::unlink(path_.c_str());
    }
    fd_ = -1;
}

void ClockServer::loop() {
    std::vector<sockaddr_un> clients;
    int64_t next_send = 0;
    while (run_) {
        // New clients announce themselves with an empty datagram.
        uint8_t tmp[64];
        sockaddr_un from{};
        socklen_t fl = sizeof from;
        ssize_t n = ::recvfrom(fd_, tmp, sizeof tmp, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        bool new_client = false;
        if (n >= 0 && fl > sizeof(sa_family_t)) {
            bool known = false;
            for (auto& c : clients)
                if (std::strcmp(c.sun_path, from.sun_path) == 0) known = true;
            if (!known) {
                clients.push_back(from);
                new_client = true;
            }
        }
        int64_t now = mono_ns();
        if (!new_client && now < next_send) continue;
        next_send = now + 100000000LL;  // 10 updates a second
        int64_t last_sync, shift;
        double freq;
        if (!ptp_ || !ptp_->overlay(last_sync, shift, freq)) continue;
        uint8_t msg[kOverlayBytes];
        encode(msg, int64_t(CLOCK_MONOTONIC), last_sync, shift, freq);
        for (size_t i = 0; i < clients.size();) {
            ssize_t r = ::sendto(fd_, msg, sizeof msg, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&clients[i]),
                                 sizeof(sockaddr_un));
            if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
                clients.erase(clients.begin() + long(i));  // client went away
            } else {
                ++i;
            }
        }
    }
}

}  // namespace jvs
