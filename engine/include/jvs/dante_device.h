// A Dante device on the network: our PTP clock + Inferno (via the Rust bridge).
// Audio is exchanged by media time (PTP clock in samples).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jvs/clock_server.h"
#include "jvs/net.h"
#include "jvs/ptp.h"

struct Jvsd;

namespace jvs {

struct DanteConfig {
    std::string name = "Jose Virtual Soundcard";
    std::string iface;           // network port, e.g. "en0"
    int rx_channels = 64;
    int tx_channels = 64;
    int sample_rate = 48000;
    int rx_latency_us = 10000;   // like the latency setting in Dante Controller
    int tx_latency_us = 10000;
    PtpMode ptp_mode = PtpMode::Auto;
    int process_id = 0;          // >= 1 to run several devices on one IP (tests)
    int alt_port = 0;
    // Another component (the Core Audio driver) writes the audio to send.
    // The device then only clears what has been sent, so a stopped writer
    // leads to silence instead of a repeating buffer.
    bool external_tx = false;
};

enum class DanteState { Stopped, WaitingForClock, Starting, Running, Failed };

class DanteDevice {
public:
    DanteDevice();
    ~DanteDevice();

    bool start(const DanteConfig& cfg, std::string* error = nullptr);
    void stop();

    DanteState state() const { return state_.load(); }
    const DanteConfig& config() const { return cfg_; }
    PtpStatus clock_status() const { return ptp_.status(); }
    const PtpFollower& ptp() const { return ptp_; }

    // Current media time in samples, or false if the clock isn't locked.
    bool now(uint64_t& media_time) const;

    // Received audio for [t, t+frames). Read only up to now(); older than
    // about buffer size is gone. out is interleaved, rx_channels wide.
    bool read_rx(uint64_t t, int frames, float* out) const;
    // Audio to send for [t, t+frames). Write ahead of now(), by less than the
    // buffer size. in is interleaved, tx_channels wide.
    bool write_tx(uint64_t t, int frames, const float* in);

    int buffer_frames() const { return buffer_frames_; }
    // Monotonic time (ns) at which media time t happens. False if unlocked.
    bool mono_from_media(uint64_t t, int64_t& mono_ns) const;
    // UDP port Dante Controller uses for routing (4440 unless it was taken).
    int arc_port() const { return arc_port_; }

    // Built-in test signal: tones on every transmit channel (channel n plays
    // 200 + 100*n Hz at -18 dBFS).
    void set_test_tones(bool on) { tones_ = on; }
    bool test_tones() const { return tones_; }

    // Route another device's channel to one of our receive channels (0-based).
    bool subscribe(int local_channel, const std::string& tx_channel, const std::string& tx_device);

private:
    void worker();
    int64_t index(uint64_t t) const;

    DanteConfig cfg_;
    PtpFollower ptp_;
    ClockServer clock_server_;
    Jvsd* jvsd_ = nullptr;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<DanteState> state_{DanteState::Stopped};
    std::atomic<bool> tones_{false};
    std::atomic<bool> audio_{false};
    uint64_t start_time_ = 0;
    int buffer_frames_ = 32768;
    int arc_port_ = 4440;
    int32_t* tx_ = nullptr;
    const int32_t* rx_ = nullptr;
};

}  // namespace jvs
