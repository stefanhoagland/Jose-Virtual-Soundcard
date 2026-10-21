// Unit tests for the wire formats and buffers. No network needed.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "jvs/net.h"
#include "jvs/ptp.h"
#include "jvs/rtp.h"
#include "jvs/sap.h"
#include "jvs/sdp.h"

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

// The shape of an SDP that a Dante device sends in AES67 mode.
static const char* kDanteSdp =
    "v=0\r\n"
    "o=- 1311738121 1311738121 IN IP4 192.168.1.101\r\n"
    "s=AVIOAI2-1d2ac9 : 2\r\n"
    "c=IN IP4 239.69.161.26/32\r\n"
    "t=0 0\r\n"
    "a=keywds:Dante\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "i=2 channels: Left, Right\r\n"
    "a=recvonly\r\n"
    "a=rtpmap:97 L24/48000/2\r\n"
    "a=ptime:1\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-1D-2A-C9:0\r\n"
    "a=mediaclk:direct=1145368364\r\n";

static void test_sdp() {
    StreamDesc d;
    std::string why;
    CHECK(parse_sdp(kDanteSdp, d, &why));
    CHECK(d.name == "AVIOAI2-1d2ac9 : 2");
    CHECK(ipv4_to_string(d.dest_ip) == "239.69.161.26");
    CHECK(ipv4_to_string(d.origin_ip) == "192.168.1.101");
    CHECK(d.port == 5004);
    CHECK(d.payload_type == 97);
    CHECK(d.encoding == Encoding::L24);
    CHECK(d.sample_rate == 48000);
    CHECK(d.channels == 2);
    CHECK(d.ptime_us == 1000);
    CHECK(d.frames_per_packet() == 48);
    CHECK(d.media_clock_offset == 1145368364u);
    CHECK(d.ptp_gmid == "00-1D-C1-FF-FE-1D-2A-C9");
    CHECK(d.ptp_domain == 0);

    // Round trip our own SDP.
    StreamDesc o;
    o.name = "JVS Test";
    o.session_id = "42";
    o.session_version = 7;
    parse_ipv4("10.0.0.5", o.origin_ip);
    parse_ipv4("239.69.200.1", o.dest_ip);
    o.channels = 8;
    o.ptime_us = 125;
    o.ptp_gmid = "00-11-22-FF-FE-33-44-55";
    StreamDesc back;
    CHECK(parse_sdp(build_sdp(o), back, &why));
    CHECK(back.name == o.name && back.channels == 8 && back.dest_ip == o.dest_ip);
    CHECK(back.ptime_us == 125);
    CHECK(back.frames_per_packet() == 6);
    CHECK(back.ptp_gmid == o.ptp_gmid);

    StreamDesc bad;
    CHECK(!parse_sdp("v=0\r\nm=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n", bad));
}

static void test_sap() {
    SapPacket p;
    p.origin_ip = 0x0100000A;
    p.msg_id_hash = 0xBEEF;
    p.sdp = kDanteSdp;
    auto bytes = build_sap(p);
    SapPacket q;
    CHECK(parse_sap(bytes.data(), int(bytes.size()), q));
    CHECK(q.msg_id_hash == 0xBEEF && q.origin_ip == p.origin_ip && !q.deletion);
    CHECK(q.payload_type == "application/sdp");
    CHECK(q.sdp == p.sdp);
    // Packets without the payload-type string are allowed too.
    std::vector<uint8_t> raw(bytes.begin(), bytes.begin() + 8);
    raw.insert(raw.end(), p.sdp.begin(), p.sdp.end());
    CHECK(parse_sap(raw.data(), int(raw.size()), q) && q.sdp == p.sdp);
    p.deletion = true;
    bytes = build_sap(p);
    CHECK(parse_sap(bytes.data(), int(bytes.size()), q) && q.deletion);
}

static void test_rtp() {
    uint8_t buf[12];
    RtpHeader h;
    h.payload_type = 97;
    h.seq = 65535;
    h.timestamp = 0xDEADBEEF;
    h.ssrc = 0x01020304;
    write_rtp(buf, h);
    RtpHeader g;
    CHECK(parse_rtp(buf, 12, g) == 12);
    CHECK(g.payload_type == 97 && g.seq == 65535 && g.timestamp == 0xDEADBEEF && g.ssrc == 0x01020304);

    float in[6] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f};
    uint8_t enc[18];
    float out[6];
    encode_samples(in, Encoding::L24, 6, enc);
    decode_samples(enc, Encoding::L24, 6, out);
    bool ok = true;
    for (int i = 0; i < 6; ++i) ok &= std::fabs(in[i] - out[i]) < 2e-7f;
    CHECK(ok);
    CHECK(enc[3] == 0x40 && enc[4] == 0x00 && enc[5] == 0x00);  // 0.5 -> 0x400000
    encode_samples(in, Encoding::L16, 6, enc);
    decode_samples(enc, Encoding::L16, 6, out);
    ok = true;
    for (int i = 0; i < 6; ++i) ok &= std::fabs(in[i] - out[i]) < 1e-4f;
    CHECK(ok);
}

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

static void test_ring() {
    MediaRing r(2, 1000);  // rounds to 1024
    float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    r.write(0xFFFFFFFE, a, 4);  // wraps across 2^32
    float o[8] = {};
    CHECK(r.read(0xFFFFFFFE, o, 4) == 4);
    CHECK(std::memcmp(a, o, sizeof a) == 0);
    CHECK(r.read(2, o, 2) == 0);  // not written yet
    CHECK(o[0] == 0 && o[3] == 0);
    // Overwritten slot (1024 later) must not read back as the old time.
    r.write(0xFFFFFFFE + 1024, a, 1);
    CHECK(r.read(0xFFFFFFFE, o, 1) == 0);
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
    test_sdp();
    test_sap();
    test_rtp();
    test_media_clock();
    test_ring();
    test_ptp_wire();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
