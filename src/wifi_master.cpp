#include "wifi_master.hpp"
#include "wifi_vendor.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace rfmon::wifi_master {
using json = nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WifiMasterReading, ts, channel_hz,
    bandwidth_hz, phy, cfo_ppm, irr_db, iq_eps, iq_phi_deg, dc_dbc, dc_ang_deg,
    snr_db, evm_pct, sync_corr)

namespace {
std::string hex(const std::string& s) {
    const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
std::string unhex(const std::string& s) {
    if (s.size() % 2) throw std::runtime_error("Odd hex string");
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("Invalid hex string");
    };
    std::string out;
    for (size_t i = 0; i < s.size(); i += 2) out += char((digit(s[i]) << 4) | digit(s[i+1]));
    return out;
}
json beacon_json(const wifi::BeaconInfo& b) {
    json j = {{"bssid", b.bssid}, {"ssid_present", b.ssid_present}, {"channel", b.channel},
        {"channel_source", b.channel_source}, {"beacon_interval_tu", b.beacon_interval_tu},
        {"capability", b.capability}, {"fcs_valid", b.fcs_valid}, {"ies_complete", b.ies_complete},
        {"frame_source", b.frame_source}, {"security", b.security}, {"ciphers", b.ciphers},
        {"pmf", b.pmf}, {"standards", b.standards}, {"wps_present", b.wps_present}};
    // SSID/WPS are arbitrary octets, not guaranteed UTF-8. Store exact bytes
    // alongside an escaped human-readable representation; JSON never loses bytes.
    auto text = [&](const char* key, const std::string& s) {
        j[key] = wifi::display_text(s);
        j[std::string(key) + "_hex"] = hex(s);
    };
    text("ssid", b.ssid); text("wps_manufacturer", b.wps_manufacturer);
    text("wps_model_name", b.wps_model_name); text("wps_model_number", b.wps_model_number);
    text("wps_device_name", b.wps_device_name);
    return j;
}
wifi::BeaconInfo read_beacon(const json& j) {
    wifi::BeaconInfo b;
    b.bssid = j.at("bssid").get<std::string>();
    b.ssid_present = j.value("ssid_present", false);
    b.channel = j.value("channel", 0); b.channel_source = j.value("channel_source", "");
    b.beacon_interval_tu = j.value("beacon_interval_tu", uint16_t(0));
    b.capability = j.value("capability", uint16_t(0));
    b.fcs_valid = j.value("fcs_valid", false); b.ies_complete = j.value("ies_complete", true);
    b.frame_source = j.value("frame_source", ""); b.security = j.value("security", "");
    b.ciphers = j.value("ciphers", ""); b.pmf = j.value("pmf", "");
    b.standards = j.value("standards", ""); b.wps_present = j.value("wps_present", false);
    b.ssid = unhex(j.value("ssid_hex", ""));
    b.wps_manufacturer = unhex(j.value("wps_manufacturer_hex", ""));
    b.wps_model_name = unhex(j.value("wps_model_name_hex", ""));
    b.wps_model_number = unhex(j.value("wps_model_number_hex", ""));
    b.wps_device_name = unhex(j.value("wps_device_name_hex", ""));
    return b;
}
json meta_json(const std::string& key, bool mac, int64_t first_seen) {
    return {{"meta", 1}, {"schema", 2}, {"key", key}, {"key_is_mac", mac}, {"first_seen_ts", first_seen}};
}
double median_of(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n/2] : 0.5 * (v[n/2-1] + v[n/2]);
}
bool safe_key(const std::string& key, bool mac) {
    if (mac) return wifi::canonical_mac(key).has_value();
    if (key.rfind("WIFI-FP-", 0) != 0 || key.size() <= 8) return false;
    return std::all_of(key.begin()+8, key.end(), [](char c) { return c >= '0' && c <= '9'; });
}
}

