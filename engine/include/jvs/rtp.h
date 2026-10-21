// RTP audio (RFC 3550 / 3190) as used by AES67: L16 or L24, big-endian,
// channels interleaved, timestamps locked to PTP.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "jvs/net.h"
#include "jvs/ptp.h"
#include "jvs/sdp.h"

namespace jvs {

struct RtpHeader {
    uint8_t payload_type = 96;
    bool marker = false;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
};

constexpr int kRtpHeaderLen = 12;
// Returns the payload offset, or -1 if this isn't a valid RTP packet.
int parse_rtp(const uint8_t* p, int len, RtpHeader& h);
void write_rtp(uint8_t* p, const RtpHeader& h);

// Sample conversion. dst/src are interleaved.
void decode_samples(const uint8_t* src, Encoding enc, int count, float* dst);
void encode_samples(const float* src, Encoding enc, int count, uint8_t* dst);

// The AES67 media clock: samples since the PTP epoch, plus the stream's offset.
// The low 32 bits are the RTP timestamp.
uint64_t media_time_from_ptp(int64_t ptp_ns, uint32_t rate, uint32_t offset);
int64_t ptp_from_media_time(uint64_t media_time, uint32_t rate, uint32_t offset);

// Ring buffer addressed by RTP timestamp. One writer and one reader thread.
class MediaRing {
public:
    MediaRing(int channels, int capacity_frames);
    int channels() const { return channels_; }
    void write(uint32_t ts, const float* interleaved, int frames);
    // Copies frames starting at ts. Frames that never arrived come back as
    // silence; returns how many frames were present.
    int read(uint32_t ts, float* interleaved, int frames) const;

private:
    int channels_;
    uint32_t mask_;
    std::vector<float> data_;
    std::unique_ptr<std::atomic<uint32_t>[]> stamp_;  // ts+1 that each slot holds (0 = empty)
};

struct RxStats {
    uint64_t packets = 0;
    uint64_t lost = 0;          // sequence gaps
    uint64_t bad = 0;           // wrong payload type / size
    uint32_t last_ts = 0;
    // How far behind its timestamp the newest packet arrived, in microseconds
    // (needs PTP lock). Tells you the minimum workable latency.
    double transit_us_avg = 0;
    double transit_us_max = 0;
};

class RtpReceiver {
public:
    RtpReceiver(const StreamDesc& d, int ring_frames = 32768);
    ~RtpReceiver();
    bool start(const NetInterface& iface, const PtpFollower* ptp);
    void stop();
    const StreamDesc& desc() const { return d_; }
    const MediaRing& ring() const { return ring_; }
    RxStats stats() const;

private:
    void loop();
    StreamDesc d_;
    MediaRing ring_;
    const PtpFollower* ptp_ = nullptr;
    UdpSocket sock_;
    std::thread th_;
    std::atomic<bool> run_{false};
    mutable std::atomic<uint64_t> packets_{0}, lost_{0}, bad_{0};
    std::atomic<uint32_t> last_ts_{0};
    std::atomic<double> transit_avg_{0}, transit_max_{0};
};

// Fills `frames` interleaved samples for the packet that starts at media_time.
using AudioSource = std::function<void(uint64_t media_time, float* interleaved, int frames)>;

// Test tones: channel n (1-based, counting from first_channel) plays
// 200 + 100*n Hz at -18 dBFS, so every channel can be told apart.
AudioSource make_test_tone(int first_channel, int channels, uint32_t rate = 48000);

struct TxStats {
    uint64_t packets = 0;
    uint64_t late = 0;      // packets sent more than one packet time late
    uint64_t resyncs = 0;
};

class RtpSender {
public:
    RtpSender(const StreamDesc& d, AudioSource src);
    ~RtpSender();
    bool start(const NetInterface& iface, const PtpFollower* ptp);
    void stop();
    const StreamDesc& desc() const { return d_; }
    TxStats stats() const;

private:
    void loop();
    StreamDesc d_;
    AudioSource src_;
    const PtpFollower* ptp_ = nullptr;
    UdpSocket sock_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> packets_{0}, late_{0}, resyncs_{0};
};

}  // namespace jvs
