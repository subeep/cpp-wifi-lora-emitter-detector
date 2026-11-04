#include "shortrange/monitor.hpp"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "shortrange/ddc.hpp"
#include "wifi_vendor.hpp"

namespace rfmon::shortrange {
namespace {

constexpr double kOutRate = 4e6;      // both decoders run at 4 MSps
constexpr double kBleHalf = 1.0e6;    // occupied half-width used for coverage
constexpr double kZigbeeHalf = 1.25e6;
constexpr int kNice = 10;             // below the Wi-Fi processing lane (5)
// Rotating-address linking limits (Monitor::link_rotating).
constexpr double kLinkGapS = 180;     // longer than the 2.4 GHz sweep's revisit gaps
constexpr double kLinkCfoHz = 3e3;    // median offsets; live per-address spread ~2 kHz (docs)
constexpr double kLinkPowerDb = 12;
constexpr double kLinkMargin = 0.5;   // best candidate must beat the next by this score
constexpr uint64_t kLinkPackets = 3;  // decide once the new address has this many packets (median offset)
constexpr double kGateDb = 5;         // demodulate where a channel is 5 dB above its floor

std::string hex16(uint16_t v) { char b[8]; std::snprintf(b, sizeof b, "0x%04X", v); return b; }
std::string zigbee_key(uint16_t pan, uint16_t short_addr) { return "PAN " + hex16(pan) + " / " + hex16(short_addr); }
std::string vendor_of(const uint8_t* msb_first6) {
    char mac[24];
    std::snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", msb_first6[0], msb_first6[1], msb_first6[2], msb_first6[3],
                  msb_first6[4], msb_first6[5]);
    if (msb_first6[0] & 0x02) return "";  // locally administered: no OUI
    return wifi::lookup_vendor(mac).name;
}
std::string eui_vendor(uint64_t eui) {
    uint8_t b[6];
    for (int i = 0; i < 6; ++i) b[i] = uint8_t(eui >> (56 - 8 * i));
    return vendor_of(b);
}
template <class T>
void merge_unique(std::vector<T>& into, const std::vector<T>& from, size_t cap) {
    for (const auto& v : from)
        if (into.size() < cap && std::find(into.begin(), into.end(), v) == into.end()) into.push_back(v);
}
void note(std::vector<std::string>& related, const std::string& text, size_t cap) {
    auto it = std::find(related.begin(), related.end(), text);
    if (it != related.end()) related.erase(it);
    related.insert(related.begin(), text);
    if (related.size() > cap) related.resize(cap);
}
std::string remote_id_key(const std::array<uint8_t, 25>& m) {
    static const char* names[] = {"Basic ID", "Location", "Authentication", "Self ID", "System", "Operator ID"};
    const unsigned type = m[0] >> 4;
    std::string key = type < 6 ? names[type] : "Message type " + std::to_string(type);
    if (type == 0) key += " / id type " + std::to_string(m[1] >> 4);
    if (type == 2) key += " / page " + std::to_string(m[1] & 15);
    return key;
}
template <class Map>
void evict_oldest(Map& m, uint64_t& counter) {
    auto oldest = m.begin();
    for (auto it = m.begin(); it != m.end(); ++it)
        if (it->second.last_seen < oldest->second.last_seen) oldest = it;
    if (oldest != m.end()) { m.erase(oldest); ++counter; }
}

template <class T>
void push_bounded(std::vector<T>& v, T value, size_t cap) {
    v.push_back(value);
    if (v.size() > cap) v.erase(v.begin());
}
double median(std::vector<float> v) {
    if (v.empty()) return NAN;
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}
// Advertising layout: PDU type and the sequence of AD types, with the
// company and Apple Continuity element types or service-data UUID. Lengths
// and values are excluded (they change with state); names are included.
std::string layout(const ble::Advert& a) {
    std::string s = a.type_name;
    for (const auto& ad : a.ad) {
        char b[16];
        std::snprintf(b, sizeof b, "|%02x", ad.type);
        s += b;
        const auto& d = ad.data;
        if (ad.type == 0xFF && d.size() >= 2) {
            std::snprintf(b, sizeof b, ":%02x%02x", d[1], d[0]);
            s += b;
            if (d[0] == 0x4C && d[1] == 0x00)
                for (size_t i = 2; i + 1 < d.size(); i += 2 + d[i + 1]) {
                    std::snprintf(b, sizeof b, "/%02x", d[i]);
                    s += b;
                }
        } else if ((ad.type == 0x16 || ad.type == 0x03 || ad.type == 0x02) && d.size() >= 2) {
            std::snprintf(b, sizeof b, ":%02x%02x", d[1], d[0]);
            s += b;
        } else if (ad.type == 0x08 || ad.type == 0x09) {
            s += ":" + a.name;
        }
    }
    return s;
}

struct ChannelResult {
    Monitor::Target target;
    std::vector<ble::Packet> ble;
    std::vector<ieee802154::Frame> zigbee;
    uint64_t syncs = 0, failures = 0;
};

}  // namespace

