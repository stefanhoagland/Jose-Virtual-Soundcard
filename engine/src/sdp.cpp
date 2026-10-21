#include "jvs/sdp.h"
#include "jvs/net.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace jvs {

namespace {
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool parse_conn(const std::string& v, uint32_t& ip) {
    // "IN IP4 239.69.1.2/32"
    auto parts = split(v, ' ');
    if (parts.size() < 3 || parts[0] != "IN" || parts[1] != "IP4") return false;
    std::string addr = parts[2].substr(0, parts[2].find('/'));
    return parse_ipv4(addr, ip);
}
}  // namespace

std::string StreamDesc::key() const { return ipv4_to_string(origin_ip) + "/" + session_id; }

bool parse_sdp(const std::string& text, StreamDesc& s, std::string* why) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    s = StreamDesc{};
    bool in_media = false, have_media = false, have_rtpmap = false;
    uint32_t session_c = 0, media_c = 0;
    int ptime_set = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2 || line[1] != '=') continue;
        char t = line[0];
        std::string v = line.substr(2);
        switch (t) {
            case 'o': {
                auto p = split(v, ' ');
                if (p.size() >= 6) {
                    s.session_id = p[1];
                    s.session_version = std::strtoull(p[2].c_str(), nullptr, 10);
                    parse_ipv4(p[5], s.origin_ip);
                }
                break;
            }
            case 's':
                s.name = v;
                break;
            case 'c':
                parse_conn(v, in_media ? media_c : session_c);
                break;
            case 'm': {
                if (have_media) {
                    in_media = false;  // only the first audio stream is used
                    break;
                }
                auto p = split(v, ' ');
                if (p.size() >= 4 && p[0] == "audio") {
                    in_media = have_media = true;
                    s.port = uint16_t(std::atoi(p[1].c_str()));
                    s.payload_type = uint8_t(std::atoi(p[3].c_str()));
                }
                break;
            }
            case 'a': {
                if (starts_with(v, "rtpmap:")) {
                    // rtpmap:96 L24/48000/8
                    auto sp = v.find(' ');
                    if (sp == std::string::npos) break;
                    int pt = std::atoi(v.c_str() + 7);
                    if (pt != s.payload_type) break;
                    auto p = split(v.substr(sp + 1), '/');
                    if (p.size() < 2) break;
                    if (p[0] == "L24")
                        s.encoding = Encoding::L24;
                    else if (p[0] == "L16")
                        s.encoding = Encoding::L16;
                    else
                        return fail("unsupported encoding (only L16/L24)");
                    s.sample_rate = uint32_t(std::atoi(p[1].c_str()));
                    s.channels = p.size() >= 3 ? std::atoi(p[2].c_str()) : 1;
                    have_rtpmap = true;
                } else if (starts_with(v, "ptime:")) {
                    double ms = std::atof(v.c_str() + 6);
                    s.ptime_us = uint32_t(ms * 1000.0 + 0.5);
                    ptime_set = 1;
                } else if (starts_with(v, "mediaclk:direct=")) {
                    s.media_clock_offset = uint32_t(std::strtoul(v.c_str() + 16, nullptr, 10));
                } else if (starts_with(v, "ts-refclk:ptp=")) {
                    // IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0
                    auto p = split(v.substr(14), ':');
                    if (p.size() >= 2) s.ptp_gmid = p[1];
                    if (p.size() >= 3) s.ptp_domain = std::atoi(p[2].c_str());
                }
                break;
            }
            default:
                break;
        }
    }
    (void)ptime_set;
    if (!have_media) return fail("no audio media line");
    if (!have_rtpmap) return fail("no rtpmap for the payload type");
    s.dest_ip = media_c ? media_c : session_c;
    if (!s.dest_ip) return fail("no connection address");
    if (s.channels < 1 || s.channels > 64) return fail("bad channel count");
    if (s.sample_rate == 0 || s.ptime_us == 0) return fail("bad rate or ptime");
    return true;
}

std::string build_sdp(const StreamDesc& s) {
    std::ostringstream o;
    std::string dst = ipv4_to_string(s.dest_ip);
    std::string org = ipv4_to_string(s.origin_ip);
    const char* enc = s.encoding == Encoding::L24 ? "L24" : "L16";
    char ptime[32];
    if (s.ptime_us % 1000 == 0)
        std::snprintf(ptime, sizeof ptime, "%u", s.ptime_us / 1000);
    else
        std::snprintf(ptime, sizeof ptime, "%.3g", s.ptime_us / 1000.0);
    o << "v=0\r\n";
    o << "o=- " << s.session_id << " " << s.session_version << " IN IP4 " << org << "\r\n";
    o << "s=" << s.name << "\r\n";
    o << "c=IN IP4 " << dst << "/32\r\n";
    o << "t=0 0\r\n";
    o << "a=clock-domain:PTPv2 " << s.ptp_domain << "\r\n";
    o << "m=audio " << s.port << " RTP/AVP " << int(s.payload_type) << "\r\n";
    o << "c=IN IP4 " << dst << "/32\r\n";
    o << "a=rtpmap:" << int(s.payload_type) << " " << enc << "/" << s.sample_rate << "/" << s.channels << "\r\n";
    o << "a=sync-time:0\r\n";
    o << "a=framecount:" << s.frames_per_packet() << "\r\n";
    o << "a=ptime:" << ptime << "\r\n";
    o << "a=mediaclk:direct=" << s.media_clock_offset << "\r\n";
    o << "a=ts-refclk:ptp=IEEE1588-2008:" << s.ptp_gmid << ":" << s.ptp_domain << "\r\n";
    o << "a=recvonly\r\n";
    return o.str();
}

}  // namespace jvs
