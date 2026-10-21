// C interface for the Mac app: one session = one network port with its
// clock, discovery, at most one received stream and one set of sent flows.
#include "jvs/jvs_c.h"

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jvs/clock.h"
#include "jvs/net.h"
#include "jvs/player.h"
#include "jvs/ptp.h"
#include "jvs/rtp.h"
#include "jvs/sap.h"
#include "jvs/wav.h"

using namespace jvs;

namespace {
template <size_t N>
void copy_str(char (&dst)[N], const std::string& src) {
    std::snprintf(dst, N, "%s", src.c_str());
}
}  // namespace

struct RxJob {
    StreamDesc desc;
    std::unique_ptr<RtpReceiver> rx;
    std::unique_ptr<Player> player;
    WavWriter wav;
    std::string wav_path;
    double latency_ms = 4;
    int play_l = 1, play_r = 2;
    bool playing = false;
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<uint64_t> missing{0}, total{0};
    std::mutex peak_mu;
    std::vector<float> peak;
};

struct jvs_session {
    std::mutex mu;
    std::string error;
    NetInterface iface;
    bool started = false;
    std::unique_ptr<PtpFollower> ptp;
    std::unique_ptr<SapListener> sap;
    std::unique_ptr<SapAnnouncer> announcer;
    std::unique_ptr<RxJob> rx;
    std::vector<std::unique_ptr<RtpSender>> tx;
    int tx_channels = 0;
};

static void rx_loop(jvs_session* s, RxJob* j) {
    const StreamDesc& d = j->desc;
    const uint64_t latency = uint64_t(j->latency_ms * d.sample_rate / 1000.0);
    const int chunk = std::max(1, int(d.sample_rate / 1000));
    std::vector<float> buf(size_t(chunk) * size_t(d.channels));
    std::vector<float> stereo(size_t(chunk) * 2);
    std::vector<float> peak(size_t(d.channels), 0.0f);
    bool have_pos = false;
    uint64_t pos = 0;
    while (j->run) {
        if (!s->ptp->locked()) {
            have_pos = false;
            sleep_until_mono(mono_ns() + 10000000);
            continue;
        }
        uint64_t target = media_time_from_ptp(s->ptp->now(), d.sample_rate, d.media_clock_offset) - latency;
        if (!have_pos || target > pos + d.sample_rate || target + d.sample_rate < pos) {
            pos = target;
            have_pos = true;
        }
        while (pos + uint64_t(chunk) <= target) {
            int got = j->rx->ring().read(uint32_t(pos), buf.data(), chunk);
            j->missing += uint64_t(chunk - got);
            j->total += uint64_t(chunk);
            for (int f = 0; f < chunk; ++f)
                for (int c = 0; c < d.channels; ++c)
                    peak[size_t(c)] = std::max(peak[size_t(c)], std::fabs(buf[size_t(f * d.channels + c)]));
            if (!j->wav_path.empty()) j->wav.write(buf.data(), chunk);
            if (j->playing) {
                int l = std::clamp(j->play_l - 1, 0, d.channels - 1);
                int r = std::clamp(j->play_r - 1, 0, d.channels - 1);
                for (int f = 0; f < chunk; ++f) {
                    stereo[size_t(f * 2)] = buf[size_t(f * d.channels + l)];
                    stereo[size_t(f * 2 + 1)] = buf[size_t(f * d.channels + r)];
                }
                j->player->push(stereo.data(), chunk);
            }
            pos += uint64_t(chunk);
        }
        {
            std::lock_guard<std::mutex> lk(j->peak_mu);
            for (size_t c = 0; c < peak.size(); ++c) j->peak[c] = std::max(j->peak[c], peak[c]);
        }
        std::fill(peak.begin(), peak.end(), 0.0f);
        sleep_until_mono(mono_ns() + 1000000);
    }
}

static void stop_rx_locked(jvs_session* s) {
    if (!s->rx) return;
    s->rx->run = false;
    if (s->rx->th.joinable()) s->rx->th.join();
    s->rx->rx->stop();
    if (s->rx->player) s->rx->player->stop();
    s->rx->wav.close();
    s->rx.reset();
}

