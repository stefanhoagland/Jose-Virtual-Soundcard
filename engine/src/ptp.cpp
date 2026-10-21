#include "jvs/ptp.h"
#include "jvs/clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace jvs {

namespace {
constexpr uint16_t kEventPort = 319;
constexpr uint16_t kGeneralPort = 320;
constexpr const char* kPtpGroup = "224.0.1.129";
constexpr int64_t kMasterTimeoutNs = 6000000000LL;
constexpr size_t kWindow = 64;

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
}  // namespace

std::string clock_identity_to_string(const ClockIdentity& id) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1], id[2], id[3],
                  id[4], id[5], id[6], id[7]);
    return buf;
}

ClockIdentity clock_identity_from_mac(const std::array<uint8_t, 6>& m) {
    return {m[0], m[1], m[2], 0xFF, 0xFE, m[3], m[4], m[5]};
}

namespace ptpmsg {

Timestamp Timestamp::from_ns(int64_t ns) {
    Timestamp t;
    if (ns < 0) ns = 0;
    t.seconds = uint64_t(ns / 1000000000LL);
    t.nanoseconds = uint32_t(ns % 1000000000LL);
    return t;
}

bool parse_header(const uint8_t* p, int len, Header& h) {
    if (len < kHeaderLen) return false;
    h.type = p[0] & 0x0F;
    h.version = p[1] & 0x0F;
    h.length = rd16(p + 2);
    h.domain = p[4];
    h.flags = rd16(p + 6);
    uint64_t c = 0;
    for (int i = 0; i < 8; ++i) c = (c << 8) | p[8 + i];
    h.correction_ns_x16 = int64_t(c);
    std::memcpy(h.src_clock.data(), p + 20, 8);
    h.src_port = rd16(p + 28);
    h.seq = rd16(p + 30);
    h.control = p[32];
    h.log_interval = int8_t(p[33]);
    return h.version == 2 && h.length <= len;
}

void write_header(uint8_t* p, const Header& h) {
    std::memset(p, 0, kHeaderLen);
    p[0] = h.type & 0x0F;
    p[1] = h.version;
    wr16(p + 2, h.length);
    p[4] = h.domain;
    wr16(p + 6, h.flags);
    uint64_t c = uint64_t(h.correction_ns_x16);
    for (int i = 7; i >= 0; --i) {
        p[8 + i] = uint8_t(c);
        c >>= 8;
    }
    std::memcpy(p + 20, h.src_clock.data(), 8);
    wr16(p + 28, h.src_port);
    wr16(p + 30, h.seq);
    p[32] = h.control;
    p[33] = uint8_t(h.log_interval);
}

Timestamp read_timestamp(const uint8_t* p) {
    Timestamp t;
    for (int i = 0; i < 6; ++i) t.seconds = (t.seconds << 8) | p[i];
    t.nanoseconds = uint32_t(p[6]) << 24 | uint32_t(p[7]) << 16 | uint32_t(p[8]) << 8 | p[9];
    return t;
}

void write_timestamp(uint8_t* p, const Timestamp& t) {
    uint64_t s = t.seconds;
    for (int i = 5; i >= 0; --i) {
        p[i] = uint8_t(s);
        s >>= 8;
    }
    p[6] = uint8_t(t.nanoseconds >> 24);
    p[7] = uint8_t(t.nanoseconds >> 16);
    p[8] = uint8_t(t.nanoseconds >> 8);
    p[9] = uint8_t(t.nanoseconds);
}

bool parse_announce(const uint8_t* p, int len, AnnounceBody& a) {
    if (len < 64) return false;
    a.utc_offset = int16_t(rd16(p + 44));
    a.priority1 = p[47];
    a.clock_class = p[48];
    a.clock_accuracy = p[49];
    a.variance = rd16(p + 50);
    a.priority2 = p[52];
    std::memcpy(a.gm_identity.data(), p + 53, 8);
    a.steps_removed = rd16(p + 61);
    a.time_source = p[63];
    return true;
}

void write_announce(uint8_t* p, const AnnounceBody& a) {
    std::memset(p + 34, 0, 30);
    wr16(p + 44, uint16_t(a.utc_offset));
    p[47] = a.priority1;
    p[48] = a.clock_class;
    p[49] = a.clock_accuracy;
    wr16(p + 50, a.variance);
    p[52] = a.priority2;
    std::memcpy(p + 53, a.gm_identity.data(), 8);
    wr16(p + 61, a.steps_removed);
    p[63] = a.time_source;
}

}  // namespace ptpmsg