std::vector<Monitor::Target> Monitor::targets(double center, double rate) {
    std::vector<Target> out;
    const double ratio = rate / kOutRate;
    if (!std::isfinite(ratio) || ratio < 2 || std::abs(ratio - std::round(ratio)) > 1e-6) return out;
    const double usable = 0.44 * rate;  // the Wi-Fi path's usable half-width (8.8 MHz at 20 MSps)
    for (int c : {37, 38, 39}) {
        const double f = ble::adv_channel_frequency(c);
        if (std::abs(f - center) + kBleHalf <= usable) out.push_back({true, c, f});
    }
    for (int c = 11; c <= 26; ++c) {
        const double f = ieee802154::channel_frequency(c);
        if (std::abs(f - center) + kZigbeeHalf <= usable) out.push_back({false, c, f});
    }
    return out;
}

Monitor::Monitor(RemoteIdParser remote_id, Limits limits) : remote_id_(std::move(remote_id)), limits_(limits) {
    for (int c : {37, 38, 39}) state_.ble_coverage.push_back({"BLE " + std::to_string(c), ble::adv_channel_frequency(c)});
    for (int c = 11; c <= 26; ++c) state_.zigbee_coverage.push_back({"Zigbee " + std::to_string(c), ieee802154::channel_frequency(c)});
    worker_ = std::thread([this] { run(); });
}

Monitor::~Monitor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Monitor::enable(bool on) {
    enabled_ = on;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.enabled = on;
    state_.status = on ? "Decoding 2.4 GHz captures" : "Stopped; lists are retained history";
}

void Monitor::observe(const std::vector<std::complex<float>>& iq, const wifi_security::CaptureRecord& r) noexcept {
    if (!enabled_) return;
    size_t reserved = 0;
    try {
        const double center = r.stream_center_hz();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++state_.offered;
            if (iq.empty() || !std::isfinite(r.sample_rate_hz) || r.sample_rate_hz <= 0) { ++state_.unsupported; return; }
            const double ratio = r.sample_rate_hz / kOutRate;
            if (ratio < 2 || std::abs(ratio - std::round(ratio)) > 1e-6) { ++state_.unsupported; return; }
            if (targets(center, r.sample_rate_hz).empty()) { ++state_.outside_band; return; }
            const size_t bytes = iq.size() * sizeof(iq[0]);
            if (pending_.size() >= limits_.queued_captures || pending_bytes_ + bytes > limits_.queued_bytes) {
                ++state_.dropped_busy;
                return;
            }
            pending_bytes_ += bytes;  // reserved now; the copy happens unlocked
            reserved = bytes;
        }
        auto job = std::make_unique<Job>();
        job->iq = iq;  // the receiver's buffer is not ours to keep
        job->rate = r.sample_rate_hz;
        job->center = center;
        job->host_ns = r.host_before_ns;
        char src[160];
        std::snprintf(src, sizeof src, "LIVE / run %s / capture %llu / %.1f MHz", r.run_id.c_str(),
                      (unsigned long long)r.capture_seq, center / 1e6);
        job->source = src;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back(std::move(job));
            state_.queued = pending_.size();
        }
        wake_.notify_one();
    } catch (...) {
        // Allocation failure: drop this capture only and release its reservation.
        std::lock_guard<std::mutex> lock(mutex_);
        pending_bytes_ -= std::min(pending_bytes_, reserved);
        ++state_.dropped_busy;
    }
}

