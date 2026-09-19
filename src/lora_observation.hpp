#pragma once
#include <string>
#include <optional>
#include <vector>
#include <complex>
#include "lora_phy_std.hpp"

namespace rfmon {
// One SF/BW hypothesis: preamble evidence or a decoder result.
// CRC and payload completeness are independent of header validity.
struct LoraPacketRow {
    std::string time;      // HH:MM:SS
    std::string status;    // explicit decode outcome, never a protocol identity
    std::string decoder = "Preamble detector";
    std::string detail;
    bool header_valid = false;
    bool payload_complete = false;
    enum class Crc { NotChecked, Absent, Failed, Valid };
    Crc crc = Crc::NotChecked;
    std::string payload_hex;
    double freq_mhz = 0;
    int sf = 0;
    double bandwidth_khz = 0;  // which of LORA_LISTEN_BW_LIST_HZ this was found at -
                            // a real hypothesis that matched, not a measurement
    std::optional<int> cr;
    std::optional<int> payload_len;
    std::optional<bool> crc_valid;
    // Payload LDRO is inferred, not transmitted in the explicit PHY header.
    std::optional<bool> ldro;
    bool ldro_ambiguous = false;
    std::optional<int> sync_word; // observation only, never a network/device ID
    std::optional<bool> sync_check_skipped; // legacy laboratory codec only
    int cfo_bins = 0;
    std::optional<std::string> payload_repr;

    // RF fingerprint (see fingerprint.hpp) - the "stable core" Tier-1
    // parameters plus the SNR they were gated on. Unset when extraction
    // was gated out (e.g. below the SNR floor) or not attempted for
    // this row (only the "detected" path currently extracts one - see
    // run_lora_listen_step()).
    std::optional<double> fp_cfo_ppm;
    std::optional<double> fp_irr_db;
    std::optional<double> fp_iq_eps;
    std::optional<double> fp_iq_phi_deg;
    std::optional<double> fp_dc_dbc;
    std::optional<double> fp_dc_ang_deg;
    std::optional<double> fp_snr_db;
    std::optional<std::string> fp_gate_reason;  // set only when gated out

    // Fit-quality covariates - shown regardless of gate outcome, once
    // actually computed (unset only if gating happened before the fit
    // ever ran, e.g. the SNR floor). A high SNR alone doesn't mean the
    // (SF, BW) hypothesis was correct - these reveal whether the fit
    // actually explains the signal, which is exactly what the
    // LORA_EVM_CEILING_PCT/LORA_SYNC_CORR_FLOOR gate uses them for.
    std::optional<double> fp_evm_pct;
    std::optional<double> fp_sync_corr;
};

const char* lora_crc_label(LoraPacketRow::Crc crc);
void set_lora_integrity(LoraPacketRow& row, bool complete, bool crc_on, bool crc_valid);
// Production defaults to the receive-only explicit PHY. Legacy codecs are
// available only through the explicit laboratory flag; never an auto fallback.
std::vector<LoraPacketRow> analyze_lora_packets(const std::vector<std::complex<float>>& iq,
                                                int sf, double bandwidth, bool laboratory_mode = false);
// Convenience wrapper for 125 kHz input, returning only the first observation.
std::optional<LoraPacketRow> analyze_lora_hypothesis(const std::vector<std::complex<float>>& iq, int sf,
                                                       bool laboratory_mode = false);
std::vector<LoraPacketRow> analyze_lora_capture(const std::vector<std::complex<float>>& iq, double rate,
                                                 double frequency, bool laboratory_mode = false);
} // namespace rfmon
