#include "lora_observation.hpp"
#include "lora_phy.hpp"
#include "config.hpp"
#include <cmath>
#include <cstdio>

namespace rfmon {
const char* lora_crc_label(LoraPacketRow::Crc crc) {
    switch (crc) {
        case LoraPacketRow::Crc::Valid: return "Valid";
        case LoraPacketRow::Crc::Failed: return "Failed";
        case LoraPacketRow::Crc::Absent: return "Absent";
        default: return "Not checked";
    }
}
void set_lora_integrity(LoraPacketRow& row, bool complete, bool crc_on, bool valid) {
    row.header_valid = true;
    row.payload_complete = complete;
    row.crc_valid.reset();
    row.crc = !complete ? LoraPacketRow::Crc::NotChecked : !crc_on ? LoraPacketRow::Crc::Absent :
              valid ? LoraPacketRow::Crc::Valid : LoraPacketRow::Crc::Failed;
    if (complete && crc_on) row.crc_valid = valid;
    row.status = !complete ? "Header only" : !crc_on ? "Payload / no CRC" :
                 valid ? "Payload / CRC valid" : "Payload / CRC failed";
    row.detail = !complete ? "Header accepted; insufficient payload samples. CRC not checked." :
                 !crc_on ? "Payload recovered; header indicates no payload CRC. Integrity unverified." :
                 valid ? "Payload CRC matches this decoder. Protocol and device identity not established." :
                 "Payload CRC failed. Bytes are unverified and must not establish identity.";
}
namespace {
void payload_text(LoraPacketRow& row, const std::vector<uint8_t>& bytes) {
    row.payload_repr = "";
    for (uint8_t b : bytes) {
        char hex[4]; std::snprintf(hex, sizeof(hex), "%02x", b);
        if (!row.payload_hex.empty()) row.payload_hex += ' ';
        row.payload_hex += hex;
        *row.payload_repr += b >= 32 && b < 127 ? char(b) : '.';
    }
}
}
std::optional<LoraPacketRow> analyze_lora_hypothesis(const std::vector<std::complex<float>>& iq, int sf,
                                                       bool skip_sync_check) {
    auto standard = lora::std_phy::demodulate(iq, sf, skip_sync_check);
    if (standard && standard->header_valid) {
        LoraPacketRow row;
        row.decoder = "SX reference (unvalidated OTA)";
        row.sf = sf; row.cr = standard->cr; row.cfo_bins = standard->cfo_bins;
        row.payload_len = standard->declared_payload_len;
        row.ldro = standard->ldro;
        row.sync_check_skipped = standard->sync_check_skipped;
        set_lora_integrity(row, standard->payload_complete, standard->crc_on, standard->crc_valid);
        if (standard->payload_complete) payload_text(row, standard->payload);
        return row;
    }
    auto internal = lora::demodulate(iq, sf);
    if (internal && internal->header_valid) {
        LoraPacketRow row;
        row.decoder = "Internal codec (nonstandard)";
        row.sf = sf; row.cr = internal->cr; row.cfo_bins = internal->cfo_bins;
        row.payload_len = int(internal->payload.size());
        set_lora_integrity(row, true, true, internal->crc_valid);
        payload_text(row, internal->payload);
        return row;
    }
    auto burst = lora::detect_burst(iq, sf);
    if (!burst) return std::nullopt;
    LoraPacketRow row;
    row.sf = sf; row.cfo_bins = burst->cfo_bins;
    row.status = "Detected only";
    // standard->sync_check_skipped is set whenever the SX-reference
    // decoder ran at all (even on failure) - if it's true here, the
    // sync-word gate was already bypassed and STILL no header
    // validated, which rules out that gate as this hypothesis's blocker.
    bool bypassed_and_still_failed = standard && standard->sync_check_skipped;
    row.detail = bypassed_and_still_failed
                     ? "Sync-word check bypassed; still no supported decoder recovered a valid header."
                 : standard ? "SX header rejected; no supported decoder recovered a valid header."
                            : "Preamble detected; synchronization/header decoding did not complete. Cause unresolved.";
    return row;
}
std::vector<LoraPacketRow> analyze_lora_capture(const std::vector<std::complex<float>>& raw, double rate,
                                                 double frequency, bool skip_sync_check) {
    std::vector<LoraPacketRow> rows;
    for (double bw : LORA_LISTEN_BW_LIST_HZ) {
        // The codecs require sample_rate == hypothesized bandwidth. Do not silently
        // round an incompatible rate and present the resulting SF/BW as a decode.
        double ratio = rate / bw;
        if (!std::isfinite(ratio) || ratio < 1 || ratio > 1024 || std::abs(ratio - std::round(ratio)) > 1e-6) continue;
        size_t factor = size_t(std::llround(ratio));
        std::vector<std::complex<float>> iq(raw.size() / factor);
        for (size_t i = 0; i < iq.size(); ++i) {
            for (size_t j = 0; j < factor; ++j) iq[i] += raw[i * factor + j];
            iq[i] /= float(factor);
        }
        for (int sf : LORA_LISTEN_SF_LIST) {
            auto row = analyze_lora_hypothesis(iq, sf, skip_sync_check);
            if (!row) continue;
            row->freq_mhz = frequency / 1e6; row->bandwidth_khz = bw / 1e3;
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}
} // namespace rfmon