WifiMasterList::WifiMasterList(std::string dir) : dir_(std::move(dir)) { load_all_devices(); }
std::string WifiMasterList::device_path(const std::string& key) const { return dir_ + "/" + key + ".ndjson"; }
std::string WifiMasterList::storage_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_error_;
}
void WifiMasterList::load_all_devices() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) { storage_error_ = "Cannot create Wi-Fi storage: " + ec.message(); return; }
    fs::directory_iterator end, it(dir_, ec);
    if (ec) { storage_error_ = "Cannot read Wi-Fi storage: " + ec.message(); return; }
    for (; it != end; it.increment(ec)) {
        if (ec) { storage_error_ = "Cannot enumerate Wi-Fi storage: " + ec.message(); break; }
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".ndjson") continue;
        std::ifstream in(entry.path());
        if (!in) { storage_error_ = "Cannot read " + entry.path().string(); continue; }
        Device dev;
        bool have_meta = false;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                auto j = json::parse(line);
                if (!j.is_object()) throw std::runtime_error("Expected object");
                if (!have_meta) {
                    dev.key = j.at("key").get<std::string>();
                    dev.key_is_mac = j.at("key_is_mac").get<bool>();
                    dev.first_seen_ts = j.at("first_seen_ts").get<int64_t>();
                    if (!safe_key(dev.key, dev.key_is_mac)) throw std::runtime_error("Invalid key");
                    if (dev.key_is_mac) dev.key = *wifi::canonical_mac(dev.key);
                    have_meta = true;
                    continue;
                }
                if (j.value("type", "") == "identity") {
                    auto b = read_beacon(j.at("identity"));
                    if (!dev.key_is_mac || b.bssid != dev.key || !b.fcs_valid) throw std::runtime_error("Invalid identity");
                    int64_t ts = j.at("ts").get<int64_t>();
                    // Validate the whole record before updating the loaded state.
                    auto count = j.at("identity_count").get<uint64_t>();
                    auto ssid_ts = j.value("ssid_seen_ts", int64_t(0));
                    auto wps_ts = j.value("wps_seen_ts", int64_t(0));
                    auto ssid_source = j.value("ssid_source", "");
                    auto wps_source = j.value("wps_source", "");
                    auto monitored = j.at("monitored_channel_hz").get<double>();
                    auto phy = j.value("phy", "DSSS");
                    if (phy != "DSSS" && phy != "OFDM") throw std::runtime_error("Invalid identity PHY");
                    if (!dev.identity || ts >= dev.identity_ts) {
                        dev.identity = std::move(b); dev.identity_ts = ts;
                        dev.identity_count = count; dev.ssid_seen_ts = ssid_ts; dev.wps_seen_ts = wps_ts;
                        dev.ssid_source = std::move(ssid_source); dev.wps_source = std::move(wps_source);
                        dev.monitored_channel_hz = monitored; dev.identity_phy = phy;
                    }
                    ++dev.identity_lines;
                    continue;
                }
                if (!j.contains("ts") || !j.contains("phy") || !j.contains("cfo_ppm"))
                    throw std::runtime_error("Unknown record");
                auto r = j.get<WifiMasterReading>();
                dev.readings.push_back(r);
            } catch (const std::exception&) {
                storage_error_ = "Skipped malformed Wi-Fi record in " + entry.path().filename().string();
                // A damaged meta line cannot safely identify any subsequent readings.
                if (!have_meta) break;
            }
        }
        if (!have_meta || (dev.readings.empty() && !dev.identity)) continue;
        while (dev.readings.size() > size_t(MAX_READINGS_PER_DEVICE)) dev.readings.pop_front();
        if (!dev.readings.empty()) {
            dev.last_seen_ts = dev.readings.back().ts;
            dev.last_channel_hz = dev.readings.back().channel_hz;
            dev.last_phy = dev.readings.back().phy;
        }
        if (dev.identity && (dev.readings.empty() || dev.identity_ts >= dev.last_seen_ts)) {
            dev.last_seen_ts = dev.identity_ts;
            dev.last_channel_hz = dev.monitored_channel_hz;
            dev.last_phy = dev.identity_phy;
        }
        if (!dev.key_is_mac) {
            try { next_fp_id_ = std::max(next_fp_id_, std::stoi(dev.key.substr(8)) + 1); }
            catch (const std::exception&) { continue; }
        }
        devices_.push_back(std::move(dev));
    }
}
int WifiMasterList::find_mac_match(const std::string& mac) const {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].key_is_mac && devices_[i].key == mac) return int(i);
    }
    return -1;
}

