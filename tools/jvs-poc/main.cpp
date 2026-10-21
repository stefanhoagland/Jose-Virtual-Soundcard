// jvs-poc: proof-of-concept tool for Jose Virtual Soundcard.
// Talks AES67 to Dante devices: follows the PTP clock, finds streams,
// receives them to a WAV file or the speakers, and sends test streams.
#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "jvs/clock.h"
#include "jvs/net.h"
#include "jvs/ptp.h"
#include "jvs/rtp.h"
#include "jvs/sap.h"
#include "jvs/sdp.h"
#include "jvs/wav.h"
#include "player.h"
#include "test_master.h"

using namespace jvs;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Args {
    std::string cmd;
    std::vector<std::pair<std::string, std::string>> opts;
    bool has(const std::string& k) const {
        for (auto& o : opts)
            if (o.first == k) return true;
        return false;
    }
    std::string get(const std::string& k, const std::string& def = "") const {
        for (auto& o : opts)
            if (o.first == k) return o.second;
        return def;
    }
    int get_int(const std::string& k, int def) const {
        auto v = get(k);
        return v.empty() ? def : std::atoi(v.c_str());
    }
    double get_double(const std::string& k, double def) const {
        auto v = get(k);
        return v.empty() ? def : std::atof(v.c_str());
    }
};

// Options that take no value.
bool is_flag(const std::string& k) { return k == "--tone" || k == "--play" || k == "--help" || k == "-h"; }

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc > 1) a.cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if (is_flag(k) || i + 1 >= argc)
            a.opts.push_back({k, ""});
        else
            a.opts.push_back({k, argv[++i]});
    }
    return a;
}

void usage() {
    std::puts(
        "jvs-poc - Jose Virtual Soundcard proof of concept (AES67 / Dante AES67 mode)\n"
        "\n"
        "Commands:\n"
        "  interfaces                         List network interfaces\n"
        "  clock    -i IFACE                  Follow the PTP clock and show its status\n"
        "  discover -i IFACE [--seconds 35]   List AES67 streams announced on the network\n"
        "  receive  -i IFACE (--stream N|NAME | --sdp FILE)\n"
        "           [--seconds 10] [--wav out.wav] [--latency-ms 4] [--play] [--play-ch 1,2]\n"
        "                                     Receive a stream; show levels, save WAV, play on Mac\n"
        "  send     -i IFACE [--name \"JVS Test\"] [--channels 8] [--flows 1]\n"
        "           [--addr 239.69.200.1] [--tone | --wav in.wav] [--seconds 0]\n"
        "                                     Send test streams (8 channels per flow for Dante)\n"
        "  ptp-master -i IFACE                Fake PTP clock for testing WITHOUT Dante gear.\n"
        "                                     Never run it on your Dante network.\n"
        "\n"
        "Common options: --domain 0 (PTP domain)\n"
        "IFACE is the network port wired to Dante, e.g. en0 or en5 (see 'interfaces').");
}

bool get_iface(const Args& a, NetInterface& ni) {
    std::string name = a.get("-i", a.get("--interface"));
    if (name.empty()) {
        std::fprintf(stderr, "Pick a network interface with -i (see 'jvs-poc interfaces').\n");
        return false;
    }
    if (!find_interface(name, ni)) {
        std::fprintf(stderr, "Interface '%s' not found or has no IPv4 address.\n", name.c_str());
        return false;
    }
    return true;
}

void print_ptp(const PtpStatus& s) {
    if (!s.have_master) {
        std::printf("PTP: waiting for a PTPv2 clock on domain %d...\n", s.domain);
        return;
    }
    std::printf("PTP: %s  grandmaster %s  via %s  delay %.1f us  jitter %.1f us  rate %+.2f ppm  syncs %llu\n",
                s.locked ? "LOCKED " : "locking", s.grandmaster.c_str(), ipv4_to_string(s.master_ip).c_str(),
                double(s.path_delay_ns) / 1000.0, s.offset_jitter_ns / 1000.0, s.rate_ppm,
                (unsigned long long)s.syncs);
}

