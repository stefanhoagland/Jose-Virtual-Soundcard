// SAP (RFC 2974): how AES67 streams announce themselves on the network.
// Dante devices in AES67 mode send and listen to SAP on 239.255.255.255:9875.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jvs/net.h"
#include "jvs/sdp.h"

namespace jvs {

struct SapPacket {
    bool deletion = false;
    uint16_t msg_id_hash = 0;
    uint32_t origin_ip = 0;  // network byte order
    std::string payload_type = "application/sdp";
    std::string sdp;
};

bool parse_sap(const uint8_t* p, int len, SapPacket& out);
std::vector<uint8_t> build_sap(const SapPacket& p);

struct DiscoveredStream {
    StreamDesc desc;
    std::string sdp;
    int64_t last_seen_mono = 0;
};

// Listens for announcements and keeps a list of live streams.
class SapListener {
public:
    ~SapListener();
    bool start(const NetInterface& iface);
    void stop();
    std::vector<DiscoveredStream> streams() const;

private:
    void loop();
    UdpSocket sock_;
    std::thread th_;
    std::atomic<bool> run_{false};
    mutable std::mutex mu_;
    std::map<std::string, DiscoveredStream> streams_;
};

// Periodically announces our own streams.
class SapAnnouncer {
public:
    ~SapAnnouncer();
    bool start(const NetInterface& iface);
    void stop();  // sends deletion messages
    void set(const StreamDesc& d);  // add or update
    void remove(const std::string& key);

private:
    void loop();
    void send_one(const StreamDesc& d, bool deletion);
    NetInterface iface_;
    UdpSocket sock_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::mutex mu_;
    std::map<std::string, StreamDesc> streams_;
};

}  // namespace jvs
