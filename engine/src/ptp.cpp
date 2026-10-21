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

namespace ptpv1 {

bool parse_header(const uint8_t* p, int len, Header& h) {
    if (len < kHeaderLen) return false;
    if (rd16(p) != 1) return false;  // versionPTP
    std::memcpy(h.subdomain, p + 4, 16);
    h.message_type = p[20];
    std::memcpy(h.uuid.data(), p + 22, 6);
    h.port = rd16(p + 28);
    h.seq = rd16(p + 30);
    h.control = p[32];
    h.flags = rd16(p + 34);
    return true;
}

void write_header(uint8_t* p, const Header& h) {
    std::memset(p, 0, kHeaderLen);
    wr16(p, 1);      // versionPTP
    wr16(p + 2, 1);  // versionNetwork
    std::memcpy(p + 4, h.subdomain, 16);
    p[20] = h.message_type;
    p[21] = 1;  // Ethernet
    std::memcpy(p + 22, h.uuid.data(), 6);
    wr16(p + 28, h.port);
    wr16(p + 30, h.seq);
    p[32] = h.control;
    wr16(p + 34, h.flags);
}

int64_t read_timestamp(const uint8_t* p) {
    uint32_t sec = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    int32_t ns = int32_t(uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7]);
    return int64_t(sec) * 1000000000LL + ns;
}

void write_timestamp(uint8_t* p, int64_t ns) {
    uint32_t sec = uint32_t(ns / 1000000000LL);
    uint32_t n = uint32_t(ns % 1000000000LL);
    for (int i = 0; i < 4; ++i) {
        p[i] = uint8_t(sec >> (24 - 8 * i));
        p[4 + i] = uint8_t(n >> (24 - 8 * i));
    }
}

bool parse_sync(const uint8_t* p, int len, SyncBody& b) {
    if (len < kSyncLen) return false;
    b.origin_ns = read_timestamp(p + 40);
    std::memcpy(b.gm_uuid.data(), p + 54, 6);
    b.gm_stratum = p[67];
    b.gm_variance = int16_t(rd16(p + 74));
    b.gm_preferred = p[77] != 0;
    b.sync_interval = int8_t(p[83]);
    return true;
}

void write_sync(uint8_t* p, const SyncBody& b) {
    std::memset(p + kHeaderLen, 0, kSyncLen - kHeaderLen);
    write_timestamp(p + 40, b.origin_ns);
    p[53] = 1;
    std::memcpy(p + 54, b.gm_uuid.data(), 6);
    wr16(p + 60, 1);
    p[67] = b.gm_stratum;
    std::memcpy(p + 68, "DFLT", 4);
    wr16(p + 74, uint16_t(b.gm_variance));
    p[77] = b.gm_preferred ? 1 : 0;
    p[83] = uint8_t(b.sync_interval);
    p[95] = b.gm_stratum;
    std::memcpy(p + 96, "DFLT", 4);
    p[101] = 1;
    std::memcpy(p + 102, b.gm_uuid.data(), 6);
    wr16(p + 110, 1);
}

}  // namespace ptpv1

using namespace ptpmsg;

namespace {
ClockIdentity identity_from_uuid(const std::array<uint8_t, 6>& u) {
    return {u[0], u[1], u[2], u[3], u[4], u[5], 0, 0};
}
std::string uuid_to_string(const ClockIdentity& id) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1], id[2], id[3], id[4], id[5]);
    return buf;
}
}  // namespace

PtpFollower::PtpFollower() = default;
PtpFollower::~PtpFollower() { stop(); }