bool wait_for_lock(PtpFollower& ptp, int timeout_s) {
    int64_t end = mono_ns() + int64_t(timeout_s) * 1000000000LL;
    int64_t next_print = 0;
    while (!g_stop && mono_ns() < end) {
        if (ptp.locked()) return true;
        if (mono_ns() >= next_print) {
            print_ptp(ptp.status());
            next_print = mono_ns() + 1000000000LL;
        }
        sleep_until_mono(mono_ns() + 100000000LL);
    }
    return ptp.locked();
}

void print_stream(int idx, const StreamDesc& d) {
    std::printf("  [%d] \"%s\"  %s:%u  %s/%u/%dch  ptime %.3g ms  from %s  clock %s\n", idx, d.name.c_str(),
                ipv4_to_string(d.dest_ip).c_str(), d.port, d.encoding == Encoding::L24 ? "L24" : "L16",
                d.sample_rate, d.channels, d.ptime_us / 1000.0, ipv4_to_string(d.origin_ip).c_str(),
                d.ptp_gmid.c_str());
}

// ---------------- commands ----------------

int cmd_interfaces() {
    for (auto& ni : list_interfaces()) {
        std::printf("%-10s %-16s", ni.name.c_str(), ipv4_to_string(ni.ipv4).c_str());
        if (ni.has_mac)
            std::printf(" %02x:%02x:%02x:%02x:%02x:%02x", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4],
                        ni.mac[5]);
        std::printf("\n");
    }
    return 0;
}

int cmd_clock(const Args& a) {
    NetInterface ni;
    if (!get_iface(a, ni)) return 2;
    PtpFollower ptp;
    if (!ptp.start(ni, uint8_t(a.get_int("--domain", 0)))) return 1;
    int seconds = a.get_int("--seconds", 0);
    int64_t end = seconds > 0 ? mono_ns() + int64_t(seconds) * 1000000000LL : INT64_MAX;
    while (!g_stop && mono_ns() < end) {
        print_ptp(ptp.status());
        sleep_until_mono(mono_ns() + 1000000000LL);
    }
    return ptp.locked() ? 0 : 1;
}

std::vector<DiscoveredStream> discover(const NetInterface& ni, int seconds, bool print) {
    SapListener sap;
    if (!sap.start(ni)) {
        std::fprintf(stderr, "Cannot listen for SAP on port 9875.\n");
        return {};
    }
    if (print) std::printf("Listening for stream announcements for %d s (Dante repeats them every ~30 s)...\n", seconds);
    int64_t end = mono_ns() + int64_t(seconds) * 1000000000LL;
    size_t shown = 0;
    while (!g_stop && mono_ns() < end) {
        auto s = sap.streams();
        if (print && s.size() != shown) {
            for (size_t i = shown; i < s.size(); ++i) print_stream(int(i), s[i].desc);
            shown = s.size();
        }
        sleep_until_mono(mono_ns() + 200000000LL);
    }
    return sap.streams();
}

int cmd_discover(const Args& a) {
    NetInterface ni;
    if (!get_iface(a, ni)) return 2;
    auto s = discover(ni, a.get_int("--seconds", 35), true);
    std::printf("Found %zu stream(s).\n", s.size());
    if (s.empty())
        std::printf("No streams? In Dante Controller, enable AES67 mode on a device (Device View > AES67 Config),\n"
                    "reboot it, then create a multicast transmit flow and tick 'AES67'.\n");
    return 0;
}

bool pick_stream(const Args& a, const NetInterface& ni, StreamDesc& d) {
    if (a.has("--sdp")) {
        std::ifstream f(a.get("--sdp"));
        std::stringstream ss;
        ss << f.rdbuf();
        std::string why;
        if (!parse_sdp(ss.str(), d, &why)) {
            std::fprintf(stderr, "SDP file not usable: %s\n", why.c_str());
            return false;
        }
        return true;
    }
    std::string want = a.get("--stream");
    if (want.empty()) {
        std::fprintf(stderr, "Say which stream with --stream (number or name from 'discover') or --sdp FILE.\n");
        return false;
    }
    std::printf("Looking for stream '%s'...\n", want.c_str());
    // Listen until the stream shows up (max 35 s).
    SapListener sap;
    if (!sap.start(ni)) return false;
    int64_t end = mono_ns() + 35LL * 1000000000LL;
    bool numeric = !want.empty() && std::all_of(want.begin(), want.end(), ::isdigit);
    while (!g_stop && mono_ns() < end) {
        auto s = sap.streams();
        for (size_t i = 0; i < s.size(); ++i) {
            bool match = numeric ? int(i) == std::atoi(want.c_str()) : s[i].desc.name.find(want) != std::string::npos;
            if (match) {
                d = s[i].desc;
                print_stream(int(i), d);
                return true;
            }
        }
        sleep_until_mono(mono_ns() + 200000000LL);
    }
    std::fprintf(stderr, "Stream not found.\n");
    return false;
}

