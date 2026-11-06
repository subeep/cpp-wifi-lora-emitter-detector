#include "lora_export.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rfmon::lora_export {
namespace {

std::string cell(const std::string& v) {
    if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
    std::string out = "\"";
    for (char c : v) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}
std::string num(double v, const char* f = "%.10g") {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
template <class T> std::string opt(const std::optional<T>& v) {
    if (!v) return "";
    if constexpr (std::is_same_v<T, bool>) return *v ? "true" : "false";
    else if constexpr (std::is_same_v<T, std::string>) return *v;
    else return num(double(*v));
}
template <class T> nlohmann::json jopt(const std::optional<T>& v) {
    return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
}
std::string line(const std::vector<std::string>& cells) {
    std::string s;
    for (size_t i = 0; i < cells.size(); ++i) s += (i ? "," : "") + cell(cells[i]);
    return s + "\n";
}
std::string iso(int64_t ts) {
    if (ts <= 0) return "";
    std::time_t t = std::time_t(ts);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

}  // namespace

std::string packets_csv(const std::vector<LoraPacketRow>& rows) {
    std::string s = line({"time", "status", "decoder", "header_valid", "payload_complete", "crc", "freq_mhz", "sf",
                          "bandwidth_khz", "cr", "payload_len", "ldro", "sync_word", "inverted_iq", "preamble_peak_ratio",
                          "sfd_peak_ratio", "cfo_hz", "drift_hz_per_symbol", "capture_offset_s", "fec_disagreements",
                          "reception_id", "reception_hypotheses", "rf_cluster_id", "lorawan_candidate",
                          "lorawan_device_kind", "lorawan_device_id", "lorawan_detail", "payload_hex", "snr_db",
                          "evm_pct", "sync_corr", "cfo_ppm", "irr_db", "iq_eps", "iq_phi_deg", "dc_dbc", "dc_ang_deg",
                          "fingerprint_gate", "detail"});
    for (const auto& r : rows)
        s += line({r.time, r.status, r.decoder, r.header_valid ? "true" : "false", r.payload_complete ? "true" : "false",
                   lora_crc_label(r.crc), num(r.freq_mhz, "%.6f"), std::to_string(r.sf), num(r.bandwidth_khz),
                   r.cr ? "4/" + std::to_string(4 + *r.cr) : "", opt(r.payload_len), opt(r.ldro), opt(r.sync_word),
                   opt(r.inverted_iq), opt(r.preamble_peak_ratio), opt(r.sfd_peak_ratio), opt(r.cfo_hz),
                   opt(r.drift_hz_per_symbol), opt(r.capture_offset_s), opt(r.fec_disagreements), r.reception_id,
                   std::to_string(r.reception_hypotheses), opt(r.rf_cluster_id), opt(r.lorawan_candidate),
                   opt(r.lorawan_device_kind), opt(r.lorawan_device_id), opt(r.lorawan_detail), r.payload_hex,
                   opt(r.fp_snr_db), opt(r.fp_evm_pct), opt(r.fp_sync_corr), opt(r.fp_cfo_ppm), opt(r.fp_irr_db),
                   opt(r.fp_iq_eps), opt(r.fp_iq_phi_deg), opt(r.fp_dc_dbc), opt(r.fp_dc_ang_deg),
                   opt(r.fp_gate_reason), r.detail});
    return s;
}

std::string packets_json(const std::vector<LoraPacketRow>& rows) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& r : rows) {
        out.push_back({{"time", r.time}, {"status", r.status}, {"decoder", r.decoder}, {"header_valid", r.header_valid},
                       {"payload_complete", r.payload_complete}, {"crc", lora_crc_label(r.crc)},
                       {"freq_mhz", r.freq_mhz}, {"sf", r.sf}, {"bandwidth_khz", r.bandwidth_khz}, {"cr", jopt(r.cr)},
                       {"payload_len", jopt(r.payload_len)}, {"ldro", jopt(r.ldro)}, {"ldro_ambiguous", r.ldro_ambiguous},
                       {"sync_word", jopt(r.sync_word)}, {"inverted_iq", jopt(r.inverted_iq)},
                       {"preamble_peak_ratio", jopt(r.preamble_peak_ratio)}, {"sfd_peak_ratio", jopt(r.sfd_peak_ratio)},
                       {"cfo_hz", jopt(r.cfo_hz)}, {"drift_hz_per_symbol", jopt(r.drift_hz_per_symbol)},
                       {"capture_offset_s", jopt(r.capture_offset_s)}, {"fec_disagreements", jopt(r.fec_disagreements)},
                       {"reception_id", r.reception_id}, {"reception_hypotheses", r.reception_hypotheses},
                       {"rf_cluster_id", jopt(r.rf_cluster_id)}, {"lorawan_candidate", jopt(r.lorawan_candidate)},
                       {"lorawan_device_kind", jopt(r.lorawan_device_kind)}, {"lorawan_device_id", jopt(r.lorawan_device_id)},
                       {"lorawan_detail", jopt(r.lorawan_detail)}, {"payload_hex", r.payload_hex},
                       {"payload_text", jopt(r.payload_repr)},
                       {"fingerprint", {{"snr_db", jopt(r.fp_snr_db)}, {"evm_pct", jopt(r.fp_evm_pct)},
                                        {"sync_corr", jopt(r.fp_sync_corr)}, {"cfo_ppm", jopt(r.fp_cfo_ppm)},
                                        {"irr_db", jopt(r.fp_irr_db)}, {"iq_eps", jopt(r.fp_iq_eps)},
                                        {"iq_phi_deg", jopt(r.fp_iq_phi_deg)}, {"dc_dbc", jopt(r.fp_dc_dbc)},
                                        {"dc_ang_deg", jopt(r.fp_dc_ang_deg)}, {"gate_reason", jopt(r.fp_gate_reason)}}},
                       {"detail", r.detail}});
    }
    return out.dump(1) + "\n";
}

