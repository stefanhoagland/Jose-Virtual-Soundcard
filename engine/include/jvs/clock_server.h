// Publishes our PTP clock to Inferno over the usrvclock protocol: a Unix
// datagram socket that sends a small "overlay" (offset + rate against the
// monotonic clock) to every client that says hello. A path "inproc:<name>"
// publishes inside this process instead (no socket; works in sandboxes).
#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "jvs/ptp.h"

namespace jvs {

class ClockServer {
public:
    ~ClockServer();
    bool start(const std::string& path, const PtpFollower* ptp);
    void stop();
    const std::string& path() const { return path_; }

    // Packs one overlay message (40 bytes). Exposed for tests.
    static void encode(uint8_t* out, int64_t clock_id, int64_t last_sync, int64_t shift, double freq_scale);

private:
    void loop();
    std::string path_;
    const PtpFollower* ptp_ = nullptr;
    int fd_ = -1;
    std::thread th_;
    std::atomic<bool> run_{false};
};

}  // namespace jvs