void Monitor::run() {
    // Per-thread on Linux; the per-channel threads it starts inherit it.
    (void)setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), kNice);
    for (;;) {
        std::unique_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !pending_.empty(); });
            if (stop_) return;
            job = std::move(pending_.front());
            pending_.pop_front();
            state_.queued = pending_.size();
        }
        try {
            process(job->iq.data(), job->iq.size(), job->rate, job->center, job->source, job->host_ns);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.status = std::string("Decode error: ") + e.what();
        }
        const size_t bytes = job->iq.size() * sizeof(job->iq[0]);
        job.reset();  // release the copy before its budget is returned
        std::lock_guard<std::mutex> lock(mutex_);
        pending_bytes_ -= std::min(pending_bytes_, bytes);
    }
}

void Monitor::process(const std::complex<float>* iq, size_t n, double rate, double center, const std::string& source,
                      int64_t host_ns) {
    const auto started = Clock::now();
    const auto tg = targets(center, rate);
    if (tg.empty()) return;
    const size_t decim = size_t(std::llround(rate / kOutRate));
    const Fir ble1 = design_fir(rate, 0.75e6, kOutRate - 0.75e6, decim), ble2 = design_fir(kOutRate, 0.75e6, 1.25e6, 1);
    const Fir zb1 = design_fir(rate, 1.2e6, kOutRate - 1.2e6, decim), zb2 = design_fir(kOutRate, 1.2e6, 1.8e6, 1);
    std::vector<std::future<ChannelResult>> work;
    for (const auto& t : tg)
        work.push_back(std::async(std::launch::async, [&, t] {
            ChannelResult r{t, {}, {}, 0, 0};
            const auto& f1 = t.ble ? ble1 : zb1;
            const auto& f2 = t.ble ? ble2 : zb2;
            const auto stage1 = ddc(iq, n, rate, t.frequency_hz - center, f1);
            const auto full = ddc(stage1.data(), stage1.size(), kOutRate, 0, f2);
            // Demodulate only where the channel rises above its own noise
            // floor; quiet stretches cannot hold a decodable packet. The gate
            // measures the narrow channel: the wide first stage passes about
            // +/-3 MHz, so traffic beside the channel (Wi-Fi channel 1 next to
            // BLE 37, found on the X310 bench) set the "floor" and hid packets.
            for (const auto& [a, b] : active_spans(full, 128, kGateDb, t.ble ? 400 : 800)) {
                const std::vector<std::complex<float>> channel(full.begin() + ptrdiff_t(a), full.begin() + ptrdiff_t(b));
                if (t.ble) {
                    ble::DemodStats st;
                    for (auto p : ble::demodulate(channel, t.channel, &st)) { p.sample += a; r.ble.push_back(std::move(p)); }
                    r.syncs += st.syncs;
                    r.failures += st.crc_failures;
                } else {
                    ieee802154::DemodStats st;
                    for (auto f : ieee802154::demodulate(channel, t.channel, &st)) { f.sample += a; r.zigbee.push_back(std::move(f)); }
                    r.syncs += st.syncs;
                    r.failures += st.fcs_failures;
                }
            }
            return r;
        }));
    std::vector<ChannelResult> results;
    for (auto& w : work) results.push_back(w.get());
    const auto now = Clock::now();
    const double seconds = double(n) / rate;
    std::lock_guard<std::mutex> lock(mutex_);
    struct Item { double t; const ble::Packet* b; const ieee802154::Frame* z; };
    std::vector<Item> items;
    for (const auto& r : results) {
        auto& cov = r.target.ble ? state_.ble_coverage[size_t(r.target.channel - 37)] : state_.zigbee_coverage[size_t(r.target.channel - 11)];
        cov.seconds += seconds;
        ++cov.captures;
        cov.syncs += r.syncs;
        cov.failures += r.failures;
        cov.packets += r.ble.size() + r.zigbee.size();
        cov.last = now;
        for (const auto& p : r.ble) items.push_back({double(p.sample) / kOutRate, &p, nullptr});
        for (const auto& f : r.zigbee) items.push_back({double(f.sample) / kOutRate, nullptr, &f});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.t < b.t; });
    ++capture_index_;
    for (const auto& it : items) {
        const int64_t at_ns = host_ns ? host_ns + int64_t(it.t * 1e9) : 0;
        if (it.b) ingest_ble(*it.b, source, at_ns, now);
        else ingest_zigbee(*it.z, source, at_ns, now);
    }
    ++state_.decoded_captures;
    state_.last_capture_s = seconds;
    state_.last_processing_s = std::chrono::duration<double>(Clock::now() - started).count();
    state_.max_processing_s = std::max(state_.max_processing_s, state_.last_processing_s);
    char b[200];
    std::snprintf(b, sizeof b, "%s | %.1f MSps | %.3f s | %zu channels", source.c_str(), rate / 1e6, seconds, tg.size());
    state_.last_capture = b;
}

