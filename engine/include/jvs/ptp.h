// PTP follower for both clock versions Dante networks use:
//  - PTPv1 (IEEE 1588-2002): Dante's normal clock.
//  - PTPv2 (IEEE 1588-2008): AES67, and Dante devices in AES67 mode.
// Software timestamps only, which is what macOS gives us; accuracy is in the
// tens of microseconds, well inside a 1 ms packet.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "jvs/net.h"

namespace jvs {

using ClockIdentity = std::array<uint8_t, 8>;

std::string clock_identity_to_string(const ClockIdentity& id);  // "00-1D-C1-FF-FE-12-34-56"
ClockIdentity clock_identity_from_mac(const std::array<uint8_t, 6>& mac);

// ---- Wire format helpers (exposed for tests and the test master) ----
namespace ptpmsg {
enum Type : uint8_t { Sync = 0x0, DelayReq = 0x1, FollowUp = 0x8, DelayResp = 0x9, Announce = 0xB };

struct Header {
    uint8_t type = 0;
    uint8_t version = 2;
    uint16_t length = 0;
    uint8_t domain = 0;
    uint16_t flags = 0;
    int64_t correction_ns_x16 = 0;  // correctionField: ns * 2^16
    ClockIdentity src_clock{};
    uint16_t src_port = 0;
    uint16_t seq = 0;
    uint8_t control = 0;
    int8_t log_interval = 0;
    bool two_step() const { return (flags >> 8) & 0x02; }
};

struct Timestamp {
    uint64_t seconds = 0;  // 48 bits on the wire
    uint32_t nanoseconds = 0;
    int64_t to_ns() const { return int64_t(seconds) * 1000000000LL + nanoseconds; }
    static Timestamp from_ns(int64_t ns);
};

struct AnnounceBody {
    int16_t utc_offset = 37;
    uint8_t priority1 = 128;
    uint8_t clock_class = 248;
    uint8_t clock_accuracy = 0xFE;
    uint16_t variance = 0xFFFF;
    uint8_t priority2 = 128;
    ClockIdentity gm_identity{};
    uint16_t steps_removed = 0;
    uint8_t time_source = 0xA0;
};

constexpr int kHeaderLen = 34;
bool parse_header(const uint8_t* p, int len, Header& h);
void write_header(uint8_t* p, const Header& h);
Timestamp read_timestamp(const uint8_t* p);
void write_timestamp(uint8_t* p, const Timestamp& t);
bool parse_announce(const uint8_t* p, int len, AnnounceBody& a);
void write_announce(uint8_t* p, const AnnounceBody& a);  // writes body at p+34
}  // namespace ptpmsg

// PTPv1 (IEEE 1588-2002) wire format. Every message starts with a 40-byte header.
namespace ptpv1 {
enum Control : uint8_t { Sync = 0, DelayReq = 1, FollowUp = 2, DelayResp = 3 };
constexpr int kHeaderLen = 40;
constexpr int kSyncLen = 124;      // Sync and Delay_Req
constexpr int kFollowUpLen = 52;
constexpr int kDelayRespLen = 60;
constexpr uint16_t kFlagAssist = 0x0008;  // a Follow_Up carries the precise time

struct Header {
    uint8_t message_type = 1;  // 1 = event, 2 = general
    char subdomain[16] = {'_', 'D', 'F', 'L', 'T'};
    std::array<uint8_t, 6> uuid{};
    uint16_t port = 1;
    uint16_t seq = 0;
    uint8_t control = 0;
    uint16_t flags = 0;
};
struct SyncBody {
    int64_t origin_ns = 0;
    std::array<uint8_t, 6> gm_uuid{};
    uint8_t gm_stratum = 255;
    int16_t gm_variance = 0;
    bool gm_preferred = false;
    int8_t sync_interval = 0;
};
bool parse_header(const uint8_t* p, int len, Header& h);
void write_header(uint8_t* p, const Header& h);
int64_t read_timestamp(const uint8_t* p);  // 4-byte seconds + 4-byte ns
void write_timestamp(uint8_t* p, int64_t ns);
bool parse_sync(const uint8_t* p, int len, SyncBody& b);
void write_sync(uint8_t* p, const SyncBody& b);  // body of Sync/Delay_Req at p+40
}  // namespace ptpv1

// Which clock version to follow. Auto prefers PTPv1 (Dante's own clock) and
// uses PTPv2 when no PTPv1 clock is around.
enum class PtpMode { Auto, V1, V2 };

struct PtpStatus {
    bool running = false;
    bool have_master = false;
    bool locked = false;
    uint8_t domain = 0;
    int version = 0;              // version of the clock we follow (1 or 2)
    std::string grandmaster;      // clock identity of the grandmaster
    std::string master;           // clock identity of the port we follow
    uint32_t master_ip = 0;
    int64_t path_delay_ns = 0;
    double offset_jitter_ns = 0;  // RMS residual of the clock fit
    double rate_ppm = 0;          // our clock vs the master
    uint64_t syncs = 0;
    uint64_t delay_resps = 0;
};

class PtpFollower {
public:
    PtpFollower();
    ~PtpFollower();