int WifiMasterList::find_fp_match(double irr_db, double dc_dbc, double iq_eps,
                                   double iq_phi_deg) const {
    int best_idx = -1;
    double best_dist = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < devices_.size(); ++i) {
        const Device& dev = devices_[i];
        if (dev.key_is_mac) continue;  // MAC devices are matched exactly, not by fingerprint
        size_t window = std::min(dev.readings.size(), size_t(MATCH_MEDIAN_WINDOW));
        if (window == 0) continue;

        std::vector<double> irr_v, dc_v, eps_v, phi_v;
        for (size_t k = dev.readings.size() - window; k < dev.readings.size(); ++k) {
            irr_v.push_back(dev.readings[k].irr_db);
            dc_v.push_back(dev.readings[k].dc_dbc);
            eps_v.push_back(dev.readings[k].iq_eps);
            phi_v.push_back(dev.readings[k].iq_phi_deg);
        }
        double m_irr = median_of(irr_v);
        double m_dc = median_of(dc_v);
        double m_eps = median_of(eps_v);
        double m_phi = median_of(phi_v);

        if (std::abs(irr_db - m_irr) > IRR_TOLERANCE_DB) continue;
        if (std::abs(dc_dbc - m_dc) > DC_DBC_TOLERANCE_DB) continue;
        if (std::abs(iq_eps - m_eps) > IQ_EPS_TOLERANCE) continue;
        if (std::abs(iq_phi_deg - m_phi) > IQ_PHI_TOLERANCE_DEG) continue;

        double dist = std::abs(irr_db - m_irr) + std::abs(dc_dbc - m_dc) +
                      std::abs(iq_eps - m_eps) * 100.0 + std::abs(iq_phi_deg - m_phi);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = int(i);
        }
    }
    return best_idx;
}

