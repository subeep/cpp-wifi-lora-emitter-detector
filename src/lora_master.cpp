#include "lora_master.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>

namespace rfmon::lora_master {

namespace {

std::string fmt_double(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.8g", v);
    return std::string(buf);
}

// Tolerant field extraction from a line this same file wrote - not a
// general JSON parser (this project has no JSON dependency, matching
// fingerprint.cpp's own hand-rolled NDJSON writer), just a substring
// search for "key": followed by strtod/strtoll, which stop at the
// first non-numeric character (comma or '}') on their own.
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

std::string format_reading_line(const LoraMasterReading& r) {
    std::string out;
    out += "{\"ts\":" + std::to_string(r.ts);
    out += ",\"freq_hz\":" + std::to_string(std::llround(r.freq_hz));
    out += ",\"sf\":" + std::to_string(r.sf);
    out += ",\"bw_hz\":" + std::to_string(std::llround(r.bw_hz));
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

std::string format_meta_line(int id, int64_t first_seen_ts) {
    return "{\"meta\":1,\"device_id\":" + std::to_string(id) +
           ",\"first_seen_ts\":" + std::to_string(first_seen_ts) + "}\n";
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

LoraMasterList::LoraMasterList(std::string dir) : dir_(std::move(dir)) { load_all_devices(); }

std::string LoraMasterList::device_path(int id) const {
    return dir_ + "/" + std::to_string(id) + ".ndjson";
}

void LoraMasterList::load_all_devices() {
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
                dev.id = int(parse_int64_field(line, "device_id"));
                dev.first_seen_ts = parse_int64_field(line, "first_seen_ts");
                have_meta = true;
                continue;
            }
            LoraMasterReading r;
            r.ts = parse_int64_field(line, "ts");
            r.freq_hz = parse_double_field(line, "freq_hz");
            r.sf = int(parse_int64_field(line, "sf"));
            r.bw_hz = parse_double_field(line, "bw_hz");
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
        // A device with no meta line or zero readings is a corrupt/
        // half-written file (e.g. killed mid-first-write, before the
        // atomic-rename compaction path was ever involved) - skip it
        // rather than resurrecting a broken entry.
        if (!have_meta || dev.readings.empty()) continue;

        // A crash could in principle leave more than MAX_READINGS_PER_DEVICE
        // on disk if it happened between hitting COMPACT_TRIGGER_READINGS
        // and the compaction rename landing - trim in memory either way.
        while (dev.readings.size() > size_t(MAX_READINGS_PER_DEVICE)) dev.readings.pop_front();

        dev.last_seen_ts = dev.readings.back().ts;
        dev.last_freq_hz = dev.readings.back().freq_hz;
        dev.last_sf = dev.readings.back().sf;
        dev.last_bw_hz = dev.readings.back().bw_hz;
        next_id_ = std::max(next_id_, dev.id + 1);
        devices_.push_back(std::move(dev));
    }
}

int LoraMasterList::find_match(double irr_db, double dc_dbc, double iq_eps,
                                double iq_phi_deg) const {
    int best_idx = -1;
    double best_dist = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < devices_.size(); ++i) {
        const Device& dev = devices_[i];
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

        // Multiple devices can qualify within tolerance - break ties by
        // picking the closest one. The iq_eps term is scaled up since
        // its tolerance (0.02) is on a much smaller numeric scale than
        // the dB/degree ones; this only affects the tie-break, not
        // whether a candidate qualifies at all.
        double dist = std::abs(irr_db - m_irr) + std::abs(dc_dbc - m_dc) +
                      std::abs(iq_eps - m_eps) * 100.0 + std::abs(iq_phi_deg - m_phi);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = int(i);
        }
    }
    return best_idx;
}

void LoraMasterList::append_reading_to_disk(int id, const LoraMasterReading& r) const {
    std::ofstream out(device_path(id), std::ios::app);
    if (!out.is_open()) return;
    out << format_reading_line(r);
}

void LoraMasterList::compact_device_file(const Device& dev) const {
    std::string tmp_path = device_path(dev.id) + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) return;
        out << format_meta_line(dev.id, dev.first_seen_ts);
        for (const auto& r : dev.readings) out << format_reading_line(r);
    }
    std::rename(tmp_path.c_str(), device_path(dev.id).c_str());
}

void LoraMasterList::record_reading(double freq_hz, int sf, double bw_hz,
                                     const fingerprint::LoraFingerprint& fp, int64_t ts) {
    LoraMasterReading r;
    r.ts = ts;
    r.freq_hz = freq_hz;
    r.sf = sf;
    r.bw_hz = bw_hz;
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
    int idx = find_match(fp.irr_db, fp.dc_dbc, fp.iq_eps, fp.iq_phi_deg);
    if (idx < 0) {
        Device dev;
        dev.id = next_id_++;
        dev.first_seen_ts = ts;
        dev.last_seen_ts = ts;
        dev.last_freq_hz = freq_hz;
        dev.last_sf = sf;
        dev.last_bw_hz = bw_hz;
        dev.readings.push_back(r);
        {
            std::ofstream out(device_path(dev.id), std::ios::app);
            if (out.is_open()) out << format_meta_line(dev.id, dev.first_seen_ts);
        }
        append_reading_to_disk(dev.id, r);
        devices_.push_back(std::move(dev));
        return;
    }

    Device& dev = devices_[size_t(idx)];
    dev.last_seen_ts = ts;
    dev.last_freq_hz = freq_hz;
    dev.last_sf = sf;
    dev.last_bw_hz = bw_hz;
    dev.readings.push_back(r);
    append_reading_to_disk(dev.id, r);
    if (dev.readings.size() > size_t(COMPACT_TRIGGER_READINGS)) {
        while (dev.readings.size() > size_t(MAX_READINGS_PER_DEVICE)) dev.readings.pop_front();
        compact_device_file(dev);
    }
}

std::vector<LoraMasterRow> LoraMasterList::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LoraMasterRow> rows;
    rows.reserve(devices_.size());
    for (const auto& dev : devices_) {
        LoraMasterRow row;
        row.device_id = dev.id;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "LORA-%04d", dev.id);
        row.device_id_str = buf;
        row.first_seen_ts = dev.first_seen_ts;
        row.last_seen_ts = dev.last_seen_ts;
        row.last_freq_hz = dev.last_freq_hz;
        row.last_sf = dev.last_sf;
        row.last_bw_hz = dev.last_bw_hz;
        row.reading_count = int(dev.readings.size());
        if (!dev.readings.empty()) row.latest = dev.readings.back();
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const LoraMasterRow& a, const LoraMasterRow& b) {
                  return a.last_seen_ts > b.last_seen_ts;
              });
    return rows;
}

}  // namespace rfmon::lora_master