    bool start(const NetInterface& iface, uint8_t domain = 0, PtpMode mode = PtpMode::Auto);
    void stop();

    // Convert between our monotonic clock and PTP time (TAI nanoseconds).
    // Only meaningful while status().locked is true.
    int64_t ptp_from_mono(int64_t mono) const;
    int64_t mono_from_ptp(int64_t ptp) const;
    int64_t now() const;
    bool locked() const { return locked_.load(); }
    // Grandmaster clock identity as used in SDP ts-refclk
    // ("00-1D-C1-FF-FE-12-34-56"). Empty if no master yet.
    std::string grandmaster() const;

    PtpStatus status() const;

    // The clock model as ptp = mono + shift + (mono - last_sync) * freq_scale,
    // the form the usrvclock protocol uses. Returns false until locked.
    bool overlay(int64_t& last_sync, int64_t& shift, double& freq_scale) const;

private:
    struct Master {
        uint8_t version = 2;
        ClockIdentity clock{};
        uint16_t port = 0;
        uint32_t ip = 0;
        ptpmsg::AnnounceBody announce;
        int64_t last_seen_mono = 0;
    };
    struct Sample {
        double x;  // mono - x0
        double y;  // (ptp - mono) - y0
    };

    void event_loop();
    void general_loop();
    void handle(const uint8_t* buf, int n, int64_t rx_mono, uint32_t src, bool event);
    void handle_v1(const uint8_t* buf, int n, int64_t rx_mono, uint32_t src);
    void on_delay_resp(int64_t t4);
    bool from_current(uint8_t version, const ClockIdentity& c, uint16_t port) const;
    void select_master(int64_t now);
    void on_sync_complete(int64_t t1_ptp, int64_t t2_mono, int64_t correction_ns);
    void maybe_send_delay_req(int64_t now);
    void fit_model();

    NetInterface iface_;
    uint8_t domain_ = 0;
    PtpMode mode_ = PtpMode::Auto;
    ClockIdentity my_identity_{};
    std::array<uint8_t, 6> my_uuid_{};
    UdpSocket ev_sock_, gen_sock_;
    std::thread ev_thread_, gen_thread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> locked_{false};

    mutable std::mutex mu_;
    std::vector<Master> masters_;
    int current_ = -1;  // index into masters_
    ClockIdentity last_clock_{};
    uint8_t last_version_ = 0;
    uint16_t last_port_ = 0;
    int jumps_ = 0;
    std::mt19937 rng_{std::random_device{}()};
    // Pending two-step sync
    uint16_t pending_seq_ = 0;
    int64_t pending_t2_ = 0;
    int64_t pending_corr_ = 0;
    bool pending_valid_ = false;
    // Delay measurement
    uint16_t dreq_seq_ = 0;
    int64_t dreq_t3_ = 0;
    bool dreq_outstanding_ = false;
    int64_t next_dreq_ = 0;
    int64_t dreq_interval_ns_ = 1000000000LL;
    int64_t path_delay_ = 0;
    bool have_delay_ = false;
    std::vector<int64_t> delays_;
    // Clock fit
    std::vector<Sample> samples_;
    bool have_origin_ = false;
    int64_t x0_ = 0, y0_ = 0;
    PtpStatus st_;

    // Model published to readers: ptp = mono + y0 + a + b*(mono - x0)
    // guarded by a sequence lock so audio threads never block.
    mutable std::atomic<uint32_t> model_seq_{0};
    std::atomic<int64_t> m_x0_{0}, m_y0_{0};
    std::atomic<double> m_a_{0}, m_b_{0};
};

}  // namespace jvs
