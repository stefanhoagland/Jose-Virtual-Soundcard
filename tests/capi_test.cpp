// Tests the C interface the Mac app uses, on one machine with a fake clock:
// the app's device and a second Dante device route test tones to each other.
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "jvs/clock.h"
#include "jvs/dante_device.h"
#include "jvs/jvs_c.h"
#include "test_master.h"

using namespace jvs;

static void wait_ms(int ms) { sleep_until_mono(mono_ns() + int64_t(ms) * 1000000); }

int main(int argc, char** argv) {
    std::string ifname = argc > 1 ? argv[1] : "eth0";
    int ptp_version = argc > 2 ? std::atoi(argv[2]) : 2;
    const int CH = 4;
    NetInterface ni;
    if (!find_interface(ifname, ni)) return 2;
    TestMaster master;
    if (!master.start(ni, 0, ptp_version)) return 2;

    jvs_interface ifs[16];
    int n = jvs_list_interfaces(ifs, 16);
    std::printf("%d interfaces, first %s %s\n", n, n ? ifs[0].name : "-", n ? ifs[0].ip : "-");

    jvs_session* s = jvs_create();
    jvs_config cfg{};
    cfg.name = "jvs app";  // cleaned to "jvs-app"
    cfg.iface = ifname.c_str();
    cfg.rx_channels = CH;
    cfg.tx_channels = CH;
    cfg.sample_rate = 48000;
    cfg.latency_us = 10000;
    cfg.ptp_mode = JVS_PTP_AUTO;
    if (jvs_start(s, &cfg) != 0) {
        std::printf("jvs_start failed: %s\n", jvs_last_error(s));
        return 1;
    }
    jvs_set_tones(s, 1);

    DanteConfig cb;
    cb.name = "jvs-b";
    cb.iface = ifname;
    cb.rx_channels = CH;
    cb.tx_channels = CH;
    cb.process_id = 2;
    cb.alt_port = 41100;
    DanteDevice b;
    std::string err;
    if (!b.start(cb, &err)) {
        std::printf("device b failed: %s\n", err.c_str());
        return 1;
    }
    b.set_test_tones(true);

    jvs_status st{};
    for (int i = 0; i < 300; ++i) {
        jvs_get_status(s, &st);
        if (st.state == JVS_RUNNING && b.state() == DanteState::Running) break;
        wait_ms(100);
    }
    std::printf("app device \"%s\" state %d, clock PTPv%d locked %d jitter %.1f us\n", st.name, st.state,
                st.ptp_version, st.locked, st.jitter_us);
    bool ok = st.state == JVS_RUNNING && std::string(st.name) == "jvs-app" && st.ptp_version == ptp_version;

    wait_ms(3000);  // let mDNS settle
    for (int c = 1; c <= CH; ++c) {
        std::string ch = "TX " + std::to_string(c);
        if (jvs_subscribe(s, c, ch.c_str(), "jvs-b") != 0) ok = false;
        b.subscribe(c - 1, ch, "jvs-app");
    }
    const char* wav = "/tmp/jvs_capi_test.wav";
    if (jvs_record_start(s, wav) != 0) ok = false;

    bool levels = false;
    for (int i = 0; i < 30 && !levels; ++i) {
        wait_ms(1000);
        jvs_get_status(s, &st);
        levels = true;
        std::printf("app levels:");
        for (int c = 0; c < CH; ++c) {
            std::printf(" %.1f", st.peak_db[c]);
            if (std::fabs(st.peak_db[c] + 18.06f) > 1.0f) levels = false;
        }
        std::printf(" dBFS\n");
    }
    ok &= levels;

    // The other direction: device b hears the app's tones.
    bool heard = false;
    std::vector<float> buf(4800 * CH);
    for (int i = 0; i < 30 && !heard; ++i) {
        uint64_t t;
        if (!b.now(t) || !b.read_rx(t - 4800 - 480, 4800, buf.data())) continue;
        heard = true;
        std::printf("b peaks:");
        for (int c = 0; c < CH; ++c) {
            float peak = 0;
            for (int f = 0; f < 4800; ++f) peak = std::max(peak, std::fabs(buf[size_t(f * CH + c)]));
            std::printf(" %.3f", peak);
            if (std::fabs(peak - 0.125f) > 0.01f) heard = false;
        }
        std::printf("\n");
        if (!heard) wait_ms(1000);
    }
    ok &= heard;

    jvs_record_stop(s);
    struct stat sb {};
    bool wav_ok = stat(wav, &sb) == 0 && sb.st_size > 44 + 48000 * CH * 4 / 2;
    std::printf("recording %lld bytes\n", (long long)sb.st_size);
    ok &= wav_ok;

    b.stop();
    jvs_destroy(s);
    master.stop();
    std::printf(ok ? "capi test passed\n" : "capi test FAILED\n");
    return ok ? 0 : 1;
}