bool PtpFollower::start(const NetInterface& iface, uint8_t domain, PtpMode mode) {
    stop();
    iface_ = iface;
    domain_ = domain;
    mode_ = mode;
    if (iface.has_mac) {
        my_uuid_ = iface.mac;
    } else {
        std::random_device rd;
        for (auto& b : my_uuid_) b = uint8_t(rd());
        my_uuid_[0] &= 0xFE;
    }
    my_identity_ = clock_identity_from_mac(my_uuid_);
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
    // Real-time so clock messages are handled promptly even when macOS
    // throttles the process.
    ev_thread_ = std::thread([this] {
        set_thread_realtime(5000, 500);
        event_loop();
    });
    gen_thread_ = std::thread([this] {
        set_thread_realtime(5000, 500);
        general_loop();
    });
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
    if (n >= 2 && buf[0] == 0 && buf[1] == 1) {
        if (mode_ != PtpMode::V2) handle_v1(buf, n, rx_mono, src);
        return;
    }
    if (mode_ == PtpMode::V1) return;
    Header h;
    if (!parse_header(buf, n, h)) return;
    if (h.domain != domain_) return;
    if (h.src_clock == my_identity_) return;  // our own Delay_Req looped back

    std::lock_guard<std::mutex> lk(mu_);
    int64_t now = mono_ns();

    if (h.type == Announce) {
        AnnounceBody a;
        if (!parse_announce(buf, n, a)) return;
        auto it = std::find_if(masters_.begin(), masters_.end(), [&](const Master& m) {
            return m.version == 2 && m.clock == h.src_clock && m.port == h.src_port;
        });
        if (it == masters_.end()) {
            masters_.push_back({2, h.src_clock, h.src_port, src, a, now});
        } else {
            it->announce = a;
            it->ip = src;
            it->last_seen_mono = now;
        }
        select_master(now);
        return;
    }

    if (!from_current(2, h.src_clock, h.src_port)) return;

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
        on_delay_resp(read_timestamp(buf + 34).to_ns() - corr);
    }
}

bool PtpFollower::from_current(uint8_t version, const ClockIdentity& c, uint16_t port) const {
    if (current_ < 0) return false;
    const Master& m = masters_[size_t(current_)];
    return m.version == version && m.clock == c && m.port == port;
}

void PtpFollower::on_delay_resp(int64_t t4) {
    if (samples_.size() < 2) return;
    double m3 = double(dreq_t3_ - x0_);
    double a = m_a_.load(), b = m_b_.load();
    // Model without path delay at t3 (see fit_model()).
    int64_t no_delay = dreq_t3_ + y0_ + int64_t(std::llround(a - double(path_delay_) + b * m3));
    int64_t d = (t4 - no_delay) / 2;
    if (d < 0 || d > 10000000) return;  // nonsense, ignore
    // Median of the last few measurements: one late packet shouldn't move it.
    delays_.push_back(d);
    if (delays_.size() > 9) delays_.erase(delays_.begin());
    std::vector<int64_t> sorted = delays_;
    std::sort(sorted.begin(), sorted.end());
    path_delay_ = sorted[sorted.size() / 2];
    have_delay_ = true;
    st_.delay_resps++;
    st_.path_delay_ns = path_delay_;
    fit_model();
}

void PtpFollower::handle_v1(const uint8_t* buf, int n, int64_t rx_mono, uint32_t src) {
    ptpv1::Header h;
    if (!ptpv1::parse_header(buf, n, h)) return;
    if (domain_ == 0 && std::strncmp(h.subdomain, "_DFLT", 16) != 0) return;
    if (h.uuid == my_uuid_) return;  // our own Delay_Req looped back
    ClockIdentity id = identity_from_uuid(h.uuid);

    std::lock_guard<std::mutex> lk(mu_);
    int64_t now = mono_ns();

    if (h.control == ptpv1::Sync) {
        ptpv1::SyncBody b;
        if (!ptpv1::parse_sync(buf, n, b)) return;
        // PTPv1 has no Announce: every Sync also describes its clock.
        AnnounceBody a;
        a.priority1 = b.gm_preferred ? 0 : 128;
        a.clock_class = b.gm_stratum;
        a.clock_accuracy = 0;
        a.variance = uint16_t(int32_t(b.gm_variance) + 32768);
        a.gm_identity = identity_from_uuid(b.gm_uuid);
        auto it = std::find_if(masters_.begin(), masters_.end(), [&](const Master& m) {
            return m.version == 1 && m.clock == id && m.port == h.port;
        });
        if (it == masters_.end()) {
            masters_.push_back({1, id, h.port, src, a, now});
        } else {
            it->announce = a;
            it->ip = src;
            it->last_seen_mono = now;
        }
        select_master(now);
        if (!from_current(1, id, h.port)) return;
        if (h.flags & ptpv1::kFlagAssist) {
            pending_seq_ = h.seq;
            pending_t2_ = rx_mono;
            pending_corr_ = 0;
            pending_valid_ = true;
        } else {
            on_sync_complete(b.origin_ns, rx_mono, 0);
        }
    } else if (h.control == ptpv1::FollowUp) {
        if (n < ptpv1::kFollowUpLen || !from_current(1, id, h.port)) return;
        uint16_t assoc = rd16(buf + 42);
        if (!pending_valid_ || assoc != pending_seq_) return;
        pending_valid_ = false;
        on_sync_complete(ptpv1::read_timestamp(buf + 44), pending_t2_, 0);
    } else if (h.control == ptpv1::DelayResp) {
        if (n < ptpv1::kDelayRespLen || !from_current(1, id, h.port)) return;
        std::array<uint8_t, 6> req;
        std::memcpy(req.data(), buf + 50, 6);
        if (req != my_uuid_ || !dreq_outstanding_ || rd16(buf + 58) != dreq_seq_) return;
        dreq_outstanding_ = false;
        on_delay_resp(ptpv1::read_timestamp(buf + 40));
    }
}

