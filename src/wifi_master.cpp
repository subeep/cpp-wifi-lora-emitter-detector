#include "wifi_master.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>

namespace rfmon::wifi_master {

namespace {

std::string fmt_double(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.8g", v);
    return std::string(buf);
}

// Same tolerant field extraction as lora_master.cpp - a substring
// search for "key": followed by strtod/strtoll, not a general JSON
// parser (this project has no JSON dependency).
double parse_double_field(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t pos = line.find(needle);
    if (pos == std::string::npos) return 0.0;
    return std::strtod(line.c_str() + pos + needle.size(), nullptr);
}

int64_t parse_int64_field(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t pos = line.find(needle);
    if (pos == std::string::npos) return 0;
    return std::strtoll(line.c_str() + pos + needle.size(), nullptr, 10);
}

bool parse_bool_field(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    size_t start = pos + needle.size();
    return line.compare(start, 4, "true") == 0;
}

// Extracts a double-quoted string value: "key":"value". Stops at the
// first unescaped closing quote - device keys (MAC strings, WIFI-FP-*
// cluster ids) never contain a quote or backslash, so no escaping
// logic is needed.
std::string parse_string_field(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t pos = line.find(needle);
    if (pos == std::string::npos) return "";
    size_t start = pos + needle.size();
    size_t end = line.find('"', start);
    if (end == std::string::npos) return "";
    return line.substr(start, end - start);
}

std::string format_reading_line(const WifiMasterReading& r) {
    std::string out;
    out += "{\"ts\":" + std::to_string(r.ts);
    out += ",\"channel_hz\":" + std::to_string(std::llround(r.channel_hz));
    out += ",\"bandwidth_hz\":" + std::to_string(std::llround(r.bandwidth_hz));
    out += ",\"phy\":\"" + r.phy + "\"";
    out += ",\"cfo_ppm\":" + fmt_double(r.cfo_ppm);
    out += ",\"irr_db\":" + fmt_double(r.irr_db);
    out += ",\"iq_eps\":" + fmt_double(r.iq_eps);
    out += ",\"iq_phi_deg\":" + fmt_double(r.iq_phi_deg);
    out += ",\"dc_dbc\":" + fmt_double(r.dc_dbc);
    out += ",\"dc_ang_deg\":" + fmt_double(r.dc_ang_deg);
    out += ",\"snr_db\":" + fmt_double(r.snr_db);
    out += ",\"evm_pct\":" + fmt_double(r.evm_pct);
    out += ",\"sync_corr\":" + fmt_double(r.sync_corr);
    out += "}\n";
    return out;
}

std::string format_meta_line(const std::string& key, bool key_is_mac, int64_t first_seen_ts) {
    return "{\"meta\":1,\"key\":\"" + key + "\",\"key_is_mac\":" + (key_is_mac ? "true" : "false") +
           ",\"first_seen_ts\":" + std::to_string(first_seen_ts) + "}\n";
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Filesystem-safe filename from a device key - MAC strings and
// WIFI-FP-* cluster ids never contain '/', but replace it defensively
// rather than assume that always stays true.
std::string sanitize_filename(const std::string& key) {
    std::string out = key;
    std::replace(out.begin(), out.end(), '/', '_');
    return out;
}

}  // namespace

WifiMasterList::WifiMasterList(std::string dir) : dir_(std::move(dir)) { load_all_devices(); }

std::string WifiMasterList::device_path(const std::string& key) const {
    return dir_ + "/" + sanitize_filename(key) + ".ndjson";
}

void WifiMasterList::load_all_devices() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (!fs::exists(dir_)) return;

    for (const auto& entry : fs::directory_iterator(dir_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".ndjson") continue;
        std::ifstream in(entry.path());
        if (!in.is_open()) continue;

        Device dev;
        bool have_meta = false;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (!have_meta) {
                dev.key = parse_string_field(line, "key");
                dev.key_is_mac = parse_bool_field(line, "key_is_mac");
                dev.first_seen_ts = parse_int64_field(line, "first_seen_ts");
                have_meta = true;
                continue;
            }
            WifiMasterReading r;
            r.ts = parse_int64_field(line, "ts");
            r.channel_hz = parse_double_field(line, "channel_hz");
            r.bandwidth_hz = parse_double_field(line, "bandwidth_hz");
            r.phy = parse_string_field(line, "phy");
            r.cfo_ppm = parse_double_field(line, "cfo_ppm");
            r.irr_db = parse_double_field(line, "irr_db");
            r.iq_eps = parse_double_field(line, "iq_eps");
            r.iq_phi_deg = parse_double_field(line, "iq_phi_deg");
            r.dc_dbc = parse_double_field(line, "dc_dbc");
            r.dc_ang_deg = parse_double_field(line, "dc_ang_deg");
            r.snr_db = parse_double_field(line, "snr_db");
            r.evm_pct = parse_double_field(line, "evm_pct");
            r.sync_corr = parse_double_field(line, "sync_corr");
            dev.readings.push_back(r);
        }
        if (!have_meta || dev.key.empty() || dev.readings.empty()) continue;