std::string devices_csv(const std::vector<LoraDeviceRow>& rows) {
    std::string s = line({"kind", "id", "frames", "uplinks", "downlinks", "first_seen_utc", "last_seen_utc",
                          "last_freq_mhz", "last_sf", "last_bw_khz", "last_type", "last_fcnt16", "rf_clusters"});
    for (const auto& r : rows) {
        std::string clusters;
        for (int c : r.rf_clusters) clusters += (clusters.empty() ? "" : " ") + std::to_string(c);
        s += line({r.kind, r.id, std::to_string(r.frames), std::to_string(r.uplinks), std::to_string(r.downlinks),
                   iso(r.first_seen_ts), iso(r.last_seen_ts), num(r.last_freq_hz / 1e6, "%.6f"), std::to_string(r.last_sf),
                   num(r.last_bw_hz / 1e3), r.last_type, opt(r.last_fcnt16), clusters});
    }
    return s;
}

std::string emitters_csv(const std::vector<lora_master::LoraMasterRow>& rows) {
    std::string s = line({"device_id", "alias", "policy", "note", "first_seen_utc", "last_seen_utc", "readings",
                          "last_freq_mhz", "last_sf", "last_bw_khz", "cfo_ppm", "irr_db", "iq_eps", "iq_phi_deg",
                          "dc_dbc", "snr_db"});
    for (const auto& r : rows)
        s += line({r.device_id_str, r.enrolment.alias, r.enrolment.policy, r.enrolment.note, iso(r.first_seen_ts),
                   iso(r.last_seen_ts), std::to_string(r.reading_count), num(r.last_freq_hz / 1e6, "%.6f"),
                   std::to_string(r.last_sf), num(r.last_bw_hz / 1e3), num(r.latest.cfo_ppm), num(r.latest.irr_db),
                   num(r.latest.iq_eps), num(r.latest.iq_phi_deg), num(r.latest.dc_dbc), num(r.latest.snr_db)});
    return s;
}

std::string history_csv(const std::vector<lora_master::LoraMasterReading>& readings) {
    std::string s = line({"ts_utc", "freq_mhz", "sf", "bw_khz", "cfo_ppm", "irr_db", "iq_eps", "iq_phi_deg", "dc_dbc",
                          "dc_ang_deg", "snr_db", "evm_pct", "sync_corr"});
    for (const auto& r : readings)
        s += line({iso(r.ts), num(r.freq_hz / 1e6, "%.6f"), std::to_string(r.sf), num(r.bw_hz / 1e3), num(r.cfo_ppm),
                   num(r.irr_db), num(r.iq_eps), num(r.iq_phi_deg), num(r.dc_dbc), num(r.dc_ang_deg), num(r.snr_db),
                   num(r.evm_pct), num(r.sync_corr)});
    return s;
}

std::string incidents_csv(const std::vector<lora_rules::Incident>& incidents) {
    std::string s = line({"id", "first_utc", "last_utc", "rule", "severity", "subject", "count", "open", "summary", "evidence"});
    for (const auto& i : incidents)
        s += line({std::to_string(i.id), iso(int64_t(i.first_ts)), iso(int64_t(i.last_ts)), i.rule, i.severity, i.subject,
                   std::to_string(i.count), i.open ? "true" : "false", i.summary, i.evidence});
    return s;
}

std::string default_directory() {
#ifdef PROJECT_ROOT_DIR
    return std::string(PROJECT_ROOT_DIR) + "/data/lora_exports";
#else
    return "lora_exports";
#endif
}

std::string write(const std::string& directory, const std::string& stem, const std::string& ext,
                  const std::string& content) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) throw std::runtime_error("Cannot create " + directory + ": " + ec.message());
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    std::string path = directory + "/" + stem + "-" + stamp + "." + ext;
    for (int n = 2; fs::exists(path, ec) && n < 100; ++n)
        path = directory + "/" + stem + "-" + stamp + "-" + std::to_string(n) + "." + ext;
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("Cannot write " + path);
    out << content;
    if (!out) throw std::runtime_error("Write failed: " + path);
    return path;
}

}  // namespace rfmon::lora_export
