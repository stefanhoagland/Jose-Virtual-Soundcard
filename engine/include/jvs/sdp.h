// AES67 stream descriptions (SDP, RFC 4566 / AES67 Annex).
#pragma once
#include <cstdint>
#include <string>

namespace jvs {

enum class Encoding { L16, L24 };

struct StreamDesc {
    std::string name;            // s=
    std::string session_id;      // o= session id
    uint64_t session_version = 0;
    uint32_t origin_ip = 0;      // o= address (network byte order)
    uint32_t dest_ip = 0;        // c= multicast group (network byte order)
    uint16_t port = 5004;
    uint8_t payload_type = 96;
    Encoding encoding = Encoding::L24;
    uint32_t sample_rate = 48000;
    int channels = 2;
    uint32_t ptime_us = 1000;     // packet time
    uint32_t media_clock_offset = 0;  // a=mediaclk:direct=
    std::string ptp_gmid;        // a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>
    int ptp_domain = 0;

    int bytes_per_sample() const { return encoding == Encoding::L24 ? 3 : 2; }
    int frames_per_packet() const { return int(uint64_t(sample_rate) * ptime_us / 1000000); }
    // A short key that identifies the stream (origin + session id).
    std::string key() const;
};

// Parse an SDP text. Returns false if it isn't a usable AES67 audio stream
// (e.g. not L16/L24, or no multicast address).
bool parse_sdp(const std::string& text, StreamDesc& out, std::string* why = nullptr);

// Build an SDP text for a stream we send.
std::string build_sdp(const StreamDesc& s);

}  // namespace jvs