std::vector<int> parse_list(const std::string& s) {
    std::vector<int> v;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) v.push_back(std::atoi(item.c_str()));
    return v;
}

int cmd_receive(const Args& a) {
    NetInterface ni;
    if (!get_iface(a, ni)) return 2;
    StreamDesc d;
    if (!pick_stream(a, ni, d)) return 1;

    PtpFollower ptp;
    if (!ptp.start(ni, uint8_t(a.get_int("--domain", d.ptp_domain)))) return 1;
    RtpReceiver rx(d);
    if (!rx.start(ni, &ptp)) {
        std::fprintf(stderr, "Cannot join %s:%u\n", ipv4_to_string(d.dest_ip).c_str(), d.port);
        return 1;
    }
    if (!wait_for_lock(ptp, 30)) {
        std::fprintf(stderr, "PTP did not lock. Is AES67 mode on, and is a Dante device the clock leader?\n");
        return 1;
    }
    print_ptp(ptp.status());
    if (!d.ptp_gmid.empty() && d.ptp_gmid != ptp.grandmaster())
        std::printf("Note: stream says its clock is %s but we follow %s.\n", d.ptp_gmid.c_str(),
                    ptp.grandmaster().c_str());

    WavWriter wav;
    std::string wav_path = a.get("--wav");
    if (!wav_path.empty() && !wav.open(wav_path, d.channels, d.sample_rate)) {
        std::fprintf(stderr, "Cannot write %s\n", wav_path.c_str());
        return 1;
    }
    Player player;
    std::vector<int> play_ch = parse_list(a.get("--play-ch", "1,2"));
    if (play_ch.size() < 2) play_ch.push_back(play_ch.empty() ? 1 : play_ch[0]);
    bool play = a.has("--play");
    if (play && !player.start(d.sample_rate)) {
        std::fprintf(stderr, "Playback is only available on macOS; continuing without it.\n");
        play = false;
    }

    const double latency_ms = a.get_double("--latency-ms", 4.0);
    const uint64_t latency = uint64_t(latency_ms * d.sample_rate / 1000.0);
    const int seconds = a.get_int("--seconds", 10);
    const int chunk = std::max(1, int(d.sample_rate / 1000));
    std::vector<float> buf(size_t(chunk) * size_t(d.channels));
    std::vector<float> stereo(size_t(chunk) * 2);
    std::vector<float> peak(size_t(d.channels), 0.0f);
    uint64_t pos = media_time_from_ptp(ptp.now(), d.sample_rate, d.media_clock_offset) - latency;
    uint64_t missing = 0, total = 0;
    int64_t end = seconds > 0 ? mono_ns() + int64_t(seconds) * 1000000000LL : INT64_MAX;
    int64_t next_print = mono_ns() + 1000000000LL;
    std::printf("Receiving %d channels with %.1f ms latency%s. Ctrl-C to stop.\n", d.channels, latency_ms,
                wav_path.empty() ? "" : (" into " + wav_path).c_str());

    while (!g_stop && mono_ns() < end) {
        uint64_t target = media_time_from_ptp(ptp.now(), d.sample_rate, d.media_clock_offset) - latency;
        if (target > pos + 48000) pos = target - uint64_t(chunk);  // stalled, skip ahead
        while (pos + uint64_t(chunk) <= target) {
            int got = rx.ring().read(uint32_t(pos), buf.data(), chunk);
            missing += uint64_t(chunk - got);
            total += uint64_t(chunk);
            for (int f = 0; f < chunk; ++f)
                for (int c = 0; c < d.channels; ++c)
                    peak[size_t(c)] = std::max(peak[size_t(c)], std::fabs(buf[size_t(f * d.channels + c)]));
            if (wav_path.size()) wav.write(buf.data(), chunk);
            if (play) {
                for (int f = 0; f < chunk; ++f)
                    for (int k = 0; k < 2; ++k) {
                        int c = std::clamp(play_ch[size_t(k)] - 1, 0, d.channels - 1);
                        stereo[size_t(f * 2 + k)] = buf[size_t(f * d.channels + c)];
                    }
                player.push(stereo.data(), chunk);
            }
            pos += uint64_t(chunk);
        }
        if (mono_ns() >= next_print) {
            next_print += 1000000000LL;
            auto st = rx.stats();
            std::printf("rx %llu pkts, lost %llu, missing %.2f%%, arrival %.0f us avg / %.0f us max after timestamp |",
                        (unsigned long long)st.packets, (unsigned long long)st.lost,
                        total ? 100.0 * double(missing) / double(total) : 0.0, st.transit_us_avg, st.transit_us_max);
            for (int c = 0; c < d.channels; ++c) {
                float p = peak[size_t(c)];
                if (p > 0)
                    std::printf(" %d:%.0f", c + 1, 20.0 * std::log10(p));
                else
                    std::printf(" %d:-inf", c + 1);
                peak[size_t(c)] = 0;
            }
            std::printf(" dBFS\n");
            std::fflush(stdout);
            if (st.transit_us_max > latency_ms * 1000.0)
                std::printf("  hint: packets arrive later than the latency; try --latency-ms %.0f\n",
                            std::ceil(st.transit_us_max / 1000.0) + 1);
        }
        sleep_until_mono(mono_ns() + 1000000);
    }
    wav.close();
    std::printf("Done. %.3f%% of samples missing.\n", total ? 100.0 * double(missing) / double(total) : 0.0);
    return 0;
}