void Monitor::push_recent(std::deque<PacketRow>& q, PacketRow row) {
    q.push_front(std::move(row));
    while (q.size() > limits_.recent_packets) q.pop_back();
}

std::string Monitor::ble_key(const ble::Address& a) { return a.text() + (a.random ? " (random)" : " (public)"); }

BleDevice& Monitor::ble_device(const ble::Address& a, Clock::time_point now) {
    const std::string key = ble_key(a);
    auto it = ble_.find(key);
    if (it == ble_.end()) {
        if (ble_.size() >= limits_.ble_devices) evict_oldest(ble_, state_.ble_evicted);
        BleDevice d;
        d.order = next_order_++;
        d.group = d.order;
        d.key = key;
        d.address = a.text();
        d.address_kind = a.kind();
        d.random = a.random;
        if (!a.random) {
            const uint8_t msb[6] = {a.b[5], a.b[4], a.b[3], a.b[2], a.b[1], a.b[0]};
            d.vendor = vendor_of(msb);
        }
        d.first_seen = now;
        it = ble_.emplace(key, std::move(d)).first;
    }
    return it->second;
}

void Monitor::ingest_ble(const ble::Packet& p, const std::string& source, int64_t host_ns, Clock::time_point now) {
    ++state_.ble_packets;
    const auto a = ble::parse(p.pdu);
    PacketRow row;
    row.at = now;
    row.host_ns = host_ns;
    row.protocol = "BLE";
    row.channel = p.channel;
    row.power_db = p.power_db;
    row.cfo_hz = p.cfo_hz;
    row.hex = ble::hex(p.pdu.data(), p.pdu.size());
    row.summary = a.valid ? a.type_name : "Unparsed PDU";
    if (!a.name.empty()) row.summary += " | " + a.name;
    if (!a.company.empty()) row.summary += " | " + a.company;
    for (const auto& t : a.tags) row.summary += " | " + t;
    const ble::Address* sender = a.scanner ? &*a.scanner : a.initiator ? &*a.initiator : a.advertiser ? &*a.advertiser : nullptr;
    row.source = sender ? sender->text() : "";
    push_recent(state_.recent_ble, row);
    if (!a.valid || !sender) return;

    const bool is_new = !ble_.count(ble_key(*sender));
    auto& d = ble_device(*sender, now);
    // Heard again after a successor address was linked to it: the two were
    // on air together, so they are not one rotating device.
    if (!d.successor.empty()) unlink_successor(d);
    push_bounded(d.cfo_history, float(p.cfo_hz), 16);
    push_bounded(d.power_history, float(p.power_db), 16);
    if (is_new) d.first_capture = capture_index_;
    d.last_capture = capture_index_;
    ++d.packets;
    ++d.pdu_counts[a.type_name];
    ++d.channel_counts[p.channel];
    d.last_power_db = p.power_db;
    d.max_power_db = std::max(d.max_power_db, p.power_db);
    d.last_cfo_hz = p.cfo_hz;
    d.last_seen = now;
    d.last_host_ns = host_ns;
    d.last_source = source;
    if (a.type == 3) {  // SCAN_REQ: the sender is a scanner
        d.roles.insert("Scanner (active scanning)");
        note(d.related, "Scan request to " + a.advertiser->text(), limits_.related);
        const std::string adv_key = a.advertiser->text() + (a.advertiser->random ? " (random)" : " (public)");
        if (auto it = ble_.find(adv_key); it != ble_.end()) note(it->second.related, "Scanned by " + sender->text(), limits_.related);
        return;
    }
    if (a.type == 5) {  // CONNECT_IND: the sender is an initiator
        d.roles.insert("Initiator (connection request)");
        note(d.related, "Connection request to " + a.advertiser->text(), limits_.related);
        const std::string adv_key = a.advertiser->text() + (a.advertiser->random ? " (random)" : " (public)");
        if (auto it = ble_.find(adv_key); it != ble_.end())
            note(it->second.related, "Connection request from " + sender->text(), limits_.related);
        d.adv_fields = a.fields;
        d.adv_hex = row.hex;
        return;
    }
    d.roles.insert(a.type == 4 ? "Advertiser (scan response)" : "Advertiser");
    const auto lay = layout(a);
    if (std::find(d.layouts.begin(), d.layouts.end(), lay) == d.layouts.end()) push_bounded(d.layouts, lay, 8);
    if (!d.link_checked && d.packets >= kLinkPackets) {
        d.link_checked = true;
        link_rotating(d);
    }
    if (a.type == 4) {
        d.scan_response_fields = a.fields;
        d.scan_response_hex = row.hex;
    } else {
        d.adv_fields = a.fields;
        d.adv_hex = row.hex;
    }
    if (a.target) note(d.related, "Directed advertising to " + a.target->text(), limits_.related);
    if (!a.name.empty()) d.name = a.name;
    if (!a.company.empty()) d.company = a.company;
    if (a.appearance) {
        for (const auto& f : a.fields)
            if (f.name == "Appearance") d.appearance = f.value;
    }
    if (a.tx_power_dbm) d.tx_power_dbm = a.tx_power_dbm;
    merge_unique(d.services, a.services, 32);
    merge_unique(d.tags, a.tags, 16);
    for (const auto& m : a.remote_id_messages) {
        std::string parsed = remote_id_ ? remote_id_(m.data()) : "";
        d.remote_id[remote_id_key(m)] = parsed.empty() ? "raw " + ble::hex(m.data(), m.size()) : parsed;
    }
}

