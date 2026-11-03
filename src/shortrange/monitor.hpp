// Bluetooth LE advertising and IEEE 802.15.4 / Zigbee decoding from the
// existing 2.4 GHz captures. A receive-only observer: it never tunes the
// radio, and a capture that arrives while the previous one is still being
// decoded is dropped and counted (acquisition never waits for it).
//
// Lists hold *addresses seen*, not physical devices: phones rotate private
// BLE addresses, and one Zigbee node can appear by short and IEEE address.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "security/wifi_frame_event.hpp"
#include "shortrange/ble.hpp"
#include "shortrange/ieee802154.hpp"

namespace rfmon::shortrange {

using Clock = std::chrono::steady_clock;

struct ChannelCoverage {
    std::string label;          // "BLE 37", "Zigbee 15"
    double frequency_hz = 0;
    double seconds = 0;         // capture time decoded on this channel
    uint64_t captures = 0, packets = 0, syncs = 0, failures = 0;
    Clock::time_point last{};   // last decode (epoch: never)
};

struct BleDevice {
    uint64_t order = 0;          // creation sequence: fixes the row position in the list
    std::string key, address, address_kind, vendor;  // vendor: public addresses only
    bool random = false;
    std::string name, company, appearance;
    std::vector<std::string> tags, services;
    std::optional<int> tx_power_dbm;
    std::set<std::string> roles;                 // Advertiser, Scanner, Initiator
    std::map<std::string, uint64_t> pdu_counts;  // PDU type name -> packets
    std::map<int, uint64_t> channel_counts;
    uint64_t packets = 0;
    double last_power_db = -200, max_power_db = -200, last_cfo_hz = 0;
    Clock::time_point first_seen{}, last_seen{};
    int64_t last_host_ns = 0;
    std::vector<ble::Field> adv_fields, scan_response_fields;
    std::string adv_hex, scan_response_hex;
    std::vector<std::string> related;            // bounded: scanners / initiators / targets seen with it
    std::map<std::string, std::string> remote_id;  // ASTM message type -> parsed JSON (or raw hex)
    std::string last_source;
    // Rotating-address linking (provisional; see Monitor::link_rotating).
    uint64_t group = 0;                       // probable-device group: order of its first address
    std::string predecessor, successor;       // linked addresses (device keys)
    std::string link_reason;                  // why this address was linked to its predecessor
    std::vector<std::string> layouts;         // advertising layouts seen (bounded)
    std::vector<float> cfo_history, power_history;  // latest packets (bounded)
    uint64_t first_capture = 0, last_capture = 0;  // Monitor capture indexes of the first / latest packet
    bool link_checked = false;                // linking is decided once, after kLinkPackets packets
    std::vector<std::string> group_addresses; // grouped view only: oldest first
    double median_cfo() const;
    double median_power() const;
};

struct ZigbeeDevice {
    uint64_t order = 0;                          // creation sequence: fixes the row position
    std::string key, address;                    // "PAN 0x1A62 / 0x796F" or an IEEE address
    std::optional<uint16_t> pan, short_addr;
    std::optional<uint64_t> ext;
    std::string vendor, network;
    std::set<std::string> roles;
    bool secured_seen = false, unsecured_seen = false, association_permit = false;
    std::optional<uint64_t> ext_pan;
    std::optional<int> stack_profile;
    std::map<std::string, uint64_t> frame_counts;
    std::map<int, uint64_t> channel_counts;
    uint64_t frames = 0;
    double last_power_db = -200, max_power_db = -200, last_cfo_hz = 0;
    Clock::time_point first_seen{}, last_seen{};
    int64_t last_host_ns = 0;
    std::vector<ieee802154::Field> last_fields;
    std::string last_hex, last_source;
};

struct PacketRow {
    Clock::time_point at{};
    int64_t host_ns = 0;
    std::string protocol, summary, source, hex;
    int channel = 0;
    double power_db = 0, cfo_hz = 0;
};

struct Snapshot {
    bool enabled = false;
    std::string status = "Stopped";
    uint64_t offered = 0, decoded_captures = 0, dropped_busy = 0, outside_band = 0, unsupported = 0;
    size_t queued = 0;           // captures waiting to be decoded
    uint64_t ble_packets = 0, zigbee_frames = 0, ble_evicted = 0, zigbee_evicted = 0;
    double last_capture_s = 0, last_processing_s = 0, max_processing_s = 0;
    std::string last_capture;    // centre / rate / provenance of the latest decoded capture
    std::vector<ChannelCoverage> ble_coverage, zigbee_coverage;
    // Stable order: rows never move as packets arrive. BLE: advertised names
    // first, then the rest, each in first-seen order (a device moves up once,
    // when its name is first received). 802.15.4 carries no names: entries
    // with a known IEEE vendor first, then first-seen order.
    std::vector<BleDevice> ble;          // every address
    // Probable devices: rotating private addresses linked into one row
    // (newest address shown, counts merged). Provisional, see link_rotating.
    std::vector<BleDevice> ble_grouped;
    uint64_t ble_links = 0, ble_links_broken = 0, ble_links_ambiguous = 0;
    std::vector<ZigbeeDevice> zigbee;
    std::deque<PacketRow> recent_ble, recent_zigbee;  // newest first
};

struct Limits {
    size_t ble_devices = 2048, zigbee_devices = 256, recent_packets = 200, related = 16;
    // Captures waiting while one decodes. The scanner hands over a step's
    // sub-captures in a burst; this absorbs one burst (4 x 160 MB at 20 MSps).
    size_t queued_captures = 4;
    size_t queued_bytes = size_t(768) << 20;
};

class Monitor {
public:
    // Optional ASTM F3411 message parser (25 bytes -> JSON text), supplied by
    // the Drones add-on when it is compiled in.
    using RemoteIdParser = std::function<std::string(const uint8_t*)>;
    explicit Monitor(RemoteIdParser remote_id = {}, Limits limits = {});
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void enable(bool on);
    bool enabled() const { return enabled_; }
    // Scanner Wi-Fi IQ observer: copies the capture into a bounded queue, or
    // counts a drop when the queue is full; never waits for the decoder.
    void observe(const std::vector<std::complex<float>>& iq, const wifi_security::CaptureRecord& record) noexcept;
    // Synchronous decode of one capture (tests / offline use).
    void process(const std::complex<float>* iq, size_t n, double rate_hz, double center_hz, const std::string& source,
                 int64_t host_ns = 0);
    Snapshot snapshot() const;
    void clear();
    // Writes the current lists as JSON; returns the path. Throws on I/O errors.
    std::string export_json(const std::string& directory) const;