int cmd_send(const Args& a) {
    NetInterface ni;
    if (!get_iface(a, ni)) return 2;
    int channels = std::clamp(a.get_int("--channels", 8), 1, 64);
    int flows = std::clamp(a.get_int("--flows", 1), 1, 8);
    if (channels > 8)
        std::printf("Note: Dante devices accept at most 8 channels per AES67 flow; use --flows for more.\n");
    std::string name = a.get("--name", "JVS Test");
    uint32_t base_addr;
    if (!parse_ipv4(a.get("--addr", "239.69.200.1"), base_addr)) {
        std::fprintf(stderr, "Bad --addr\n");
        return 2;
    }
    WavData wav;
    bool use_wav = a.has("--wav");
    if (use_wav) {
        std::string why;
        if (!read_wav(a.get("--wav"), wav, &why)) {
            std::fprintf(stderr, "%s\n", why.c_str());
            return 1;
        }
        if (wav.rate != 48000) std::printf("Note: WAV is %u Hz but the stream runs at 48000 Hz.\n", wav.rate);
    }

    PtpFollower ptp;
    if (!ptp.start(ni, uint8_t(a.get_int("--domain", 0)))) return 1;
    if (!wait_for_lock(ptp, 30)) {
        std::fprintf(stderr, "PTP did not lock.\n");
        return 1;
    }
    print_ptp(ptp.status());

    SapAnnouncer sap;
    if (!sap.start(ni)) {
        std::fprintf(stderr, "Cannot open SAP socket.\n");
        return 1;
    }
    std::vector<std::unique_ptr<RtpSender>> senders;
    uint64_t sid = uint64_t(std::time(nullptr));
    for (int f = 0; f < flows; ++f) {
        StreamDesc d;
        d.name = flows > 1 ? name + " " + std::to_string(f + 1) : name;
        d.session_id = std::to_string(sid + uint64_t(f));
        d.session_version = sid;
        d.origin_ip = ni.ipv4;
        // Consecutive multicast groups: 239.69.200.1, .2, ...
        uint32_t host = ntohl(base_addr) + uint32_t(f);
        d.dest_ip = htonl(host);
        d.channels = channels;
        d.ptp_gmid = ptp.grandmaster();
        d.ptp_domain = a.get_int("--domain", 0);
        int first_ch = f * channels;
        AudioSource src;
        if (use_wav) {
            auto pos = std::make_shared<size_t>(0);
            src = [&wav, pos, first_ch, channels](uint64_t, float* out, int frames) {
                size_t nf = wav.frames();
                for (int i = 0; i < frames; ++i) {
                    for (int c = 0; c < channels; ++c) {
                        int wc = (first_ch + c) % std::max(1, wav.channels);
                        out[i * channels + c] = nf ? wav.samples[(*pos) * size_t(wav.channels) + size_t(wc)] : 0.0f;
                    }
                    if (nf) *pos = (*pos + 1) % nf;
                }
            };
        } else {
            // Each channel gets its own pitch so you can tell them apart:
            // channel n plays 200 + 100*n Hz at -18 dBFS.
            src = [first_ch, channels](uint64_t mt, float* out, int frames) {
                for (int i = 0; i < frames; ++i) {
                    uint64_t t = (mt + uint64_t(i)) % 48000;
                    for (int c = 0; c < channels; ++c) {
                        int f_hz = 200 + 100 * ((first_ch + c) % 64 + 1);
                        double ph = 2.0 * M_PI * double(t * uint64_t(f_hz) % 48000) / 48000.0;
                        out[i * channels + c] = float(0.125 * std::sin(ph));
                    }
                }
            };
        }
        auto s = std::make_unique<RtpSender>(d, src);
        if (!s->start(ni, &ptp)) {
            std::fprintf(stderr, "Cannot open send socket.\n");
            return 1;
        }
        sap.set(d);
        std::printf("Sending \"%s\": %d ch to %s:%u\n", d.name.c_str(), channels, ipv4_to_string(d.dest_ip).c_str(),
                    d.port);
        senders.push_back(std::move(s));
    }
    std::printf("In Dante Controller these appear as AES67 flows you can route to any Dante receiver.\n");

    int seconds = a.get_int("--seconds", 0);
    int64_t end = seconds > 0 ? mono_ns() + int64_t(seconds) * 1000000000LL : INT64_MAX;
    while (!g_stop && mono_ns() < end) {
        sleep_until_mono(mono_ns() + 1000000000LL);
        uint64_t pk = 0, late = 0, rs = 0;
        for (auto& s : senders) {
            auto st = s->stats();
            pk += st.packets;
            late += st.late;
            rs += st.resyncs;
        }
        auto ps = ptp.status();
        std::printf("tx %llu pkts, late %llu, resyncs %llu | PTP %s jitter %.1f us\n", (unsigned long long)pk,
                    (unsigned long long)late, (unsigned long long)rs, ps.locked ? "locked" : "LOST",
                    ps.offset_jitter_ns / 1000.0);
        std::fflush(stdout);
    }
    for (auto& s : senders) s->stop();
    sap.stop();  // tells receivers the streams are gone
    return 0;
}

