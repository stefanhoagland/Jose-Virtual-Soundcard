// C interface for the Mac app: one session = one Dante device, plus test
// tools (levels, speaker monitor, WAV recording) on what it receives.
#include "jvs/jvs_c.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jvs/clock.h"
#include "jvs/dante_device.h"
#include "jvs/net.h"
#include "jvs/player.h"
#include "jvs/wav.h"

using namespace jvs;

namespace {
template <size_t N>
void copy_str(char (&dst)[N], const std::string& src) {
    std::snprintf(dst, N, "%s", src.c_str());
}

std::string clean_name(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += c;
        else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !out.empty() && out.back() != '-')
            out += '-';
        if (out.size() >= 31) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "Jose-Virtual-Soundcard" : out;
}
}  // namespace

struct jvs_session {
    std::mutex mu;  // start/stop and the test tools
    std::string error;
    std::unique_ptr<DanteDevice> dev;

    // Test tools, run by the monitor thread.
    std::thread th;
    std::atomic<bool> run{false};
    std::mutex tools_mu;
    std::unique_ptr<Player> player;
    int mon_l = 1, mon_r = 2;
    WavWriter wav;
    std::string wav_path;
    std::mutex peak_mu;
    std::vector<float> peak;
};

// Reads what the device receives, 1 ms at a time, a little behind "now".
static void monitor_loop(jvs_session* s) {
    DanteDevice& d = *s->dev;
    const int ch = d.config().rx_channels;
    const int rate = d.config().sample_rate;
    const int chunk = std::max(1, rate / 1000);
    const uint64_t margin = uint64_t(rate) * 2 / 1000;
    std::vector<float> buf(size_t(chunk) * size_t(std::max(ch, 1)));
    std::vector<float> stereo(size_t(chunk) * 2);
    std::vector<float> peak(size_t(ch), 0.0f);
    bool have_pos = false;
    uint64_t pos = 0;
    while (s->run) {
        uint64_t now;
        if (ch == 0 || d.state() != DanteState::Running || !d.now(now)) {
            have_pos = false;
            sleep_until_mono(mono_ns() + 10000000);
            continue;
        }
        uint64_t target = now - margin;
        if (!have_pos || target > pos + uint64_t(rate) / 4 || target + uint64_t(rate) / 4 < pos) {
            pos = target;  // start, or skip after a stall or clock step
            have_pos = true;
        }
        while (pos + uint64_t(chunk) <= target) {
            d.read_rx(pos, chunk, buf.data());
            for (int f = 0; f < chunk; ++f)
                for (int c = 0; c < ch; ++c)
                    peak[size_t(c)] = std::max(peak[size_t(c)], std::fabs(buf[size_t(f * ch + c)]));
            {
                std::lock_guard<std::mutex> lk(s->tools_mu);
                if (!s->wav_path.empty()) s->wav.write(buf.data(), chunk);
                if (s->player) {
                    int l = std::clamp(s->mon_l - 1, 0, ch - 1);
                    int r = std::clamp(s->mon_r - 1, 0, ch - 1);
                    for (int f = 0; f < chunk; ++f) {
                        stereo[size_t(f * 2)] = buf[size_t(f * ch + l)];
                        stereo[size_t(f * 2 + 1)] = buf[size_t(f * ch + r)];
                    }
                    s->player->push(stereo.data(), chunk);
                }
            }
            pos += uint64_t(chunk);
        }
        {
            std::lock_guard<std::mutex> lk(s->peak_mu);
            for (size_t c = 0; c < peak.size(); ++c) s->peak[c] = std::max(s->peak[c], peak[c]);
        }
        std::fill(peak.begin(), peak.end(), 0.0f);
        sleep_until_mono(mono_ns() + 1000000);
    }
}

static void stop_tools_locked(jvs_session* s) {
    std::lock_guard<std::mutex> lk(s->tools_mu);
    if (s->player) s->player->stop();
    s->player.reset();
    s->wav.close();
    s->wav_path.clear();
}

