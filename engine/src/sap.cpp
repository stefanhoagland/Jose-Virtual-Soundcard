#include "jvs/sap.h"
#include "jvs/clock.h"

#include <algorithm>
#include <cstring>

namespace jvs {

namespace {
constexpr uint16_t kSapPort = 9875;
constexpr const char* kSapGroups[] = {"239.255.255.255", "224.2.127.254"};
constexpr int64_t kAnnounceIntervalNs = 30LL * 1000000000LL;
constexpr int64_t kStreamTimeoutNs = 10LL * 60 * 1000000000LL;  // RFC 2974: 10 announcements or 1 hour

uint16_t hash16(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return uint16_t(h ^ (h >> 16));
}
}  // namespace

bool parse_sap(const uint8_t* p, int len, SapPacket& out) {
    if (len < 8) return false;
    uint8_t b0 = p[0];
    int version = b0 >> 5;
    bool ipv6 = b0 & 0x10;
    out.deletion = b0 & 0x04;
    bool encrypted = b0 & 0x02;
    bool compressed = b0 & 0x01;
    if (version != 1 || ipv6 || encrypted || compressed) return false;
    int auth_len = p[1] * 4;
    out.msg_id_hash = uint16_t(p[2] << 8 | p[3]);
    std::memcpy(&out.origin_ip, p + 4, 4);
    int off = 8 + auth_len;
    if (off > len) return false;
    // Optional payload type, NUL-terminated, unless the payload starts with "v=0".
    out.payload_type = "application/sdp";
    if (!(len - off >= 3 && std::memcmp(p + off, "v=0", 3) == 0)) {
        const void* z = std::memchr(p + off, 0, size_t(len - off));
        if (!z) return false;
        int zi = int(static_cast<const uint8_t*>(z) - p);
        out.payload_type.assign(reinterpret_cast<const char*>(p + off), size_t(zi - off));
        off = zi + 1;
    }
    out.sdp.assign(reinterpret_cast<const char*>(p + off), size_t(len - off));
    return true;
}

std::vector<uint8_t> build_sap(const SapPacket& s) {
    std::vector<uint8_t> v(8);
    v[0] = uint8_t(1 << 5 | (s.deletion ? 0x04 : 0));
    v[1] = 0;
    v[2] = uint8_t(s.msg_id_hash >> 8);
    v[3] = uint8_t(s.msg_id_hash);
    std::memcpy(v.data() + 4, &s.origin_ip, 4);
    v.insert(v.end(), s.payload_type.begin(), s.payload_type.end());
    v.push_back(0);
    v.insert(v.end(), s.sdp.begin(), s.sdp.end());
    return v;
}

// ---------------- Listener ----------------

SapListener::~SapListener() { stop(); }

bool SapListener::start(const NetInterface& iface) {
    stop();
    if (!sock_.open(kSapPort)) return false;
    bool joined = false;
    for (auto g : kSapGroups) {
        uint32_t ip;
        parse_ipv4(g, ip);
        joined |= sock_.join(ip, iface.ipv4);
    }
    if (!joined) return false;
    sock_.set_recv_timeout_ms(200);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void SapListener::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    sock_.close();
}

void SapListener::loop() {
    std::vector<uint8_t> buf(4096);
    while (run_) {
        int n = sock_.recv(buf.data(), int(buf.size()));
        int64_t now = mono_ns();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (now - it->second.last_seen_mono > kStreamTimeoutNs)
                it = streams_.erase(it);
            else
                ++it;
        }
        if (n <= 0) continue;
        SapPacket pk;
        if (!parse_sap(buf.data(), n, pk) || pk.payload_type != "application/sdp") continue;
        StreamDesc d;
        if (!parse_sdp(pk.sdp, d)) continue;
        if (pk.deletion) {
            streams_.erase(d.key());
            continue;
        }
        auto& e = streams_[d.key()];
        e.desc = d;
        e.sdp = pk.sdp;
        e.last_seen_mono = now;
    }
}

std::vector<DiscoveredStream> SapListener::streams() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DiscoveredStream> v;
    for (auto& [k, s] : streams_) v.push_back(s);
    return v;
}

// ---------------- Announcer ----------------

SapAnnouncer::~SapAnnouncer() { stop(); }

bool SapAnnouncer::start(const NetInterface& iface) {
    stop();
    iface_ = iface;
    if (!sock_.open(0)) return false;
    sock_.set_multicast_out(iface.ipv4, 255, true);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void SapAnnouncer::stop() {
    if (!run_) return;
    run_ = false;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [k, d] : streams_) send_one(d, true);
    streams_.clear();
    sock_.close();
}

void SapAnnouncer::set(const StreamDesc& d) {
    std::lock_guard<std::mutex> lk(mu_);
    streams_[d.key()] = d;
    send_one(d, false);  // announce right away
}

void SapAnnouncer::remove(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(key);
    if (it == streams_.end()) return;
    send_one(it->second, true);
    streams_.erase(it);
}

void SapAnnouncer::send_one(const StreamDesc& d, bool deletion) {
    SapPacket p;
    p.deletion = deletion;
    p.origin_ip = iface_.ipv4;
    p.sdp = build_sdp(d);
    p.msg_id_hash = hash16(d.session_id + "/" + std::to_string(d.session_version));
    auto bytes = build_sap(p);
    uint32_t g;
    parse_ipv4(kSapGroups[0], g);
    sock_.send_to(bytes.data(), int(bytes.size()), g, kSapPort);
}

void SapAnnouncer::loop() {
    int64_t next = mono_ns() + kAnnounceIntervalNs;
    while (run_) {
        sleep_until_mono(std::min<int64_t>(next, mono_ns() + 200000000LL));
        if (mono_ns() < next) continue;
        next += kAnnounceIntervalNs;
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [k, d] : streams_) send_one(d, false);
    }
}

}  // namespace jvs