int cmd_ptp_master(const Args& a) {
    NetInterface ni;
    if (!get_iface(a, ni)) return 2;
    TestMaster m;
    if (!m.start(ni, uint8_t(a.get_int("--domain", 0)))) {
        std::fprintf(stderr, "Cannot start test master.\n");
        return 1;
    }
    std::printf("Test PTP master running on %s. Ctrl-C to stop.\n", ni.name.c_str());
    int seconds = a.get_int("--seconds", 0);
    int64_t end = seconds > 0 ? mono_ns() + int64_t(seconds) * 1000000000LL : INT64_MAX;
    while (!g_stop && mono_ns() < end) sleep_until_mono(mono_ns() + 100000000LL);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Args a = parse_args(argc, argv);
    if (a.cmd.empty() || a.cmd == "help" || a.cmd == "--help" || a.cmd == "-h" || a.has("--help")) {
        usage();
        return 0;
    }
    if (a.cmd == "interfaces") return cmd_interfaces();
    if (a.cmd == "clock") return cmd_clock(a);
    if (a.cmd == "discover") return cmd_discover(a);
    if (a.cmd == "receive") return cmd_receive(a);
    if (a.cmd == "send") return cmd_send(a);
    if (a.cmd == "ptp-master") return cmd_ptp_master(a);
    std::fprintf(stderr, "Unknown command '%s'.\n\n", a.cmd.c_str());
    usage();
    return 2;
}