void WifiMasterList::append_line(const std::string& key, const std::string& line) {
    const auto path = device_path(key);
    // A crash may leave a partial last line. Keep the next valid observation
    // separate so it can still be recovered on restart.
    bool missing_newline = false;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (in && in.tellg() > 0) {
            in.seekg(-1, std::ios::end);
            char last = '\n'; in.get(last); missing_newline = last != '\n';
        }
    }
    std::ofstream out(path, std::ios::app);
    if (missing_newline) out << '\n';
    out << line << '\n';
    out.flush();
    if (!out) storage_error_ = "Cannot save Wi-Fi record: " + device_path(key);
}
std::string WifiMasterList::identity_line(const Device& dev) const {
    return json{{"type", "identity"}, {"phy", dev.identity_phy}, {"ts", dev.identity_ts}, {"identity_count", dev.identity_count},
        { "ssid_seen_ts", dev.ssid_seen_ts}, {"wps_seen_ts", dev.wps_seen_ts},
        {"ssid_source", dev.ssid_source}, {"wps_source", dev.wps_source},
        {"monitored_channel_hz", dev.monitored_channel_hz}, {"identity", beacon_json(*dev.identity)}}.dump();
}
void WifiMasterList::compact_device_file(const Device& dev) {
    std::string path = device_path(dev.key), tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << meta_json(dev.key, dev.key_is_mac, dev.first_seen_ts).dump() << '\n';
        if (dev.identity) out << identity_line(dev) << '\n';
        for (const auto& r : dev.readings) out << json(r).dump() << '\n';
        out.flush();
        if (!out) { storage_error_ = "Cannot compact Wi-Fi record: " + tmp; return; }
        out.close();
        if (!out) { storage_error_ = "Cannot close Wi-Fi record: " + tmp; return; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) storage_error_ = "Cannot replace Wi-Fi record: " + ec.message();
}
std::string WifiMasterList::record_identity(const wifi::BeaconInfo& info, double monitored_hz, int64_t ts, const std::string& phy) {
    if (phy != "DSSS" && phy != "OFDM") return "";
    auto mac = wifi::canonical_mac(info.bssid);
    if (!info.fcs_valid || !mac || !std::isfinite(monitored_hz)) return "";
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = find_mac_match(*mac);
    if (idx < 0) {
        Device dev;
        dev.key = *mac; dev.key_is_mac = true; dev.first_seen_ts = ts;
        append_line(dev.key, meta_json(dev.key, true, ts).dump());
        devices_.push_back(std::move(dev));
        idx = int(devices_.size()-1);
    }
    auto& dev = devices_[size_t(idx)];
    if (dev.identity && ts < dev.identity_ts) return dev.key;
    wifi::BeaconInfo next = info;
    next.bssid = *mac;
    if (!next.ssid.empty()) { dev.ssid_seen_ts = ts; dev.ssid_source = info.frame_source; }
    else if (dev.identity) next.ssid = dev.identity->ssid;
    // Keep last observed WPS hints when the latest frame omits the WPS IE.
    // Its timestamp is separate, and wps_present still describes the latest frame.
    if (next.wps_present) { dev.wps_seen_ts = ts; dev.wps_source = info.frame_source; }
    else if (dev.identity) {
        next.wps_manufacturer = dev.identity->wps_manufacturer;
        next.wps_model_name = dev.identity->wps_model_name;
        next.wps_model_number = dev.identity->wps_model_number;
        next.wps_device_name = dev.identity->wps_device_name;
    }
    dev.identity = std::move(next); dev.identity_ts = ts; dev.identity_phy = phy;
    ++dev.identity_count; dev.monitored_channel_hz = monitored_hz;
    if (dev.readings.empty() || ts >= dev.last_seen_ts) {
        dev.last_seen_ts = ts; dev.last_channel_hz = monitored_hz; dev.last_phy = dev.identity_phy;
    }
    append_line(dev.key, identity_line(dev));
    if (++dev.identity_lines > 100) { compact_device_file(dev); dev.identity_lines = 1; }
    return dev.key;
}
std::string WifiMasterList::record_reading(std::optional<std::string> mac, const std::string& phy,
        double channel_hz, double bandwidth_hz, const wifi_fingerprint::WifiFingerprint& fp, int64_t ts) {
    if (fp.gated_out) return "";
    if (mac) { mac = wifi::canonical_mac(*mac); if (!mac) return ""; }
    WifiMasterReading r;
    r.ts = ts; r.channel_hz = channel_hz; r.bandwidth_hz = bandwidth_hz; r.phy = phy;
    r.cfo_ppm = fp.cfo_ppm; r.irr_db = fp.irr_db; r.iq_eps = fp.iq_eps;
    r.iq_phi_deg = fp.iq_phi_deg; r.dc_dbc = fp.dc_dbc; r.dc_ang_deg = fp.dc_ang_deg;
    r.snr_db = fp.snr_db; r.evm_pct = fp.evm_pct; r.sync_corr = fp.sync_corr;
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = mac ? find_mac_match(*mac) : find_fp_match(fp.irr_db, fp.dc_dbc, fp.iq_eps, fp.iq_phi_deg);
    if (idx < 0) {
        Device dev;
        if (mac) { dev.key = *mac; dev.key_is_mac = true; }
        else {
            char buf[32]; std::snprintf(buf, sizeof(buf), "WIFI-FP-%04d", next_fp_id_++); dev.key = buf;
        }
        dev.first_seen_ts = ts;
        append_line(dev.key, meta_json(dev.key, dev.key_is_mac, ts).dump());
        devices_.push_back(std::move(dev)); idx = int(devices_.size()-1);
    }
    auto& dev = devices_[size_t(idx)];
    if ((!dev.identity && dev.readings.empty()) || ts >= dev.last_seen_ts) {
        dev.last_seen_ts = ts; dev.last_channel_hz = channel_hz; dev.last_phy = phy;
    }
    dev.readings.push_back(r);
    append_line(dev.key, json(r).dump());
    if (dev.readings.size() > size_t(COMPACT_TRIGGER_READINGS)) {
        while (dev.readings.size() > size_t(MAX_READINGS_PER_DEVICE)) dev.readings.pop_front();
        compact_device_file(dev); dev.identity_lines = dev.identity ? 1 : 0;
    }
    return dev.key;
}
std::vector<WifiMasterRow> WifiMasterList::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WifiMasterRow> rows;
    for (const auto& dev : devices_) {
        WifiMasterRow row;
        row.device_key = dev.key; row.key_is_mac = dev.key_is_mac;
        row.first_seen_ts = dev.first_seen_ts; row.last_seen_ts = dev.last_seen_ts;
        row.last_channel_hz = dev.last_channel_hz; row.last_phy = dev.last_phy;
        row.reading_count = int(dev.readings.size());
        if (!dev.readings.empty()) row.latest = dev.readings.back();
        row.identity = dev.identity; row.identity_phy = dev.identity_phy; row.identity_ts = dev.identity_ts;
        row.identity_count = dev.identity_count; row.monitored_channel_hz = dev.monitored_channel_hz;
        row.ssid_seen_ts = dev.ssid_seen_ts; row.wps_seen_ts = dev.wps_seen_ts;
        row.ssid_source = dev.ssid_source; row.wps_source = dev.wps_source;
        if (dev.key_is_mac) {
            auto vendor = wifi::lookup_vendor(dev.key);
            row.vendor = std::move(vendor.name); row.vendor_source = std::move(vendor.source);
        }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.last_seen_ts != b.last_seen_ts) return a.last_seen_ts > b.last_seen_ts;
        return a.device_key < b.device_key;
    });
    return rows;
}
} // namespace rfmon::wifi_master
