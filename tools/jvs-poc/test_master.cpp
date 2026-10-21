// A minimal PTPv2 grandmaster for testing without Dante gear.
// Never run this on a live Dante network: it would try to become the clock.
#include "test_master.h"

#include <cstdio>
#include <cstring>
#include <random>

#include "jvs/clock.h"

namespace jvs {

using namespace ptpmsg;

bool TestMaster::start(const NetInterface& iface, uint8_t domain) {
    iface_ = iface;
    domain_ = domain;
    std::random_device rd;
    for (auto& b : id_) b = uint8_t(rd());
    if (!ev_.open(319) || !gen_.open(320)) return false;
    uint32_t g;
    parse_ipv4("224.0.1.129", g);
    group_ = g;
    ev_.join(g, iface.ipv4);
    ev_.set_multicast_out(iface.ipv4, 1, true);
    gen_.set_multicast_out(iface.ipv4, 1, true);
    ev_.set_recv_timeout_ms(20);
    run_ = true;
    tx_ = std::thread([this] { tx_loop(); });
    rx_ = std::thread([this] { rx_loop(); });
    return true;
}

void TestMaster::stop() {
    run_ = false;
    if (tx_.joinable()) tx_.join();
    if (rx_.joinable()) rx_.join();
}

TestMaster::~TestMaster() { stop(); }

int64_t TestMaster::tai_now() const { return realtime_ns() + 37LL * 1000000000LL; }

void TestMaster::tx_loop() {
    uint16_t sync_seq = 0, ann_seq = 0;
    int64_t next_sync = mono_ns(), next_ann = mono_ns();
    while (run_) {
        int64_t now = mono_ns();
        if (now >= next_ann) {
            uint8_t b[64];
            Header h;
            h.type = Announce;
            h.length = 64;
            h.domain = domain_;
            h.src_clock = id_;
            h.src_port = 1;
            h.seq = ann_seq++;
            h.control = 5;
            h.log_interval = 0;
            write_header(b, h);
            AnnounceBody a;
            a.priority1 = 250;  // lose to any real clock
            a.clock_class = 248;
            a.gm_identity = id_;
            write_announce(b, a);
            write_timestamp(b + 34, Timestamp::from_ns(tai_now()));
            gen_.send_to(b, 64, group_, 320);
            next_ann += 1000000000LL;
        }
        if (now >= next_sync) {
            uint8_t b[44];
            Header h;
            h.type = Sync;
            h.length = 44;
            h.domain = domain_;
            h.flags = 0x0200;  // two-step
            h.src_clock = id_;
            h.src_port = 1;
            h.seq = sync_seq;
            h.control = 0;
            h.log_interval = -3;
            write_header(b, h);
            write_timestamp(b + 34, Timestamp{});
            int64_t t1 = tai_now();
            ev_.send_to(b, 44, group_, 319);
            h.type = FollowUp;
            h.flags = 0;
            h.control = 2;
            write_header(b, h);
            write_timestamp(b + 34, Timestamp::from_ns(t1));
            gen_.send_to(b, 44, group_, 320);
            sync_seq++;
            next_sync += 125000000LL;
        }
        sleep_until_mono(std::min<int64_t>(next_sync, next_ann));
    }
}

void TestMaster::rx_loop() {
    uint8_t buf[256];
    while (run_) {
        int64_t rx = 0;
        int n = ev_.recv(buf, sizeof buf, &rx);
        if (n <= 0) continue;
        Header h;
        if (!parse_header(buf, n, h) || h.type != DelayReq || h.domain != domain_) continue;
        int64_t t4 = tai_now() - (mono_ns() - rx);
        uint8_t b[54];
        Header r;
        r.type = DelayResp;
        r.length = 54;
        r.domain = domain_;
        r.src_clock = id_;
        r.src_port = 1;
        r.seq = h.seq;
        r.control = 3;
        r.log_interval = 0;
        write_header(b, r);
        write_timestamp(b + 34, Timestamp::from_ns(t4));
        std::memcpy(b + 44, h.src_clock.data(), 8);
        b[52] = uint8_t(h.src_port >> 8);
        b[53] = uint8_t(h.src_port);
        gen_.send_to(b, 54, group_, 320);
    }
}

}  // namespace jvs
