// End-to-end check of the C interface used by the Mac app:
// fake clock -> session sends 2 flows -> session discovers and receives flow 2.
#include <cstdio>
#include <cstring>
#include <string>

#include "jvs/clock.h"
#include "jvs/jvs_c.h"
#include "test_master.h"

using namespace jvs;

static void wait_ms(int ms) { sleep_until_mono(mono_ns() + int64_t(ms) * 1000000); }

int main(int argc, char** argv) {
    const char* ifname = argc > 1 ? argv[1] : "eth0";
    NetInterface ni;
    if (!find_interface(ifname, ni)) {
        std::printf("no interface %s\n", ifname);
        return 2;
    }
    TestMaster master;
    if (!master.start(ni)) return 2;

    jvs_interface ifs[16];
    int nif = jvs_list_interfaces(ifs, 16);
    std::printf("%d interface(s), first %s %s\n", nif, nif ? ifs[0].name : "-", nif ? ifs[0].ip : "-");

    jvs_session* s = jvs_create();
    if (jvs_start(s, ifname) != 0) {
        std::printf("start failed: %s\n", jvs_last_error(s));
        return 1;
    }
    jvs_clock_status cs;
    for (int i = 0; i < 100; ++i) {
        jvs_get_clock(s, &cs);
        if (cs.locked) break;
        wait_ms(100);
    }
    std::printf("clock locked=%d gm=%s jitter=%.1fus\n", cs.locked, cs.grandmaster, cs.jitter_us);
    if (!cs.locked) return 1;

    if (jvs_tx_start(s, "CAPI", 8, 2, "239.69.201.1") != 0) {
        std::printf("tx failed: %s\n", jvs_last_error(s));
        return 1;
    }
    jvs_stream_info st[16];
    int n = 0, want = -1;
    for (int i = 0; i < 50 && want < 0; ++i) {
        n = jvs_get_streams(s, st, 16);
        for (int k = 0; k < n; ++k)
            if (std::string(st[k].name) == "CAPI 2") want = k;
        wait_ms(100);
    }
    if (want < 0) {
        std::printf("stream not discovered\n");
        return 1;
    }
    std::printf("found %s %s ch=%d\n", st[want].name, st[want].addr, st[want].channels);
    if (jvs_rx_start(s, st[want].key, 20, 0, 1, 2, nullptr) != 0) {
        std::printf("rx failed: %s\n", jvs_last_error(s));
        return 1;
    }
    jvs_rx_status rx;
    wait_ms(1000);
    jvs_get_rx(s, &rx);  // discard first window
    wait_ms(2000);
    jvs_get_rx(s, &rx);
    jvs_tx_status tx;
    jvs_get_tx(s, &tx);
    std::printf("rx packets=%llu lost=%llu missing=%.2f%% | tx packets=%llu flows=%d\n", rx.packets, rx.lost,
                rx.missing_pct, tx.packets, tx.flows);
    int ok = 1;
    for (int c = 0; c < rx.channels; ++c) {
        std::printf(" %d:%.1f", c + 1, rx.peak_db[c]);
        if (rx.peak_db[c] < -19 || rx.peak_db[c] > -17) ok = 0;
    }
    std::printf(" dBFS\n");
    jvs_destroy(s);
    master.stop();
    if (!ok || rx.packets < 1000) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("capi test passed\n");
    return 0;
}