double BleDevice::median_cfo() const { return median(cfo_history); }
double BleDevice::median_power() const { return median(power_history); }

// A private (resolvable / non-resolvable) address that first appears is
// linked to an earlier one as the probable same device when all hold:
//  - same address kind; the earlier address is older and has no successor;
//  - the earlier address was last heard before the new one's first capture
//    (no overlap), at most kLinkGapS earlier (covers the sweep's revisit gaps);
//  - they share an advertising layout;
//  - median carrier offset within kLinkCfoHz and median power within
//    kLinkPowerDb (decided after kLinkPackets packets of the new address);
//  - the best candidate is clearly better than the next one.
// Provisional: without the device's IRK no link is certain.
void Monitor::link_rotating(BleDevice& d) {
    if (d.address_kind != "Random resolvable (rotating)" && d.address_kind != "Random non-resolvable") return;
    const double cfo = d.median_cfo(), power = d.median_power();
    BleDevice* best = nullptr;
    double best_score = 1e9, second = 1e9;
    for (auto& [k, c] : ble_) {
        if (&c == &d || c.order > d.order || !c.successor.empty() || c.address_kind != d.address_kind) continue;
        if (c.last_capture >= d.first_capture) continue;
        const double gap = std::chrono::duration<double>(d.first_seen - c.last_seen).count();
        if (gap < 0 || gap > kLinkGapS) continue;
        bool shared = false;
        for (const auto& l : d.layouts) shared |= std::find(c.layouts.begin(), c.layouts.end(), l) != c.layouts.end();
        if (!shared) continue;
        const double dc = std::abs(c.median_cfo() - cfo), dp = std::abs(c.median_power() - power);
        if (!(dc <= kLinkCfoHz) || !(dp <= kLinkPowerDb)) continue;
        const double score = dc / kLinkCfoHz + dp / kLinkPowerDb + gap / kLinkGapS;
        if (score < best_score) { second = best_score; best_score = score; best = &c; }
        else if (score < second) second = score;
    }
    if (!best) return;
    if (second - best_score < kLinkMargin) { ++state_.ble_links_ambiguous; return; }
    d.predecessor = best->key;
    d.group = best->group;
    best->successor = d.key;
    char why[200];
    std::snprintf(why, sizeof why, "follows %s: same advertising layout, carrier offset %+.1f kHz, power %+.1f dB, %.0f s gap",
                  best->address.c_str(), (cfo - best->median_cfo()) / 1e3, power - best->median_power(),
                  std::chrono::duration<double>(d.first_seen - best->last_seen).count());
    d.link_reason = why;
    ++state_.ble_links;
}

void Monitor::unlink_successor(BleDevice& d) {
    auto it = ble_.find(d.successor);
    d.successor.clear();
    ++state_.ble_links_broken;
    if (it == ble_.end()) return;
    it->second.predecessor.clear();
    it->second.link_reason.clear();
    const uint64_t g = it->second.order;
    for (BleDevice* x = &it->second; x;) {
        x->group = g;
        auto next = ble_.find(x->successor);
        x = next == ble_.end() ? nullptr : &next->second;
    }
}