extern "C" {

int jvs_list_interfaces(jvs_interface* out, int max) {
    int n = 0;
    for (auto& ni : list_interfaces()) {
        if (n >= max) break;
        if (ni.name == "lo0" || ni.name == "lo") continue;
        copy_str(out[n].name, ni.name);
        copy_str(out[n].ip, ipv4_to_string(ni.ipv4));
        ++n;
    }
    return n;
}

void jvs_clean_name(const char* in, char* out, int out_size) {
    if (out_size <= 0) return;
    std::snprintf(out, size_t(out_size), "%s", clean_name(in ? in : "").c_str());
}

jvs_session* jvs_create(void) { return new jvs_session(); }

void jvs_destroy(jvs_session* s) {
    if (!s) return;
    jvs_stop(s);
    delete s;
}

int jvs_start(jvs_session* s, const jvs_config* c) {
    jvs_stop(s);
    std::lock_guard<std::mutex> lk(s->mu);
    DanteConfig cfg;
    cfg.name = clean_name(c->name ? c->name : "");
    cfg.iface = c->iface ? c->iface : "";
    cfg.rx_channels = std::clamp(c->rx_channels, 0, JVS_MAX_CHANNELS);
    cfg.tx_channels = std::clamp(c->tx_channels, 0, JVS_MAX_CHANNELS);
    switch (c->sample_rate) {
        case 44100: case 48000: case 88200: case 96000: cfg.sample_rate = c->sample_rate; break;
        default: cfg.sample_rate = 48000;
    }
    cfg.rx_latency_us = std::clamp(c->latency_us, 250, 100000);
    cfg.tx_latency_us = cfg.rx_latency_us;
    cfg.ptp_mode = c->ptp_mode == JVS_PTP_V1 ? PtpMode::V1 : c->ptp_mode == JVS_PTP_V2 ? PtpMode::V2 : PtpMode::Auto;
    auto dev = std::make_unique<DanteDevice>();
    std::string err;
    if (!dev->start(cfg, &err)) {
        s->error = err;
        return -1;
    }
    s->dev = std::move(dev);
    s->peak.assign(size_t(cfg.rx_channels), 0.0f);
    s->run = true;
    s->th = std::thread([s] { monitor_loop(s); });
    s->error.clear();
    return 0;
}

void jvs_stop(jvs_session* s) {
    std::lock_guard<std::mutex> lk(s->mu);
    s->run = false;
    if (s->th.joinable()) s->th.join();
    stop_tools_locked(s);
    if (s->dev) s->dev->stop();
    s->dev.reset();
}

const char* jvs_last_error(jvs_session* s) { return s->error.c_str(); }

void jvs_get_status(jvs_session* s, jvs_status* out) {
    std::memset(out, 0, sizeof *out);
    for (float& p : out->peak_db) p = -120.0f;
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->dev) return;
    DanteDevice& d = *s->dev;
    const DanteConfig& cfg = d.config();
    switch (d.state()) {
        case DanteState::Stopped: out->state = JVS_STOPPED; break;
        case DanteState::WaitingForClock: out->state = JVS_WAITING_FOR_CLOCK; break;
        case DanteState::Starting: out->state = JVS_STARTING; break;
        case DanteState::Running: out->state = JVS_RUNNING; break;
        case DanteState::Failed: out->state = JVS_FAILED; break;
    }
    copy_str(out->name, cfg.name);
    out->rx_channels = cfg.rx_channels;
    out->tx_channels = cfg.tx_channels;
    out->sample_rate = cfg.sample_rate;
    out->latency_us = cfg.rx_latency_us;

    PtpStatus st = d.clock_status();
    out->ptp_version = st.have_master ? st.version : 0;
    out->have_master = st.have_master;
    out->locked = st.locked;
    copy_str(out->grandmaster, st.grandmaster);
    copy_str(out->master_ip, st.have_master ? ipv4_to_string(st.master_ip) : "");
    out->delay_us = double(st.path_delay_ns) / 1000.0;
    out->jitter_us = st.offset_jitter_ns / 1000.0;
    out->rate_ppm = st.rate_ppm;
    out->syncs = st.syncs;

    out->tones = d.test_tones();
    {
        std::lock_guard<std::mutex> tk(s->tools_mu);
        out->monitoring = s->player != nullptr;
        out->monitor_left = s->mon_l;
        out->monitor_right = s->mon_r;
        out->recording = !s->wav_path.empty();
        copy_str(out->wav_path, s->wav_path);
    }
    std::lock_guard<std::mutex> pk(s->peak_mu);
    for (size_t c = 0; c < s->peak.size() && c < JVS_MAX_CHANNELS; ++c) {
        out->peak_db[c] = s->peak[c] > 1e-6f ? 20.0f * std::log10(s->peak[c]) : -120.0f;
        s->peak[c] = 0;
    }
}

void jvs_set_tones(jvs_session* s, int on) {
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->dev) s->dev->set_test_tones(on != 0);
}

int jvs_set_monitor(jvs_session* s, int on, int left, int right) {
    std::lock_guard<std::mutex> lk(s->mu);
    std::lock_guard<std::mutex> tk(s->tools_mu);
    s->mon_l = left;
    s->mon_r = right;
    if (!on) {
        if (s->player) s->player->stop();
        s->player.reset();
        return 0;
    }
    if (!s->dev) {
        s->error = "Start the device first.";
        return -1;
    }
    if (s->player) return 0;
    auto p = std::make_unique<Player>();
    if (!p->start(uint32_t(s->dev->config().sample_rate))) {
        s->error = "Couldn't open the Mac's speakers.";
        return -1;
    }
    s->player = std::move(p);
    return 0;
}

int jvs_record_start(jvs_session* s, const char* path) {
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->dev || s->dev->config().rx_channels == 0) {
        s->error = "Start the device with at least one receive channel first.";
        return -1;
    }
    std::lock_guard<std::mutex> tk(s->tools_mu);
    s->wav.close();
    s->wav_path.clear();
    if (!path || !*path || !s->wav.open(path, s->dev->config().rx_channels, uint32_t(s->dev->config().sample_rate))) {
        s->error = std::string("Couldn't create ") + (path ? path : "the recording");
        return -1;
    }
    s->wav_path = path;
    return 0;
}

void jvs_record_stop(jvs_session* s) {
    std::lock_guard<std::mutex> lk(s->mu);
    std::lock_guard<std::mutex> tk(s->tools_mu);
    s->wav.close();
    s->wav_path.clear();
}

int jvs_subscribe(jvs_session* s, int local_channel, const char* tx_channel, const char* tx_device) {
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->dev || local_channel < 1 || local_channel > s->dev->config().rx_channels || !tx_channel || !tx_device) {
        s->error = "That receive channel doesn't exist.";
        return -1;
    }
    if (!s->dev->subscribe(local_channel - 1, tx_channel, tx_device)) {
        s->error = "Couldn't make that route.";
        return -1;
    }
    return 0;
}

}  // extern "C"
