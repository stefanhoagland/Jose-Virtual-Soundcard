#include "jvs/rtp.h"
#include "jvs/clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace jvs {

int parse_rtp(const uint8_t* p, int len, RtpHeader& h) {
    if (len < kRtpHeaderLen) return -1;
    if ((p[0] >> 6) != 2) return -1;
    bool padding = p[0] & 0x20;
    bool ext = p[0] & 0x10;
    int cc = p[0] & 0x0F;
    h.marker = p[1] & 0x80;
    h.payload_type = p[1] & 0x7F;
    h.seq = uint16_t(p[2] << 8 | p[3]);
    h.timestamp = uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7];
    h.ssrc = uint32_t(p[8]) << 24 | uint32_t(p[9]) << 16 | uint32_t(p[10]) << 8 | p[11];
    int off = kRtpHeaderLen + cc * 4;
    if (ext) {
        if (off + 4 > len) return -1;
        int words = p[off + 2] << 8 | p[off + 3];
        off += 4 + words * 4;
    }
    int end = len;
    if (padding) end -= p[len - 1];
    if (off > end) return -1;
    return off;
}

void write_rtp(uint8_t* p, const RtpHeader& h) {
    p[0] = 0x80;
    p[1] = uint8_t((h.marker ? 0x80 : 0) | (h.payload_type & 0x7F));
    p[2] = uint8_t(h.seq >> 8);
    p[3] = uint8_t(h.seq);
    p[4] = uint8_t(h.timestamp >> 24);
    p[5] = uint8_t(h.timestamp >> 16);
    p[6] = uint8_t(h.timestamp >> 8);
    p[7] = uint8_t(h.timestamp);
    p[8] = uint8_t(h.ssrc >> 24);
    p[9] = uint8_t(h.ssrc >> 16);
    p[10] = uint8_t(h.ssrc >> 8);
    p[11] = uint8_t(h.ssrc);
}

void decode_samples(const uint8_t* s, Encoding enc, int count, float* d) {
    if (enc == Encoding::L24) {
        for (int i = 0; i < count; ++i, s += 3) {
            int32_t v = int32_t(uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8) >> 8;
            d[i] = float(v) * (1.0f / 8388608.0f);
        }
    } else {
        for (int i = 0; i < count; ++i, s += 2) {
            int16_t v = int16_t(uint16_t(s[0] << 8 | s[1]));
            d[i] = float(v) * (1.0f / 32768.0f);
        }
    }
}

void encode_samples(const float* s, Encoding enc, int count, uint8_t* d) {
    if (enc == Encoding::L24) {
        for (int i = 0; i < count; ++i, d += 3) {
            float f = std::clamp(s[i], -1.0f, 1.0f);
            int32_t v = int32_t(std::lrint(double(f) * 8388607.0));
            d[0] = uint8_t(v >> 16);
            d[1] = uint8_t(v >> 8);
            d[2] = uint8_t(v);
        }
    } else {
        for (int i = 0; i < count; ++i, d += 2) {
            float f = std::clamp(s[i], -1.0f, 1.0f);
            int32_t v = int32_t(std::lrint(double(f) * 32767.0));
            d[0] = uint8_t(v >> 8);
            d[1] = uint8_t(v);
        }
    }
}

uint64_t media_time_from_ptp(int64_t ptp_ns, uint32_t rate, uint32_t offset) {
    if (ptp_ns < 0) ptp_ns = 0;
    uint64_t sec = uint64_t(ptp_ns / 1000000000LL);
    uint64_t rem = uint64_t(ptp_ns % 1000000000LL);
    return sec * rate + rem * rate / 1000000000ULL + offset;
}

int64_t ptp_from_media_time(uint64_t mt, uint32_t rate, uint32_t offset) {
    mt -= offset;
    uint64_t sec = mt / rate;
    uint64_t rem = mt % rate;
    return int64_t(sec * 1000000000ULL + (rem * 1000000000ULL + rate - 1) / rate);
}