ZigbeeDevice* Monitor::zigbee_device(const std::string& key, Clock::time_point now) {
    auto it = zigbee_.find(key);
    if (it == zigbee_.end()) {
        if (zigbee_.size() >= limits_.zigbee_devices) evict_oldest(zigbee_, state_.zigbee_evicted);
        ZigbeeDevice d;
        d.order = next_order_++;
        d.key = key;
        d.address = key;
        d.first_seen = now;
        it = zigbee_.emplace(key, std::move(d)).first;
    }
    return &it->second;
}

void Monitor::ingest_zigbee(const ieee802154::Frame& f, const std::string& source, int64_t host_ns, Clock::time_point now) {
    ++state_.zigbee_frames;
    const auto m = ieee802154::parse(f.psdu);
    PacketRow row;
    row.at = now;
    row.host_ns = host_ns;
    row.protocol = m.network.empty() ? "802.15.4" : m.network;
    row.channel = f.channel;
    row.power_db = f.power_db;
    row.cfo_hz = f.cfo_hz;
    row.hex = ble::hex(f.psdu.data(), f.psdu.size());
    row.summary = m.valid ? m.summary : "Unparsed MAC frame";
    if (m.src.mode) row.source = (m.src_pan ? "PAN " + hex16(*m.src_pan) + " / " : "") + m.src.text();
    push_recent(state_.recent_zigbee, row);
    if (!m.valid || m.src.mode == 0) return;

    ZigbeeDevice* d = nullptr;
    if (m.src.mode == 2) {
        const uint16_t pan = m.src_pan ? *m.src_pan : 0xFFFF;
        d = zigbee_device(zigbee_key(pan, m.src.short_addr), now);
        d->pan = pan;
        d->short_addr = m.src.short_addr;
    } else {
        d = zigbee_device("IEEE " + ieee802154::eui64_text(m.src.ext), now);
        d->ext = m.src.ext;
        if (m.src_pan) d->pan = *m.src_pan;
    }
    // The NWK originator's IEEE address may accompany its short address.
    if (m.nwk_src && m.nwk_src_ext && m.src.mode == 2 && *m.nwk_src == m.src.short_addr) d->ext = *m.nwk_src_ext;
    if (d->ext && d->vendor.empty()) d->vendor = eui_vendor(*d->ext);
    ++d->frames;
    ++d->frame_counts[m.summary];
    ++d->channel_counts[f.channel];
    d->last_power_db = f.power_db;
    d->max_power_db = std::max(d->max_power_db, f.power_db);
    d->last_cfo_hz = f.cfo_hz;
    d->last_seen = now;
    d->last_host_ns = host_ns;
    d->last_source = source;
    d->last_fields = m.fields;
    d->last_hex = row.hex;
    if (!m.network.empty()) d->network = m.network;
    if (!m.role_hint.empty()) d->roles.insert(m.role_hint);
    if (m.security || m.nwk_security) d->secured_seen = true;
    else if (m.type == 1) d->unsecured_seen = true;
    if (m.is_beacon) d->association_permit = m.association_permit;
    if (m.zigbee_ext_pan) d->ext_pan = m.zigbee_ext_pan;
    if (m.stack_profile) d->stack_profile = m.stack_profile;
    // A relayed frame also shows its NWK originator, which did not transmit it.
    if (m.nwk_src && m.src.mode == 2 && *m.nwk_src != m.src.short_addr && d->pan) {
        auto* o = zigbee_device(zigbee_key(*d->pan, *m.nwk_src), now);
        o->pan = d->pan;
        o->short_addr = *m.nwk_src;
        if (m.nwk_src_ext) { o->ext = *m.nwk_src_ext; if (o->vendor.empty()) o->vendor = eui_vendor(*o->ext); }
        o->network = "Zigbee";
        o->roles.insert("NWK originator (seen relayed)");
        ++o->frame_counts["Relayed by " + hex16(m.src.short_addr)];
        o->last_seen = now;
        o->last_host_ns = host_ns;
        o->last_source = source;
    }
}

