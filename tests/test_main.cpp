// Unit tests for the wire formats and clocks. No network needed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "jvs/clock_server.h"
#include "jvs/jvs_c.h"
#include "jvs/media.h"
#include "jvs/net.h"
#include "jvs/ptp.h"

using namespace jvs;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_pass;                                                        \
        } else {                                                             \
            ++g_fail;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static void test_media_clock() {
    // 1.5 s after the epoch at 48 kHz = 72000 samples.
    CHECK(media_time_from_ptp(1500000000LL, 48000, 0) == 72000);
    CHECK(media_time_from_ptp(1500000000LL, 48000, 10) == 72010);
    // Today's TAI time does not overflow.
    int64_t now = 1792000000LL * 1000000000LL;
    uint64_t mt = media_time_from_ptp(now, 96000, 5);
    CHECK(mt == 1792000000ULL * 96000 + 5);
    int64_t back = ptp_from_media_time(mt, 96000, 5);
    CHECK(back == now);
    CHECK(ptp_from_media_time(1, 48000, 0) == 20834);  // rounds up
}

static void test_tone() {
    auto tone = make_test_tone(0, 2, 48000);
    std::vector<float> b(480 * 2);
    tone(0, b.data(), 480);
    float peak0 = 0, peak1 = 0;
    for (int i = 0; i < 480; ++i) {
        peak0 = std::max(peak0, std::fabs(b[size_t(i * 2)]));
        peak1 = std::max(peak1, std::fabs(b[size_t(i * 2 + 1)]));
    }
    CHECK(std::fabs(peak0 - 0.125f) < 0.002f);  // -18 dBFS
    CHECK(std::fabs(peak1 - 0.125f) < 0.002f);
    // Same media time gives the same samples (tones are locked to the clock).
    std::vector<float> c(10 * 2);
    tone(100, c.data(), 10);
    CHECK(std::memcmp(c.data(), b.data() + 200, sizeof(float) * 20) == 0);
}

static void test_ptpv1_wire() {
    using namespace ptpv1;
    uint8_t b[kSyncLen] = {};
    Header h;
    h.uuid = {0x00, 0x1d, 0xc1, 0x12, 0x34, 0x56};
    h.port = 1;
    h.seq = 4321;
    h.control = Sync;
    h.flags = kFlagAssist;
    write_header(b, h);
    SyncBody sb;
    sb.origin_ns = 1792000000LL * 1000000000LL + 123456789;
    sb.gm_uuid = h.uuid;
    sb.gm_stratum = 1;
    sb.sync_interval = -2;
    write_sync(b, sb);
    CHECK(b[0] == 0 && b[1] == 1);  // versionPTP = 1
    CHECK(std::memcmp(b + 4, "_DFLT", 5) == 0);
    Header g;
    CHECK(parse_header(b, kSyncLen, g));
    CHECK(g.control == Sync && g.seq == 4321 && g.port == 1 && g.uuid == h.uuid && (g.flags & kFlagAssist));
    SyncBody gb;
    CHECK(parse_sync(b, kSyncLen, gb));
    CHECK(gb.origin_ns == sb.origin_ns && gb.gm_uuid == sb.gm_uuid && gb.gm_stratum == 1);
    CHECK(!parse_header(b, kHeaderLen - 1, g));  // too short
    // A PTPv2 packet is not PTPv1.
    uint8_t v2[64] = {};
    ptpmsg::Header h2;
    h2.type = ptpmsg::Sync;
    h2.length = 44;
    ptpmsg::write_header(v2, h2);
    CHECK(!parse_header(v2, 64, g));
    uint8_t ts[8];
    write_timestamp(ts, 5000000007LL);
    CHECK(read_timestamp(ts) == 5000000007LL);
}

static void test_clock_overlay() {
    uint8_t b[40];
    ClockServer::encode(b, 1, 1000, -2000, 1.5e-6);
    CHECK(b[0] == 'V' && b[1] == 'C');
    uint16_t major, minor, flags;
    int64_t id, last, shift;
    double fs;
    std::memcpy(&major, b + 2, 2);
    std::memcpy(&minor, b + 4, 2);
    std::memcpy(&flags, b + 6, 2);
    std::memcpy(&id, b + 8, 8);
    std::memcpy(&last, b + 16, 8);
    std::memcpy(&shift, b + 24, 8);
    std::memcpy(&fs, b + 32, 8);
    CHECK(major == 1 && minor == 0 && flags == 1);
    CHECK(id == 1 && last == 1000 && shift == -2000 && fs == 1.5e-6);
}

static void test_clean_name() {
    char out[32];
    jvs_clean_name("Jose Virtual Soundcard", out, sizeof out);
    CHECK(std::string(out) == "Jose-Virtual-Soundcard");
    jvs_clean_name("  St. Mark's  FOH!! ", out, sizeof out);
    CHECK(std::string(out) == "St-Marks-FOH");
    jvs_clean_name("", out, sizeof out);
    CHECK(std::string(out) == "Jose-Virtual-Soundcard");
    jvs_clean_name("a123456789012345678901234567890123456789", out, sizeof out);
    CHECK(std::strlen(out) == 31);
}

static void test_ptp_wire() {
    using namespace ptpmsg;
    uint8_t b[64];
    Header h;
    h.type = Announce;
    h.length = 64;
    h.domain = 3;
    h.flags = 0x0208;
    h.correction_ns_x16 = int64_t(1234) << 16;
    h.src_clock = {1, 2, 3, 4, 5, 6, 7, 8};
    h.src_port = 9;
    h.seq = 513;
    h.log_interval = -3;
    write_header(b, h);
    AnnounceBody a;
    a.priority1 = 1;
    a.clock_class = 6;
    a.gm_identity = {8, 7, 6, 5, 4, 3, 2, 1};
    write_announce(b, a);
    Timestamp t = Timestamp::from_ns(1792000000123456789LL);
    write_timestamp(b + 34, t);
    Header g;
    CHECK(parse_header(b, 64, g));
    CHECK(g.type == Announce && g.domain == 3 && g.two_step() && g.seq == 513 && g.src_port == 9);
    CHECK((g.correction_ns_x16 >> 16) == 1234);
    CHECK(g.log_interval == -3 && g.src_clock == h.src_clock);
    AnnounceBody ab;
    CHECK(parse_announce(b, 64, ab));
    CHECK(ab.priority1 == 1 && ab.clock_class == 6 && ab.gm_identity == a.gm_identity);
    CHECK(read_timestamp(b + 34).to_ns() == 1792000000123456789LL);
    CHECK(clock_identity_to_string(a.gm_identity) == "08-07-06-05-04-03-02-01");
    CHECK(clock_identity_from_mac({0x00, 0x1d, 0xc1, 0x12, 0x34, 0x56}) ==
          (ClockIdentity{0x00, 0x1d, 0xc1, 0xff, 0xfe, 0x12, 0x34, 0x56}));
    b[1] = 1;  // PTPv1 is ignored
    CHECK(!parse_header(b, 64, g));
}

int main() {
    test_media_clock();
    test_tone();
    test_ptp_wire();
    test_ptpv1_wire();
    test_clock_overlay();
    test_clean_name();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