static void stop_tx_locked(jvs_session* s) {
    for (auto& t : s->tx) {
        t->stop();
        if (s->announcer) s->announcer->remove(t->desc().key());
    }
    s->tx.clear();
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

jvs_session* jvs_create(void) { return new jvs_session(); }

void jvs_destroy(jvs_session* s) {
    if (!s) return;
    jvs_stop(s);
    delete s;
}

int jvs_start(jvs_session* s, const char* iface) {
    jvs_stop(s);
    std::lock_guard<std::mutex> lk(s->mu);
    if (!find_interface(iface ? iface : "", s->iface)) {
        s->error = "That network port has no IPv4 address.";
        return -1;
    }
    s->ptp = std::make_unique<PtpFollower>();
    if (!s->ptp->start(s->iface, 0)) {
        s->error =
            "Couldn't open the clock ports (UDP 319/320). Quit Dante Virtual Soundcard, Dante Via or Dante "
            "Controller on this Mac and try again.";
        s->ptp.reset();
        return -1;
    }
    s->sap = std::make_unique<SapListener>();
    if (!s->sap->start(s->iface)) {
        s->error = "Couldn't listen for stream announcements (UDP 9875).";
        s->ptp->stop();
        s->ptp.reset();
        s->sap.reset();
        return -1;
    }
    s->announcer = std::make_unique<SapAnnouncer>();
    s->announcer->start(s->iface);
    s->started = true;
    s->error.clear();
    return 0;
}

void jvs_stop(jvs_session* s) {
    std::lock_guard<std::mutex> lk(s->mu);
    stop_rx_locked(s);
    stop_tx_locked(s);
    if (s->announcer) s->announcer->stop();
    if (s->sap) s->sap->stop();
    if (s->ptp) s->ptp->stop();
    s->announcer.reset();
    s->sap.reset();
    s->ptp.reset();
    s->started = false;
}

const char* jvs_last_error(jvs_session* s) { return s->error.c_str(); }

void jvs_get_clock(jvs_session* s, jvs_clock_status* out) {
    std::memset(out, 0, sizeof *out);
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->ptp) return;
    PtpStatus st = s->ptp->status();
    out->running = st.running;
    out->have_master = st.have_master;
    out->locked = st.locked;
    copy_str(out->grandmaster, st.grandmaster);
    copy_str(out->master_ip, st.have_master ? ipv4_to_string(st.master_ip) : "");
    out->delay_us = double(st.path_delay_ns) / 1000.0;
    out->jitter_us = st.offset_jitter_ns / 1000.0;
    out->rate_ppm = st.rate_ppm;
    out->syncs = st.syncs;
}

int jvs_get_streams(jvs_session* s, jvs_stream_info* out, int max) {
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->sap) return 0;
    auto list = s->sap->streams();
    int n = 0;
    for (auto& ds : list) {
        if (n >= max) break;
        const StreamDesc& d = ds.desc;
        jvs_stream_info& o = out[n++];
        std::memset(&o, 0, sizeof o);
        copy_str(o.key, d.key());
        copy_str(o.name, d.name);
        copy_str(o.addr, ipv4_to_string(d.dest_ip));
        copy_str(o.origin, ipv4_to_string(d.origin_ip));
        o.port = d.port;
        o.channels = d.channels;
        o.sample_rate = int(d.sample_rate);
        o.bits = d.encoding == Encoding::L24 ? 24 : 16;
        o.ptime_ms = d.ptime_us / 1000.0;
    }
    return n;
}

int jvs_rx_start(jvs_session* s, const char* key, double latency_ms, int play, int play_left, int play_right,
                 const char* wav_path) {
    std::lock_guard<std::mutex> lk(s->mu);
    stop_rx_locked(s);
    if (!s->started) {
        s->error = "Start the network first.";
        return -1;
    }
    StreamDesc d;
    bool found = false;
    for (auto& ds : s->sap->streams())
        if (ds.desc.key() == key) {
            d = ds.desc;
            found = true;
        }
    if (!found) {
        s->error = "That stream is no longer on the network.";
        return -1;
    }
    auto j = std::make_unique<RxJob>();
    j->desc = d;
    j->latency_ms = std::clamp(latency_ms, 0.25, 500.0);
    j->play_l = play_left;
    j->play_r = play_right;
    j->peak.assign(size_t(d.channels), 0.0f);
    j->rx = std::make_unique<RtpReceiver>(d);
    if (!j->rx->start(s->iface, s->ptp.get())) {
        s->error = "Couldn't join the stream's multicast address.";
        return -1;
    }
    if (play) {
        j->player = std::make_unique<Player>();
        j->playing = j->player->start(d.sample_rate);
    }
    if (wav_path && *wav_path) {
        if (!j->wav.open(wav_path, d.channels, d.sample_rate)) {
            s->error = std::string("Couldn't create ") + wav_path;
            j->rx->stop();
            return -1;
        }
        j->wav_path = wav_path;
    }
    j->run = true;
    RxJob* raw = j.get();
    j->th = std::thread([s, raw] { rx_loop(s, raw); });
    s->rx = std::move(j);
    return 0;
}