Snapshot Monitor::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s = state_;
    s.ble.reserve(ble_.size());
    for (const auto& [k, d] : ble_) s.ble.push_back(d);
    for (const auto& [k, d] : zigbee_) s.zigbee.push_back(d);
    // Probable-device view: one row per group, shown as its newest address
    // with counts merged; the row keeps the group's first position.
    std::map<uint64_t, std::vector<const BleDevice*>> groups;
    for (const auto& d : s.ble) groups[d.group].push_back(&d);
    for (auto& [g, members] : groups) {
        std::sort(members.begin(), members.end(), [](const BleDevice* a, const BleDevice* b) { return a->order < b->order; });
        BleDevice row = *members.back();
        row.key = "group/" + std::to_string(g);
        row.order = members.front()->order;
        row.first_seen = members.front()->first_seen;
        if (members.size() > 1) {
            row.packets = 0;
            row.channel_counts.clear();
            row.pdu_counts.clear();
            for (const auto* m : members) {
                row.packets += m->packets;
                for (const auto& [c, k] : m->channel_counts) row.channel_counts[c] += k;
                for (const auto& [t, k] : m->pdu_counts) row.pdu_counts[t] += k;
                row.max_power_db = std::max(row.max_power_db, m->max_power_db);
                row.first_seen = std::min(row.first_seen, m->first_seen);
                row.roles.insert(m->roles.begin(), m->roles.end());
                merge_unique(row.tags, m->tags, 16);
                merge_unique(row.services, m->services, 32);
                for (const auto& [k, v] : m->remote_id) row.remote_id.emplace(k, v);
                if (row.name.empty()) row.name = m->name;
                if (row.company.empty()) row.company = m->company;
            }
        }
        for (const auto* m : members) row.group_addresses.push_back(m->address);
        s.ble_grouped.push_back(std::move(row));
    }
    auto named_first = [](const BleDevice& a, const BleDevice& b) {
        if (a.name.empty() != b.name.empty()) return !a.name.empty();
        return a.order < b.order;
    };
    std::sort(s.ble.begin(), s.ble.end(), named_first);
    std::sort(s.ble_grouped.begin(), s.ble_grouped.end(), named_first);
    std::sort(s.zigbee.begin(), s.zigbee.end(), [](const ZigbeeDevice& a, const ZigbeeDevice& b) {
        if (a.vendor.empty() != b.vendor.empty()) return !a.vendor.empty();
        return a.order < b.order;
    });
    return s;
}

void Monitor::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ble_.clear();
    zigbee_.clear();
    const bool on = state_.enabled;
    const auto status = state_.status;
    auto ble_cov = state_.ble_coverage, zb_cov = state_.zigbee_coverage;
    for (auto& c : ble_cov) c = {c.label, c.frequency_hz};
    for (auto& c : zb_cov) c = {c.label, c.frequency_hz};
    state_ = Snapshot{};
    state_.enabled = on;
    state_.status = status;
    state_.ble_coverage = ble_cov;
    state_.zigbee_coverage = zb_cov;
}