void PtpFollower::select_master(int64_t now) {
    masters_.erase(std::remove_if(masters_.begin(), masters_.end(),
                                  [&](const Master& m) { return now - m.last_seen_mono > kMasterTimeoutNs; }),
                   masters_.end());
    int best = -1;
    auto key = [](const Master& m) {
        const auto& a = m.announce;
        // PTPv1 first: it's the clock every Dante device follows.
        return std::make_tuple(m.version, a.priority1, a.clock_class, a.clock_accuracy, a.variance, a.priority2,
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
    if (!st_.have_master || m.version != last_version_ || m.clock != last_clock_ || m.port != last_port_) {
        // New master: start the clock fit again.
        samples_.clear();
        have_origin_ = false;
        have_delay_ = false;
        delays_.clear();
        pending_valid_ = false;
        locked_ = false;
        next_dreq_ = 0;
    }
    last_clock_ = m.clock;
    last_port_ = m.port;
    last_version_ = m.version;
    st_.version = m.version;
    st_.have_master = true;
    st_.master = m.version == 1 ? uuid_to_string(m.clock) : clock_identity_to_string(m.clock);
    st_.grandmaster =
        m.version == 1 ? uuid_to_string(m.announce.gm_identity) : clock_identity_to_string(m.announce.gm_identity);
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
    uint32_t group;
    parse_ipv4(kPtpGroup, group);
    if (masters_[size_t(current_)].version == 1) {
        uint8_t b1[ptpv1::kSyncLen];
        ptpv1::Header h1;
        h1.message_type = 1;
        h1.uuid = my_uuid_;
        h1.port = 1;
        h1.seq = ++dreq_seq_;
        h1.control = ptpv1::DelayReq;
        ptpv1::write_header(b1, h1);
        ptpv1::SyncBody body;
        body.gm_uuid = my_uuid_;
        ptpv1::write_sync(b1, body);
        dreq_t3_ = mono_ns();
        dreq_outstanding_ = ev_sock_.send_to(b1, sizeof b1, group, kEventPort);
        return;
    }
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

bool PtpFollower::overlay(int64_t& last_sync, int64_t& shift, double& freq_scale) const {
    if (!locked_) return false;
    for (;;) {
        uint32_t s0 = model_seq_.load(std::memory_order_acquire);
        if (s0 & 1) continue;
        int64_t x0 = m_x0_.load(std::memory_order_relaxed);
        int64_t y0 = m_y0_.load(std::memory_order_relaxed);
        double a = m_a_.load(std::memory_order_relaxed);
        double b = m_b_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (model_seq_.load(std::memory_order_relaxed) != s0) continue;
        last_sync = x0;
        shift = y0 + int64_t(std::llround(a));
        freq_scale = b;
        return true;
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