void jvs_rx_stop(jvs_session* s) {
    std::lock_guard<std::mutex> lk(s->mu);
    stop_rx_locked(s);
}

void jvs_get_rx(jvs_session* s, jvs_rx_status* out) {
    std::memset(out, 0, sizeof *out);
    for (float& p : out->peak_db) p = -120.0f;
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->rx) return;
    RxJob& j = *s->rx;
    RxStats st = j.rx->stats();
    out->active = 1;
    copy_str(out->stream_name, j.desc.name);
    out->channels = j.desc.channels;
    out->packets = st.packets;
    out->lost = st.lost;
    uint64_t total = j.total;
    out->missing_pct = total ? 100.0 * double(j.missing) / double(total) : 0.0;
    out->arrival_avg_us = st.transit_us_avg;
    out->arrival_max_us = st.transit_us_max;
    out->latency_ms = j.latency_ms;
    out->playing = j.playing;
    out->recording = !j.wav_path.empty();
    copy_str(out->wav_path, j.wav_path);
    std::lock_guard<std::mutex> pk(j.peak_mu);
    for (size_t c = 0; c < j.peak.size() && c < JVS_MAX_CHANNELS; ++c) {
        out->peak_db[c] = j.peak[c] > 1e-6f ? 20.0f * std::log10(j.peak[c]) : -120.0f;
        j.peak[c] = 0;
    }
}

int jvs_tx_start(jvs_session* s, const char* name, int channels, int flows, const char* base_addr) {
    std::lock_guard<std::mutex> lk(s->mu);
    stop_tx_locked(s);
    if (!s->started) {
        s->error = "Start the network first.";
        return -1;
    }
    if (!s->ptp->locked()) {
        s->error = "Wait for the clock to lock before sending.";
        return -1;
    }
    channels = std::clamp(channels, 1, 64);
    flows = std::clamp(flows, 1, 8);
    uint32_t base;
    if (!parse_ipv4(base_addr && *base_addr ? base_addr : "239.69.200.1", base)) {
        s->error = "That multicast address isn't valid.";
        return -1;
    }
    std::string nm = name && *name ? name : "JVS Test";
    uint64_t sid = uint64_t(std::time(nullptr));
    for (int f = 0; f < flows; ++f) {
        StreamDesc d;
        d.name = flows > 1 ? nm + " " + std::to_string(f + 1) : nm;
        d.session_id = std::to_string(sid + uint64_t(f));
        d.session_version = sid;
        d.origin_ip = s->iface.ipv4;
        d.dest_ip = htonl(ntohl(base) + uint32_t(f));
        d.channels = channels;
        d.ptp_gmid = s->ptp->grandmaster();
        auto t = std::make_unique<RtpSender>(d, make_test_tone(f * channels, channels));
        if (!t->start(s->iface, s->ptp.get())) {
            s->error = "Couldn't open a socket to send.";
            stop_tx_locked(s);
            return -1;
        }
        s->announcer->set(d);
        s->tx.push_back(std::move(t));
    }
    s->tx_channels = channels;
    return 0;
}

void jvs_tx_stop(jvs_session* s) {
    std::lock_guard<std::mutex> lk(s->mu);
    stop_tx_locked(s);
}

void jvs_get_tx(jvs_session* s, jvs_tx_status* out) {
    std::memset(out, 0, sizeof *out);
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->tx.empty()) return;
    out->active = 1;
    out->flows = int(s->tx.size());
    out->channels = s->tx_channels;
    for (auto& t : s->tx) {
        TxStats st = t->stats();
        out->packets += st.packets;
        out->late += st.late;
        out->resyncs += st.resyncs;
    }
}

}  // extern "C"