    // Channels a capture at (center, rate) can decode; exposed for the coverage view and tests.
    struct Target { bool ble; int channel; double frequency_hz; };
    static std::vector<Target> targets(double center_hz, double rate_hz);

private:
    struct Job {
        std::vector<std::complex<float>> iq;
        double rate = 0, center = 0;
        std::string source;
        int64_t host_ns = 0;
    };
    void run();
    void ingest_ble(const ble::Packet& p, const std::string& source, int64_t host_ns, Clock::time_point now);
    void ingest_zigbee(const ieee802154::Frame& f, const std::string& source, int64_t host_ns, Clock::time_point now);
    static std::string ble_key(const ble::Address& a);
    BleDevice& ble_device(const ble::Address& a, Clock::time_point now);
    void link_rotating(BleDevice& d);
    void unlink_successor(BleDevice& d);
    ZigbeeDevice* zigbee_device(const std::string& key, Clock::time_point now);
    void push_recent(std::deque<PacketRow>& q, PacketRow row);

    RemoteIdParser remote_id_;
    Limits limits_;
    std::atomic<bool> enabled_{false}, stop_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::unique_ptr<Job>> pending_;
    size_t pending_bytes_ = 0;
    std::thread worker_;
    Snapshot state_;
    std::map<std::string, BleDevice> ble_;
    std::map<std::string, ZigbeeDevice> zigbee_;
    uint64_t next_order_ = 0, capture_index_ = 0;
};

}  // namespace rfmon::shortrange
