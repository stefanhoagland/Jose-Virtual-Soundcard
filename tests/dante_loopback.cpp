// End-to-end Dante test on one machine: a fake PTPv1 clock, device A sends
// test tones, device B subscribes to A's channels (like routing in Dante
// Controller) and we check B receives the right tone on every channel.
#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "jvs/clock.h"
#include "jvs/dante_device.h"
#include "test_master.h"

using namespace jvs;

static void wait_ms(int ms) { sleep_until_mono(mono_ns() + int64_t(ms) * 1000000); }

int main(int argc, char** argv) {
    std::string ifname = argc > 1 ? argv[1] : "eth0";
    int ptp_version = argc > 2 ? std::atoi(argv[2]) : 1;
    const int CH = 4;
    NetInterface ni;
    if (!find_interface(ifname, ni)) return 2;
    TestMaster master;
    if (!master.start(ni, 0, ptp_version)) return 2;

    DanteConfig ca;
    ca.name = "jvs-a";
    ca.iface = ifname;
    ca.rx_channels = 0;
    ca.tx_channels = CH;
    ca.process_id = 1;
    ca.alt_port = 41000;
    ca.ptp_mode = ptp_version == 1 ? PtpMode::V1 : PtpMode::V2;
    DanteConfig cb = ca;
    cb.name = "jvs-b";
    cb.rx_channels = CH;
    cb.tx_channels = 0;
    cb.process_id = 2;
    cb.alt_port = 41100;

    DanteDevice a, b;
    std::string err;
    if (!a.start(ca, &err) || !b.start(cb, &err)) {
        std::printf("start failed: %s\n", err.c_str());
        return 1;
    }
    a.set_test_tones(true);
    for (int i = 0; i < 300 && (a.state() != DanteState::Running || b.state() != DanteState::Running); ++i) wait_ms(100);
    auto cs = b.clock_status();
    std::printf("A %d, B %d, clock v%d locked=%d jitter %.1f us\n", int(a.state()), int(b.state()), cs.version,
                cs.locked, cs.offset_jitter_ns / 1000);
    if (a.state() != DanteState::Running || b.state() != DanteState::Running) return 1;

    wait_ms(3000);  // let mDNS settle
    for (int c = 0; c < CH; ++c) b.subscribe(c, "TX " + std::to_string(c + 1), "jvs-a");

    // Wait for audio, then analyse 100 ms.
    const int N = 4800;
    std::vector<float> buf(size_t(N) * CH);
    bool ok = false;
    for (int attempt = 0; attempt < 30 && !ok; ++attempt) {
        wait_ms(1000);
        uint64_t t;
        if (!b.now(t)) continue;
        b.read_rx(t - uint64_t(N) - 480, N, buf.data());
        ok = true;
        std::printf("t+%ds:", attempt + 1);
        for (int c = 0; c < CH; ++c) {
            double want = 200 + 100 * (c + 1), re = 0, im = 0, peak = 0;
            for (int i = 0; i < N; ++i) {
                double s = buf[size_t(i * CH + c)];
                re += s * std::cos(2 * M_PI * want * i / 48000);
                im += s * std::sin(2 * M_PI * want * i / 48000);
                peak = std::max(peak, std::fabs(s));
            }
            double amp = 2 * std::hypot(re, im) / N;
            std::printf("  ch%d %gHz amp %.3f peak %.3f", c + 1, want, amp, peak);
            if (amp < 0.10 || amp > 0.15) ok = false;
        }
        std::printf("\n");
    }

    // Glitch check: record channel 1 for 3 s while every CPU core is busy,
    // and count jumps in the waveform (each one is an audible click).
    int glitches = 0;
    if (ok) {
        std::atomic<bool> hog{true};
        std::vector<std::thread> hogs;
        for (unsigned i = 0; i < std::max(2u, std::thread::hardware_concurrency()); ++i)
            hogs.emplace_back([&hog] {
                volatile double x = 1;
                while (hog) x = x * 1.0000001 + 1e-9;
            });
        const int B = 2400;  // 50 ms blocks, read 20 ms behind now
        std::vector<float> blk(size_t(B) * CH);
        uint64_t t0 = 0;
        b.now(t0);
        uint64_t pos = t0 - 960;
        float prev1 = 0, prev2 = 0;
        int seen = 0;
        while (pos < t0 + 3 * 48000) {
            uint64_t now = 0;
            b.now(now);
            if (pos + B + 960 > now) {
                wait_ms(5);
                continue;
            }
            b.read_rx(pos, B, blk.data());
            for (int i = 0; i < B; ++i) {
                float s = blk[size_t(i * CH)];
                // A 300 Hz tone at 0.125 never bends more than 0.0002 per sample.
                if (seen >= 2 && std::fabs(s - 2 * prev1 + prev2) > 0.01f) {
                    if (glitches < 5) std::printf("  click at %.3f s (block sample %d)\n", double(int64_t(pos + uint64_t(i) - t0)) / 48000, i);
                    ++glitches;
                }
                prev2 = prev1;
                prev1 = s;
                ++seen;
            }
            pos += B;
        }
        hog = false;
        for (auto& h : hogs) h.join();
        std::printf("glitches in 3 s under full CPU load: %d\n", glitches);
        if (glitches > 0) ok = false;
    }
    b.stop();
    a.stop();
    master.stop();
    std::printf(ok ? "dante loopback passed\n" : "dante loopback FAILED\n");
    return ok ? 0 : 1;
}
