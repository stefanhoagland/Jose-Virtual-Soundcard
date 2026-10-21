// jvs-poc: command-line tool for Jose Virtual Soundcard.
// Runs a Dante device (through Inferno) on our PTP clock, from the terminal.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "jvs/clock.h"
#include "jvs/dante_device.h"
#include "jvs/net.h"
#include "jvs/ptp.h"
#include "jvs/wav.h"
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
bool is_flag(const std::string& k) { return k == "--tones" || k == "--help" || k == "-h"; }

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
        "jvs-poc - Jose Virtual Soundcard command-line tool (Dante, via Inferno)\n"
        "\n"
        "Commands:\n"
        "  interfaces                         List network interfaces\n"
        "  clock    -i IFACE                  Follow the PTP clock and show its status\n"
        "  device   -i IFACE [--name Jose-Virtual-Soundcard] [--rx 64] [--tx 64]\n"
        "           [--rate 48000] [--latency-ms 10] [--tones] [--wav out.wav] [--seconds 0]\n"
        "           [--subscribe LOCAL=CHANNEL@DEVICE ...]\n"
        "                                     Put a Dante device on the network; show levels\n"
        "  ptp-master -i IFACE [--ptp 1|2]    Fake PTP clock for testing WITHOUT Dante gear.\n"
        "                                     Never run it on your Dante network.\n"
        "\n"
        "Common options: --domain 0 (PTP domain), --ptp auto|1|2 (clock version)\n"
        "IFACE is the network port wired to Dante, e.g. en0 or en5 (see 'interfaces').");
}

PtpMode ptp_mode(const Args& a) {
    std::string v = a.get("--ptp", "auto");
    if (v == "1" || v == "v1") return PtpMode::V1;
    if (v == "2" || v == "v2") return PtpMode::V2;
    return PtpMode::Auto;
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
        std::printf("PTP: waiting for a clock on domain %d...\n", s.domain);
        return;
    }
    std::printf("PTPv%d: %s  grandmaster %s  via %s  delay %.1f us  jitter %.1f us  rate %+.2f ppm  syncs %llu\n",
                s.version, s.locked ? "LOCKED " : "locking", s.grandmaster.c_str(), ipv4_to_string(s.master_ip).c_str(),
                double(s.path_delay_ns) / 1000.0, s.offset_jitter_ns / 1000.0, s.rate_ppm,
                (unsigned long long)s.syncs);
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
    if (!ptp.start(ni, uint8_t(a.get_int("--domain", 0)), ptp_mode(a))) return 1;
    int seconds = a.get_int("--seconds", 0);
    int64_t end = seconds > 0 ? mono_ns() + int64_t(seconds) * 1000000000LL : INT64_MAX;
    while (!g_stop && mono_ns() < end) {
        print_ptp(ptp.status());
        sleep_until_mono(mono_ns() + 1000000000LL);
    }
    return ptp.locked() ? 0 : 1;
}


const char* state_name(DanteState s) {
    switch (s) {
        case DanteState::Stopped: return "stopped";
        case DanteState::WaitingForClock: return "waiting for a clock";
        case DanteState::Starting: return "starting";
        case DanteState::Running: return "running";
        case DanteState::Failed: return "failed";
    }
    return "?";
}