using namespace ptpmsg;

PtpFollower::PtpFollower() = default;
PtpFollower::~PtpFollower() { stop(); }

bool PtpFollower::start(const NetInterface& iface, uint8_t domain) {
    stop();
    iface_ = iface;
    domain_ = domain;
    if (iface.has_mac) {
        my_identity_ = clock_identity_from_mac(iface.mac);
    } else {
        std::random_device rd;
        for (auto& b : my_identity_) b = uint8_t(rd());
    }
    uint32_t group;
    parse_ipv4(kPtpGroup, group);
    if (!ev_sock_.open(kEventPort) || !gen_sock_.open(kGeneralPort)) {
        std::fprintf(stderr, "ptp: cannot bind UDP ports 319/320 (another PTP program running?)\n");
        return false;
    }
    if (!ev_sock_.join(group, iface.ipv4) || !gen_sock_.join(group, iface.ipv4)) {
        std::fprintf(stderr, "ptp: cannot join %s on %s\n", kPtpGroup, iface.name.c_str());
        return false;
    }
    ev_sock_.set_multicast_out(iface.ipv4, 1, true);
    ev_sock_.set_dscp(46);
    ev_sock_.enable_rx_timestamps();
    ev_sock_.set_recv_timeout_ms(50);
    gen_sock_.set_recv_timeout_ms(200);
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_ = PtpStatus{};
        st_.running = true;
        st_.domain = domain;
        masters_.clear();
        current_ = -1;
        samples_.clear();
        have_origin_ = false;
        have_delay_ = false;
    }
    run_ = true;
    ev_thread_ = std::thread([this] { event_loop(); });
    gen_thread_ = std::thread([this] { general_loop(); });
    return true;
}

void PtpFollower::stop() {
    run_ = false;
    if (ev_thread_.joinable()) ev_thread_.join();
    if (gen_thread_.joinable()) gen_thread_.join();
    ev_sock_.close();
    gen_sock_.close();
    locked_ = false;
    std::lock_guard<std::mutex> lk(mu_);
    st_.running = false;
}

void PtpFollower::event_loop() {
    uint8_t buf[256];
    while (run_) {
        int64_t rx = 0;
        uint32_t src = 0;
        int n = ev_sock_.recv(buf, sizeof buf, &rx, &src);
        if (n > 0) handle(buf, n, rx, src, true);
        maybe_send_delay_req(mono_ns());
    }
}

void PtpFollower::general_loop() {
    uint8_t buf[256];
    while (run_) {
        int64_t rx = 0;
        uint32_t src = 0;
        int n = gen_sock_.recv(buf, sizeof buf, &rx, &src);
        if (n > 0) handle(buf, n, rx, src, false);
    }
}

void PtpFollower::handle(const uint8_t* buf, int n, int64_t rx_mono, uint32_t src, bool) {
    Header h;
    if (!parse_header(buf, n, h)) return;  // also drops PTPv1 (native Dante)
    if (h.domain != domain_) return;
    if (h.src_clock == my_identity_) return;  // our own Delay_Req looped back

    std::lock_guard<std::mutex> lk(mu_);
    int64_t now = mono_ns();

    if (h.type == Announce) {
        AnnounceBody a;
        if (!parse_announce(buf, n, a)) return;
        auto it = std::find_if(masters_.begin(), masters_.end(), [&](const Master& m) {
            return m.clock == h.src_clock && m.port == h.src_port;
        });
        if (it == masters_.end()) {
            masters_.push_back({h.src_clock, h.src_port, src, a, now});
        } else {
            it->announce = a;
            it->ip = src;
            it->last_seen_mono = now;
        }
        select_master(now);
        return;
    }

    if (current_ < 0) return;
    const Master& cm = masters_[size_t(current_)];
    bool from_master = cm.clock == h.src_clock && cm.port == h.src_port;
    if (!from_master) return;

    int64_t corr = h.correction_ns_x16 >> 16;
    if (h.type == Sync) {
        if (n < 44) return;
        if (h.two_step()) {
            pending_seq_ = h.seq;
            pending_t2_ = rx_mono;
            pending_corr_ = corr;
            pending_valid_ = true;
        } else {
            on_sync_complete(read_timestamp(buf + 34).to_ns(), rx_mono, corr);
        }
    } else if (h.type == FollowUp) {
        if (n < 44 || !pending_valid_ || h.seq != pending_seq_) return;
        pending_valid_ = false;
        on_sync_complete(read_timestamp(buf + 34).to_ns(), pending_t2_, pending_corr_ + corr);
    } else if (h.type == DelayResp) {
        if (n < 54 || !dreq_outstanding_ || h.seq != dreq_seq_) return;
        ClockIdentity req;
        std::memcpy(req.data(), buf + 44, 8);
        if (req != my_identity_) return;
        dreq_outstanding_ = false;
        // Master asks us to use this interval between Delay_Reqs.
        int li = std::clamp<int>(h.log_interval, -4, 4);
        dreq_interval_ns_ = std::max<int64_t>(int64_t(std::ldexp(1e9, li)), 250000000LL);
        if (samples_.size() < 2) return;
        int64_t t4 = read_timestamp(buf + 34).to_ns() - corr;
        double m3 = double(dreq_t3_ - x0_);
        double a = m_a_.load(), b = m_b_.load();
        // Model without path delay at t3 (see fit_model()).
        int64_t no_delay = dreq_t3_ + y0_ + int64_t(std::llround(a - double(path_delay_) + b * m3));
        int64_t d = (t4 - no_delay) / 2;
        if (d < 0 || d > 10000000) return;  // nonsense, ignore
        // Smooth: fast at first, then slow.
        if (!have_delay_) {
            path_delay_ = d;
            have_delay_ = true;
        } else {
            path_delay_ += (d - path_delay_) / 8;
        }
        st_.delay_resps++;
        st_.path_delay_ns = path_delay_;
        fit_model();
    }
}

