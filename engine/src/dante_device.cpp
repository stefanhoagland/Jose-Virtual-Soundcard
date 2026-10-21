#include "jvs/dante_device.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "jvs/clock.h"
#include "jvs/media.h"
#include "../../dante/jvs_dante.h"

namespace jvs {

namespace {
constexpr float kToFloat = 1.0f / 2147483648.0f;

// Can we bind UDP ip:port the way Inferno will (no address reuse)?
bool udp_port_free(uint32_t ip_be, int port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(uint16_t(port));
    sa.sin_addr.s_addr = ip_be;
    bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
    ::close(fd);
    return ok;
}

// Dante's control ports: routing (ARC), device info (CMC), audio setup, info requests.
constexpr int kStandardPorts[] = {4440, 8800, 4455, 8700};

// Inferno's alternative layout uses base, base+1, base+2, base+3.
int pick_alt_port(uint32_t ip_be) {
    for (int base = 24440; base < 24440 + 100 * 10; base += 10) {
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) ok = udp_port_free(ip_be, base + i);
        if (ok) return base;
    }
    return 0;
}

inline int32_t to_i32(float f) {
    f = std::clamp(f, -1.0f, 1.0f);
    return int32_t(std::lrint(double(f) * 2147483392.0));  // a hair under INT32_MAX
}
}  // namespace

DanteDevice::DanteDevice() = default;
DanteDevice::~DanteDevice() { stop(); }

bool DanteDevice::start(const DanteConfig& cfg, std::string* error) {
    stop();
    cfg_ = cfg;
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        state_ = DanteState::Failed;
        return false;
    };
    NetInterface ni;
    if (!find_interface(cfg.iface, ni)) return fail("That network port has no IPv4 address.");
    if (!ptp_.start(ni, 0, cfg.ptp_mode))
        return fail(
            "Couldn't open the clock ports (UDP 319/320). Quit Dante Virtual Soundcard, Dante Via or Dante "
            "Controller on this Mac and try again.");
    char path[64];
    std::snprintf(path, sizeof path, "/tmp/jvs-clock-%d-%d", int(getpid()), cfg.process_id);
    if (!clock_server_.start(path, &ptp_)) {
        ptp_.stop();
        return fail("Couldn't create the clock socket.");
    }
    std::string ip = ipv4_to_string(ni.ipv4);
    JvsdConfig jc{};
    jc.name = cfg.name.c_str();
    jc.bind_ip = ip.c_str();
    jc.rx_channels = cfg.rx_channels;
    jc.tx_channels = cfg.tx_channels;
    jc.sample_rate = cfg.sample_rate;
    jc.rx_latency_us = cfg.rx_latency_us;
    jc.tx_latency_us = cfg.tx_latency_us;
    jc.clock_socket = clock_server_.path().c_str();
    jc.buffer_frames = buffer_frames_;
    jc.process_id = cfg.process_id;
    jc.alt_port = cfg.alt_port;
    // Dante Controller installs a helper (ConMon) that holds UDP 8700 and
    // 8800 on its Mac, and Dante Virtual Soundcard may hold others. If any
    // standard port is taken, use other ports; Dante Controller learns
    // them from the device's announcements.
    if (jc.alt_port == 0) {
        bool all_free = true;
        for (int port : kStandardPorts) all_free &= udp_port_free(ni.ipv4, port);
        if (!all_free) jc.alt_port = pick_alt_port(ni.ipv4);
    }
    arc_port_ = jc.alt_port > 0 ? jc.alt_port : 4440;
    jvsd_ = jvsd_create(&jc);
    if (!jvsd_) {
        clock_server_.stop();
        ptp_.stop();
        return fail("Couldn't start the Dante device (check the name and network port).");
    }
    buffer_frames_ = jvsd_buffer_frames(jvsd_);
    rx_ = jvsd_rx_buffer(jvsd_);
    tx_ = jvsd_tx_buffer(jvsd_);
    state_ = DanteState::WaitingForClock;
    run_ = true;
    th_ = std::thread([this] { worker(); });
    return true;
}

void DanteDevice::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    audio_ = false;
    if (jvsd_) jvsd_destroy(jvsd_);
    jvsd_ = nullptr;
    rx_ = nullptr;
    tx_ = nullptr;
    clock_server_.stop();
    ptp_.stop();
    state_ = DanteState::Stopped;
}