// ---------------- MediaRing ----------------

MediaRing::MediaRing(int channels, int capacity_frames) : channels_(channels) {
    uint32_t cap = 1;
    while (cap < uint32_t(capacity_frames)) cap <<= 1;
    mask_ = cap - 1;
    data_.assign(size_t(cap) * size_t(channels), 0.0f);
    stamp_.reset(new std::atomic<uint32_t>[cap]);
    for (uint32_t i = 0; i < cap; ++i) stamp_[i].store(0, std::memory_order_relaxed);
}

void MediaRing::write(uint32_t ts, const float* in, int frames) {
    for (int i = 0; i < frames; ++i) {
        uint32_t t = ts + uint32_t(i);
        uint32_t slot = t & mask_;
        stamp_[slot].store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&data_[size_t(slot) * size_t(channels_)], in + size_t(i) * size_t(channels_),
                    sizeof(float) * size_t(channels_));
        stamp_[slot].store(t + 1, std::memory_order_release);
    }
}

int MediaRing::read(uint32_t ts, float* out, int frames) const {
    int present = 0;
    for (int i = 0; i < frames; ++i) {
        uint32_t t = ts + uint32_t(i);
        uint32_t slot = t & mask_;
        float* o = out + size_t(i) * size_t(channels_);
        if (stamp_[slot].load(std::memory_order_acquire) == t + 1) {
            std::memcpy(o, &data_[size_t(slot) * size_t(channels_)], sizeof(float) * size_t(channels_));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (stamp_[slot].load(std::memory_order_relaxed) == t + 1) {
                ++present;
                continue;
            }
        }
        std::fill(o, o + channels_, 0.0f);
    }
    return present;
}

// ---------------- Receiver ----------------

RtpReceiver::RtpReceiver(const StreamDesc& d, int ring_frames) : d_(d), ring_(d.channels, ring_frames) {}
RtpReceiver::~RtpReceiver() { stop(); }

bool RtpReceiver::start(const NetInterface& iface, const PtpFollower* ptp) {
    stop();
    ptp_ = ptp;
    if (!sock_.open(d_.port, d_.dest_ip)) return false;
    if (!sock_.join(d_.dest_ip, iface.ipv4)) return false;
    sock_.set_buffer_sizes(1 << 20);
    sock_.enable_rx_timestamps();
    sock_.set_recv_timeout_ms(100);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void RtpReceiver::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    sock_.close();
}

void RtpReceiver::loop() {
    std::vector<uint8_t> buf(9000);
    std::vector<float> samples;
    bool have_seq = false;
    uint16_t expect = 0;
    const int bps = d_.bytes_per_sample();
    while (run_) {
        int64_t rx = 0;
        int n = sock_.recv(buf.data(), int(buf.size()), &rx);
        if (n <= 0) continue;
        RtpHeader h;
        int off = parse_rtp(buf.data(), n, h);
        if (off < 0 || h.payload_type != d_.payload_type) {
            bad_++;
            continue;
        }
        int bytes = n - off;
        int frame_bytes = bps * d_.channels;
        if (bytes <= 0 || bytes % frame_bytes != 0) {
            bad_++;
            continue;
        }
        int frames = bytes / frame_bytes;
        if (have_seq && h.seq != expect) {
            int16_t gap = int16_t(h.seq - expect);
            if (gap > 0) lost_ += uint64_t(gap);
        }
        have_seq = true;
        expect = uint16_t(h.seq + 1);
        samples.resize(size_t(frames) * size_t(d_.channels));
        decode_samples(buf.data() + off, d_.encoding, frames * d_.channels, samples.data());
        ring_.write(h.timestamp, samples.data(), frames);
        packets_++;
        last_ts_ = h.timestamp;

        if (ptp_ && ptp_->locked()) {
            uint32_t now_ts =
                uint32_t(media_time_from_ptp(ptp_->ptp_from_mono(rx), d_.sample_rate, d_.media_clock_offset));
            int32_t behind = int32_t(now_ts - (h.timestamp + uint32_t(frames)));
            double us = double(behind) * 1e6 / double(d_.sample_rate);
            double avg = transit_avg_.load();
            transit_avg_ = avg == 0 ? us : avg + (us - avg) * 0.01;
            if (us > transit_max_.load()) transit_max_ = us;
        }
    }
}

