#pragma once
#include <atomic>
#include <thread>

#include "jvs/net.h"
#include "jvs/ptp.h"

namespace jvs {

class TestMaster {
public:
    ~TestMaster();
    bool start(const NetInterface& iface, uint8_t domain = 0, int version = 2);
    void stop();

private:
    void tx_loop();
    void rx_loop();
    int64_t tai_now() const;
    NetInterface iface_;
    uint8_t domain_ = 0;
    int version_ = 2;
    std::array<uint8_t, 6> uuid_{};
    ClockIdentity id_{};
    uint32_t group_ = 0;
    UdpSocket ev_, gen_;
    std::thread tx_, rx_;
    std::atomic<bool> run_{false};
};

}  // namespace jvs