void PtpFollower::select_master(int64_t now) {
    masters_.erase(std::remove_if(masters_.begin(), masters_.end(),
                                  [&](const Master& m) { return now - m.last_seen_mono > kMasterTimeoutNs; }),
                   masters_.end());
    int best = -1;
    auto key = [](const Master& m) {
        const auto& a = m.announce;
        return std::make_tuple(a.priority1, a.clock_class, a.clock_accuracy, a.variance, a.priority2,
                               a.gm_identity, a.steps_removed, m.clock, m.port);
    };
    for (size_t i = 0; i < masters_.size(); ++i)
        if (best < 0 || key(masters_[i]) < key(masters_[size_t(best)])) best = int(i);

    current_ = best;
    if (best < 0) {
        st_.have_master = false;
        locked_ = false;
        return;
    }
    const Master& m = masters_[size_t(best)];
    if (!st_.have_master || m.clock != last_clock_ || m.port != last_port_) {
        // New master: start the clock fit again.
        samples_.clear();
        have_origin_ = false;
        have_delay_ = false;
        pending_valid_ = false;
        locked_ = false;
        next_dreq_ = 0;
    }
    last_clock_ = m.clock;
    last_port_ = m.port;
    st_.have_master = true;
    st_.master = clock_identity_to_string(m.clock);
    st_.grandmaster = clock_identity_to_string(m.announce.gm_identity);
    st_.master_ip = m.ip;
}

void PtpFollower::on_sync_complete(int64_t t1, int64_t t2_mono, int64_t correction_ns) {
    st_.syncs++;
    int64_t raw = t1 + correction_ns - t2_mono;  // ptp - mono, without path delay
    if (!have_origin_) {
        x0_ = t2_mono;
        y0_ = raw;
        have_origin_ = true;
        samples_.clear();
    }
    Sample s{double(t2_mono - x0_), double(raw - y0_)};

    // Large jump against the current model: master stepped its clock.
    if (samples_.size() >= 8) {
        double pred = m_a_.load() - double(path_delay_) + m_b_.load() * s.x;
        if (std::fabs(s.y - pred) > 1e6) {
            if (++jumps_ >= 3) {
                jumps_ = 0;
                samples_.clear();
                x0_ = t2_mono;
                y0_ = raw;
                s = {0, 0};
                locked_ = false;
            } else {
                return;
            }
        }
    }
    jumps_ = 0;
    samples_.push_back(s);
    if (samples_.size() > kWindow) samples_.erase(samples_.begin());
    fit_model();
}