RxStats RtpReceiver::stats() const {
    RxStats s;
    s.packets = packets_;
    s.lost = lost_;
    s.bad = bad_;
    s.last_ts = last_ts_;
    s.transit_us_avg = transit_avg_;
    s.transit_us_max = transit_max_;
    return s;
}

// ---------------- Sender ----------------

RtpSender::RtpSender(const StreamDesc& d, AudioSource src) : d_(d), src_(std::move(src)) {}
RtpSender::~RtpSender() { stop(); }

bool RtpSender::start(const NetInterface& iface, const PtpFollower* ptp) {
    stop();
    ptp_ = ptp;
    if (!sock_.open(0)) return false;
    sock_.set_multicast_out(iface.ipv4, 32, true);
    sock_.set_buffer_sizes(1 << 20);
    sock_.set_dscp(34);  // AES67 default for media (AF41)
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void RtpSender::stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    sock_.close();
}

void RtpSender::loop() {
    const int fpp = d_.frames_per_packet();
    const int ch = d_.channels;
    std::vector<float> pcm(size_t(fpp) * size_t(ch));
    std::vector<uint8_t> pkt(size_t(kRtpHeaderLen) + size_t(fpp) * size_t(ch) * size_t(d_.bytes_per_sample()));
    RtpHeader h;
    h.payload_type = d_.payload_type;
    std::mt19937 rng{std::random_device{}()};
    h.ssrc = rng();
    h.seq = uint16_t(rng());
    uint64_t next_mt = 0;
    bool started = false;
    const int64_t packet_ns = int64_t(d_.ptime_us) * 1000;

    while (run_) {
        if (!ptp_ || !ptp_->locked()) {
            started = false;
            sleep_until_mono(mono_ns() + 10000000);
            continue;
        }
        uint64_t now_mt = media_time_from_ptp(ptp_->now(), d_.sample_rate, d_.media_clock_offset);
        if (!started) {
            // Start on a packet boundary, one packet in the past.
            next_mt = (now_mt / uint64_t(fpp)) * uint64_t(fpp) - uint64_t(fpp);
            started = true;
        }
        // A packet holding samples [mt, mt+fpp) goes out once they have "happened".
        uint64_t due_mt = next_mt + uint64_t(fpp);
        if (now_mt < due_mt) {
            int64_t due_ptp = ptp_from_media_time(due_mt, d_.sample_rate, d_.media_clock_offset);
            int64_t wake = ptp_->mono_from_ptp(due_ptp);
            // Don't oversleep on a model update.
            sleep_until_mono(std::min<int64_t>(wake, mono_ns() + 4 * packet_ns));
            continue;
        }
        if (now_mt - due_mt > uint64_t(fpp) * 20) {
            // Far behind (machine stalled, or clock stepped): skip ahead.
            resyncs_++;
            started = false;
            continue;
        }
        if (now_mt - due_mt > uint64_t(fpp)) late_++;
        src_(next_mt, pcm.data(), fpp);
        h.timestamp = uint32_t(next_mt);
        write_rtp(pkt.data(), h);
        encode_samples(pcm.data(), d_.encoding, fpp * ch, pkt.data() + kRtpHeaderLen);
        sock_.send_to(pkt.data(), int(pkt.size()), d_.dest_ip, d_.port);
        packets_++;
        h.seq++;
        next_mt += uint64_t(fpp);
    }
}

TxStats RtpSender::stats() const {
    TxStats s;
    s.packets = packets_;
    s.late = late_;
    s.resyncs = resyncs_;
    return s;
}

}  // namespace jvs