bool DanteDevice::now(uint64_t& t) const {
    if (!ptp_.locked()) return false;
    t = media_time_from_ptp(ptp_.now(), uint32_t(cfg_.sample_rate), 0);
    return true;
}

int64_t DanteDevice::index(uint64_t t) const {
    return int64_t((t - start_time_) % uint64_t(buffer_frames_));
}

bool DanteDevice::read_rx(uint64_t t, int frames, float* out) const {
    const int ch = cfg_.rx_channels;
    if (!audio_ || !rx_ || ch == 0 || frames > buffer_frames_) {
        std::fill(out, out + size_t(frames) * size_t(ch), 0.0f);
        return false;
    }
    for (int f = 0; f < frames; ++f) {
        const int32_t* src = rx_ + size_t(index(t + uint64_t(f))) * size_t(ch);
        for (int c = 0; c < ch; ++c) out[size_t(f) * size_t(ch) + size_t(c)] = float(__atomic_load_n(src + c, __ATOMIC_RELAXED)) * kToFloat;
    }
    return true;
}

bool DanteDevice::write_tx(uint64_t t, int frames, const float* in) {
    const int ch = cfg_.tx_channels;
    if (!audio_ || !tx_ || ch == 0 || frames > buffer_frames_) return false;
    for (int f = 0; f < frames; ++f) {
        int32_t* dst = tx_ + size_t(index(t + uint64_t(f))) * size_t(ch);
        for (int c = 0; c < ch; ++c) __atomic_store_n(dst + c, to_i32(in[size_t(f) * size_t(ch) + size_t(c)]), __ATOMIC_RELAXED);
    }
    return true;
}

bool DanteDevice::subscribe(int local_channel, const std::string& tx_channel, const std::string& tx_device) {
    return jvsd_ && jvsd_subscribe(jvsd_, local_channel, tx_channel.c_str(), tx_device.c_str()) == 0;
}

void DanteDevice::worker() {
    const int rate = cfg_.sample_rate;
    const int chunk = std::max(1, rate / 1000);
    // Inferno sends each sample the moment its time comes, straight from this
    // buffer, so the tones must already be there. Write them well ahead so a
    // late wake-up of this thread can't leave old samples (a click).
    const uint64_t lead = std::min<uint64_t>(uint64_t(rate) * 20 / 1000, uint64_t(buffer_frames_) / 4);
    AudioSource tone = make_test_tone(0, cfg_.tx_channels, uint32_t(rate));
    std::vector<float> buf(size_t(chunk) * size_t(std::max(1, cfg_.tx_channels)));
    std::vector<float> silence(buf.size(), 0.0f);
    uint64_t tx_pos = 0;
    bool tx_init = false;

    while (run_) {
        int st = jvsd_state(jvsd_);
        if (st < 0) {
            state_ = DanteState::Failed;
            break;
        }
        if (!ptp_.locked()) {
            if (!audio_) state_ = DanteState::WaitingForClock;
            sleep_until_mono(mono_ns() + 50000000);
            continue;
        }
        if (st == 0) {
            state_ = DanteState::Starting;
            sleep_until_mono(mono_ns() + 50000000);
            continue;
        }
        uint64_t t = 0;
        if (!now(t)) continue;
        if (!audio_) {
            start_time_ = t;
            jvsd_start_audio(jvsd_, t);
            audio_ = true;
            state_ = DanteState::Running;
        }
        // Keep the transmit buffer filled a little ahead: tones or silence.
        if (!tx_init || tx_pos + uint64_t(buffer_frames_) / 2 < t || tx_pos > t + uint64_t(buffer_frames_) / 2) {
            tx_pos = t;
            tx_init = true;
        }
        while (cfg_.tx_channels > 0 && tx_pos < t + lead) {
            if (tones_) {
                tone(tx_pos, buf.data(), chunk);
                write_tx(tx_pos, chunk, buf.data());
            } else {
                write_tx(tx_pos, chunk, silence.data());
            }
            tx_pos += uint64_t(chunk);
        }
        sleep_until_mono(mono_ns() + 1000000);
    }
}

}  // namespace jvs