void PtpFollower::fit_model() {
    if (samples_.empty()) return;
    auto fit = [](const std::vector<Sample>& v, double& a, double& b) {
        double n = double(v.size()), sx = 0, sy = 0;
        for (auto& s : v) {
            sx += s.x;
            sy += s.y;
        }
        double mx = sx / n, my = sy / n, sxx = 0, sxy = 0;
        for (auto& s : v) {
            sxx += (s.x - mx) * (s.x - mx);
            sxy += (s.x - mx) * (s.y - my);
        }
        b = sxx > 0 ? sxy / sxx : 0;
        a = my - b * mx;
    };
    double a, b;
    fit(samples_, a, b);
    double rms = 0;
    if (samples_.size() >= 8) {
        // Drop outliers (late packets) and fit again.
        std::vector<double> res;
        for (auto& s : samples_) res.push_back(std::fabs(s.y - (a + b * s.x)));
        std::vector<double> sorted = res;
        std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
        double mad = sorted[sorted.size() / 2];
        double limit = std::max(4.0 * 1.4826 * mad, 20000.0);
        std::vector<Sample> kept;
        for (size_t i = 0; i < samples_.size(); ++i)
            if (res[i] <= limit) kept.push_back(samples_[i]);
        if (kept.size() >= 4) fit(kept, a, b);
        for (auto& s : kept) {
            double r = s.y - (a + b * s.x);
            rms += r * r;
        }
        rms = std::sqrt(rms / double(std::max<size_t>(kept.size(), 1)));
    }
    // Publish (sequence lock: odd while writing).
    model_seq_.fetch_add(1, std::memory_order_acq_rel);
    m_x0_.store(x0_, std::memory_order_relaxed);
    m_y0_.store(y0_, std::memory_order_relaxed);
    m_a_.store(a + double(path_delay_), std::memory_order_relaxed);
    m_b_.store(b, std::memory_order_relaxed);
    model_seq_.fetch_add(1, std::memory_order_release);

    st_.offset_jitter_ns = rms;
    st_.rate_ppm = b * 1e6;
    bool lk = samples_.size() >= 8 && have_delay_ && rms < 500000;
    locked_ = lk;
    st_.locked = lk;
}

void PtpFollower::maybe_send_delay_req(int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    // Drop a master that went silent.
    if (current_ >= 0 && now - masters_[size_t(current_)].last_seen_mono > kMasterTimeoutNs) select_master(now);
    if (current_ < 0 || samples_.size() < 2) return;
    if (now < next_dreq_) return;
    // Jitter the interval a little so many followers don't line up.
    next_dreq_ = now + dreq_interval_ns_ / 2 + int64_t(rng_() % uint64_t(dreq_interval_ns_));
    uint8_t buf[44];
    Header h;
    h.type = DelayReq;
    h.length = 44;
    h.domain = domain_;
    h.src_clock = my_identity_;
    h.src_port = 1;
    h.seq = ++dreq_seq_;
    h.control = 1;
    h.log_interval = 0x7F;
    write_header(buf, h);
    write_timestamp(buf + 34, Timestamp{});
    uint32_t group;
    parse_ipv4(kPtpGroup, group);
    dreq_t3_ = mono_ns();
    dreq_outstanding_ = ev_sock_.send_to(buf, sizeof buf, group, kEventPort);
}

int64_t PtpFollower::ptp_from_mono(int64_t mono) const {
    for (;;) {
        uint32_t s0 = model_seq_.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        int64_t x0 = m_x0_.load(std::memory_order_relaxed);
        int64_t y0 = m_y0_.load(std::memory_order_relaxed);
        double a = m_a_.load(std::memory_order_relaxed);
        double b = m_b_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (model_seq_.load(std::memory_order_relaxed) != s0) continue;
        double dx = double(mono - x0);
        return mono + y0 + int64_t(std::llround(a + b * dx));
    }
}

int64_t PtpFollower::mono_from_ptp(int64_t ptp) const {
    for (;;) {
        uint32_t s0 = model_seq_.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        int64_t x0 = m_x0_.load(std::memory_order_relaxed);
        int64_t y0 = m_y0_.load(std::memory_order_relaxed);
        double a = m_a_.load(std::memory_order_relaxed);
        double b = m_b_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (model_seq_.load(std::memory_order_relaxed) != s0) continue;
        // ptp = mono + y0 + a + b*(mono - x0)  =>  solve for mono.
        double q = double(ptp - y0 - x0) - a;
        return x0 + int64_t(std::llround(q / (1.0 + b)));
    }
}

int64_t PtpFollower::now() const { return ptp_from_mono(mono_ns()); }

std::string PtpFollower::grandmaster() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.have_master ? st_.grandmaster : std::string();
}

PtpStatus PtpFollower::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

}  // namespace jvs