        while (dev.readings.size() > size_t(MAX_READINGS_PER_DEVICE)) dev.readings.pop_front();

        dev.last_seen_ts = dev.readings.back().ts;
        dev.last_channel_hz = dev.readings.back().channel_hz;
        dev.last_phy = dev.readings.back().phy;

        if (!dev.key_is_mac) {
            // Cluster ids are "WIFI-FP-%04d" - recover the numeric
            // suffix to keep next_fp_id_ monotonic across a reload,
            // same pattern as lora_master.cpp's next_id_ re-seeding.
            size_t dash = dev.key.find_last_of('-');
            if (dash != std::string::npos) {
                int n = int(std::strtol(dev.key.c_str() + dash + 1, nullptr, 10));
                next_fp_id_ = std::max(next_fp_id_, n + 1);
            }
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

void WifiMasterList::append_reading_to_disk(const std::string& key,
                                             const WifiMasterReading& r) const {
    std::ofstream out(device_path(key), std::ios::app);
    if (!out.is_open()) return;
    out << format_reading_line(r);
}

void WifiMasterList::compact_device_file(const Device& dev) const {
    std::string tmp_path = device_path(dev.key) + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) return;
        out << format_meta_line(dev.key, dev.key_is_mac, dev.first_seen_ts);
        for (const auto& r : dev.readings) out << format_reading_line(r);
    }
    std::rename(tmp_path.c_str(), device_path(dev.key).c_str());
}

void WifiMasterList::record_reading(std::optional<std::string> mac, const std::string& phy,
                                     double channel_hz, double bandwidth_hz,
                                     const wifi_fingerprint::WifiFingerprint& fp, int64_t ts) {
    WifiMasterReading r;
    r.ts = ts;
    r.channel_hz = channel_hz;
    r.bandwidth_hz = bandwidth_hz;
    r.phy = phy;
    r.cfo_ppm = fp.cfo_ppm;
    r.irr_db = fp.irr_db;
    r.iq_eps = fp.iq_eps;
    r.iq_phi_deg = fp.iq_phi_deg;
    r.dc_dbc = fp.dc_dbc;
    r.dc_ang_deg = fp.dc_ang_deg;
    r.snr_db = fp.snr_db;
    r.evm_pct = fp.evm_pct;
    r.sync_corr = fp.sync_corr;

    std::lock_guard<std::mutex> lock(mutex_);

    int idx = mac.has_value() ? find_mac_match(*mac)
                               : find_fp_match(fp.irr_db, fp.dc_dbc, fp.iq_eps, fp.iq_phi_deg);

    if (idx < 0) {
        Device dev;
        if (mac.has_value()) {
            dev.key = *mac;
            dev.key_is_mac = true;
        } else {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "WIFI-FP-%04d", next_fp_id_++);
            dev.key = buf;
            dev.key_is_mac = false;
        }
        dev.first_seen_ts = ts;
        dev.last_seen_ts = ts;
        dev.last_channel_hz = channel_hz;
        dev.last_phy = phy;
        dev.readings.push_back(r);
        {
            std::ofstream out(device_path(dev.key), std::ios::app);
            if (out.is_open()) out << format_meta_line(dev.key, dev.key_is_mac, dev.first_seen_ts);
        }
        append_reading_to_disk(dev.key, r);
        devices_.push_back(std::move(dev));
        return;
    }

    Device& dev = devices_[size_t(idx)];
    dev.last_seen_ts = ts;
    dev.last_channel_hz = channel_hz;
    dev.last_phy = phy;
    dev.readings.push_back(r);
    append_reading_to_disk(dev.key, r);
    if (dev.readings.size() > size_t(COMPACT_TRIGGER_READINGS)) {
        while (dev.readings.size() > size_t(MAX_READINGS_PER_DEVICE)) dev.readings.pop_front();
        compact_device_file(dev);
    }
}

std::vector<WifiMasterRow> WifiMasterList::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WifiMasterRow> rows;
    rows.reserve(devices_.size());
    for (const auto& dev : devices_) {
        WifiMasterRow row;
        row.device_key = dev.key;
        row.key_is_mac = dev.key_is_mac;
        row.first_seen_ts = dev.first_seen_ts;
        row.last_seen_ts = dev.last_seen_ts;
        row.last_channel_hz = dev.last_channel_hz;
        row.last_phy = dev.last_phy;
        row.reading_count = int(dev.readings.size());
        if (!dev.readings.empty()) row.latest = dev.readings.back();
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const WifiMasterRow& a, const WifiMasterRow& b) {
        return a.last_seen_ts > b.last_seen_ts;
    });
    return rows;
}

}  // namespace rfmon::wifi_master