int cmd_device(const Args& a) {
    NetInterface ni;
    if (!get_iface(a, ni)) return 2;
    DanteConfig cfg;
    cfg.name = a.get("--name", "Jose-Virtual-Soundcard");
    cfg.iface = ni.name;
    cfg.rx_channels = std::clamp(a.get_int("--rx", 64), 0, 64);
    cfg.tx_channels = std::clamp(a.get_int("--tx", 64), 0, 64);
    cfg.sample_rate = a.get_int("--rate", 48000);
    cfg.rx_latency_us = int(a.get_double("--latency-ms", 10) * 1000);
    cfg.tx_latency_us = cfg.rx_latency_us;
    cfg.ptp_mode = ptp_mode(a);
    DanteDevice dev;
    std::string err;
    if (!dev.start(cfg, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    dev.set_test_tones(a.has("--tones"));
    WavWriter wav;
    std::string wav_path = a.get("--wav");
    if (!wav_path.empty() && !wav.open(wav_path, std::max(cfg.rx_channels, 1), uint32_t(cfg.sample_rate))) {
        std::fprintf(stderr, "Cannot create %s\n", wav_path.c_str());
        return 1;
    }
    std::vector<std::string> subs;
    for (auto& o : a.opts)
        if (o.first == "--subscribe") subs.push_back(o.second);
    bool subscribed = subs.empty();

    std::printf("Device \"%s\" on %s: %d in, %d out, %d Hz. Ctrl-C to stop.\n", cfg.name.c_str(), ni.name.c_str(),
                cfg.rx_channels, cfg.tx_channels, cfg.sample_rate);
    const int ch = cfg.rx_channels;
    const int chunk = cfg.sample_rate / 1000;
    std::vector<float> buf(size_t(chunk) * size_t(std::max(ch, 1)));
    std::vector<float> peak(size_t(ch), 0.0f);
    uint64_t pos = 0;
    bool have_pos = false;
    int seconds = a.get_int("--seconds", 0);
    int64_t start = mono_ns();
    int64_t end = seconds > 0 ? start + int64_t(seconds) * 1000000000LL : INT64_MAX;
    int64_t next_print = 0;
    while (!g_stop && mono_ns() < end && dev.state() != DanteState::Failed) {
        uint64_t now;
        if (dev.state() == DanteState::Running && dev.now(now)) {
            if (!subscribed && mono_ns() > start + 3000000000LL) {
                for (auto& sub : subs) {
                    // LOCAL=CHANNEL@DEVICE, e.g. 1=Left@AVIO-1
                    size_t eq = sub.find('='), at = sub.rfind('@');
                    if (eq == std::string::npos || at == std::string::npos || at < eq) {
                        std::fprintf(stderr, "Bad --subscribe '%s' (want 1=CHANNEL@DEVICE)\n", sub.c_str());
                        continue;
                    }
                    int local = std::atoi(sub.substr(0, eq).c_str()) - 1;
                    dev.subscribe(local, sub.substr(eq + 1, at - eq - 1), sub.substr(at + 1));
                }
                subscribed = true;
            }
            uint64_t target = now - uint64_t(cfg.sample_rate) * 2 / 1000;
            if (!have_pos || target > pos + uint64_t(cfg.sample_rate)) {
                pos = target;
                have_pos = true;
            }
            while (ch > 0 && pos + uint64_t(chunk) <= target) {
                dev.read_rx(pos, chunk, buf.data());
                for (int f = 0; f < chunk; ++f)
                    for (int c = 0; c < ch; ++c)
                        peak[size_t(c)] = std::max(peak[size_t(c)], std::fabs(buf[size_t(f * ch + c)]));
                if (!wav_path.empty()) wav.write(buf.data(), chunk);
                pos += uint64_t(chunk);
            }
        }
        if (mono_ns() >= next_print) {
            std::printf("[%s] ", state_name(dev.state()));
            print_ptp(dev.clock_status());
            if (dev.state() == DanteState::Running && ch > 0) {
                std::printf("  levels (dBFS):");
                for (int c = 0; c < ch; ++c) {
                    float p = peak[size_t(c)];
                    if (c % 16 == 0) std::printf("\n   ");
                    std::printf(" %3.0f", p > 1e-6f ? 20.0 * std::log10(p) : -120.0);
                }
                std::printf("\n");
            }
            std::fill(peak.begin(), peak.end(), 0.0f);
            next_print = mono_ns() + 1000000000LL;
        }
        sleep_until_mono(mono_ns() + 1000000);
    }
    bool failed = dev.state() == DanteState::Failed;
    dev.stop();
    wav.close();
    if (failed) std::fprintf(stderr, "The Dante device stopped with an error.\n");
    return failed ? 1 : 0;
}

int cmd_ptp_master(const Args& a) {
    NetInterface ni;
    if (!get_iface(a, ni)) return 2;
    TestMaster m;
    if (!m.start(ni, uint8_t(a.get_int("--domain", 0)), a.get("--ptp", "2") == "1" ? 1 : 2)) {
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
    if (a.cmd == "device") return cmd_device(a);
    if (a.cmd == "ptp-master") return cmd_ptp_master(a);
    std::fprintf(stderr, "Unknown command '%s'.\n\n", a.cmd.c_str());
    usage();
    return 2;
}