std::string Monitor::export_json(const std::string& directory) const {
    namespace fs = std::filesystem;
    using J = nlohmann::json;
    const auto s = snapshot();
    const auto now = Clock::now();
    auto age = [&](Clock::time_point t) { return std::chrono::duration<double>(now - t).count(); };
    auto fields = [](const auto& v) { J a = J::array(); for (const auto& f : v) a.push_back({{"name", f.name}, {"value", f.value}}); return a; };
    J j;
    j["schema"] = "rfmon-shortrange-observations";
    j["version"] = 1;
    j["note"] = "Passive observations. Entries are addresses seen, not physical devices; private BLE addresses rotate. "
                "Reported names and identifiers are unauthenticated. Encrypted payloads were not decrypted.";
    j["counters"] = {{"offered", s.offered}, {"decoded_captures", s.decoded_captures}, {"dropped_busy", s.dropped_busy},
                     {"outside_band", s.outside_band}, {"unsupported", s.unsupported}, {"ble_packets", s.ble_packets},
                     {"zigbee_frames", s.zigbee_frames}, {"ble_evicted", s.ble_evicted}, {"zigbee_evicted", s.zigbee_evicted},
                     {"ble_probable_devices", s.ble_grouped.size()}, {"ble_links", s.ble_links},
                     {"ble_links_broken", s.ble_links_broken}, {"ble_links_ambiguous", s.ble_links_ambiguous}};
    for (const auto* cov : {&s.ble_coverage, &s.zigbee_coverage})
        for (const auto& c : *cov)
            j["coverage"].push_back({{"channel", c.label}, {"frequency_hz", c.frequency_hz}, {"seconds", c.seconds},
                                     {"captures", c.captures}, {"packets", c.packets}, {"syncs", c.syncs}, {"failures", c.failures}});
    j["bluetooth_le"] = J::array();
    for (const auto& d : s.ble) {
        J e = {{"address", d.address}, {"address_type", d.address_kind}, {"vendor", d.vendor}, {"name", d.name},
               {"company", d.company}, {"appearance", d.appearance}, {"tags", d.tags}, {"services", d.services},
               {"roles", d.roles}, {"packets", d.packets}, {"pdu_counts", d.pdu_counts}, {"last_power_db", d.last_power_db},
               {"max_power_db", d.max_power_db}, {"last_cfo_hz", d.last_cfo_hz}, {"age_s", age(d.last_seen)},
               {"last_host_ns", d.last_host_ns}, {"advertising_fields", fields(d.adv_fields)}, {"advertising_hex", d.adv_hex},
               {"scan_response_fields", fields(d.scan_response_fields)}, {"scan_response_hex", d.scan_response_hex},
               {"related", d.related}, {"remote_id", d.remote_id}, {"last_source", d.last_source}};
        J ch = J::object();
        for (const auto& [c, k] : d.channel_counts) ch[std::to_string(c)] = k;
        e["channels"] = ch;
        if (d.tx_power_dbm) e["tx_power_dbm"] = *d.tx_power_dbm;
        e["probable_device_group"] = d.group;
        e["linked_from"] = d.predecessor;
        e["linked_to"] = d.successor;
        e["link_reason"] = d.link_reason;
        e["advertising_layouts"] = d.layouts;
        e["cfo_history_hz"] = d.cfo_history;
        e["power_history_db"] = d.power_history;
        j["bluetooth_le"].push_back(e);
    }
    // Recent packets (newest first, bounded by Limits::recent_packets) with raw bytes.
    for (const auto* q : {&s.recent_ble, &s.recent_zigbee}) {
        J rows = J::array();
        for (const auto& r : *q)
            rows.push_back({{"protocol", r.protocol}, {"channel", r.channel}, {"age_s", age(r.at)}, {"host_ns", r.host_ns},
                            {"sender", r.source}, {"power_db", r.power_db}, {"cfo_hz", r.cfo_hz}, {"summary", r.summary},
                            {"hex", r.hex}});
        j[q == &s.recent_ble ? "recent_ble_packets" : "recent_802154_frames"] = rows;
    }
    j["ieee802154"] = J::array();
    for (const auto& d : s.zigbee) {
        J e = {{"address", d.address}, {"network", d.network}, {"vendor", d.vendor}, {"roles", d.roles}, {"frames", d.frames},
               {"frame_counts", d.frame_counts}, {"secured_seen", d.secured_seen}, {"unsecured_seen", d.unsecured_seen},
               {"last_power_db", d.last_power_db}, {"max_power_db", d.max_power_db}, {"last_cfo_hz", d.last_cfo_hz},
               {"age_s", age(d.last_seen)}, {"last_host_ns", d.last_host_ns}, {"last_fields", fields(d.last_fields)},
               {"last_hex", d.last_hex}, {"last_source", d.last_source}};
        if (d.pan) e["pan_id"] = *d.pan;
        if (d.short_addr) e["short_address"] = *d.short_addr;
        if (d.ext) e["ieee_address"] = ieee802154::eui64_text(*d.ext);
        if (d.ext_pan) e["extended_pan_id"] = ieee802154::eui64_text(*d.ext_pan);
        J ch = J::object();
        for (const auto& [c, k] : d.channel_counts) ch[std::to_string(c)] = k;
        e["channels"] = ch;
        j["ieee802154"].push_back(e);
    }
    fs::create_directories(directory);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm);
    fs::path path;
    for (int i = 0;; ++i) {
        path = fs::path(directory) / (std::string("shortrange-") + stamp + (i ? "-" + std::to_string(i) : "") + ".json");
        if (!fs::exists(path)) break;
        if (i > 1000) throw std::runtime_error("Cannot choose a unique export name");
    }
    std::ofstream out(path);
    out << j.dump(2) << "\n";
    if (!out) throw std::runtime_error("Export write failed");
    return path.string();
}

}  // namespace rfmon::shortrange
