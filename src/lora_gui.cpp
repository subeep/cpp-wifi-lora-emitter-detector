#include "lora_gui.hpp"
#include "fingerprint.hpp"
#include "lora_capture.hpp"
#include "imgui.h"
#include <future>
#include <chrono>

namespace rfmon {
void draw_lora_security_panel(const lora_security::Snapshot& s) {
    if(!ImGui::CollapsingHeader("LoRa security monitoring"))return;
    ImGui::TextWrapped("Capture evidence only. LoRa replay/flood attack rules are not implemented. Supported decode: explicit PHY SF7-12, BW125/250/500 kHz when the capture rate permits.");
    ImGui::TextWrapped("Latest capture: %s",s.health.c_str());
    ImGui::Text("Captures: %llu | Excluded: %llu | Device-timed: %llu",(unsigned long long)s.captures,
        (unsigned long long)s.excluded,(unsigned long long)s.device_timed);
    ImGui::Text("Sampled: %.3f s | Analysed: %.3f s | Usable supported-PHY exposure: %.3f s",s.sampled_s,s.analysed_s,s.usable_s);
    ImGui::Text("Events: %llu | Eligible protocol inputs: %llu | Failed: %llu | CRC absent: %llu | Partial: %llu",
        (unsigned long long)s.events,(unsigned long long)s.eligible,(unsigned long long)s.crc_failed,
        (unsigned long long)s.crc_absent,(unsigned long long)s.partial);
    ImGui::Text("Duplicate hypotheses: %llu | Lost capture batches: %llu | Rejected inputs: %llu",
        (unsigned long long)s.hypotheses_deduplicated,(unsigned long long)s.input_loss,(unsigned long long)s.rejected_inputs);
    ImGui::TextWrapped("Capture coverage applies to supported decoder hypotheses; it does not establish packet recall. Sample-start indices have coarse preamble uncertainty. CRC-valid bytes do not authenticate LoRaWAN MICs or identify a physical device.");
    ImGui::Text("Unsampled device time: %.3f s | Unknown gaps: %llu | Time regressions: %llu",s.dead_s,
        (unsigned long long)s.unknown_gaps,(unsigned long long)s.time_regressions);
    if(!s.coverage.empty()&&ImGui::BeginTable("lora_security_coverage",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
        for(const char* label:{"Frequency MHz","Rate ksps","Gain dB","Sampled s","Usable s"})ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        for(const auto& [key,c]:s.coverage) {
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::Text("%.3f",c.center_hz/1e6);
            ImGui::TableSetColumnIndex(1);ImGui::Text("%.1f",c.rate_hz/1e3);
            ImGui::TableSetColumnIndex(2);if(c.gain_db)ImGui::Text("%.1f",*c.gain_db);else ImGui::TextUnformatted("AGC/unknown");
            ImGui::TableSetColumnIndex(3);ImGui::Text("%.3f",c.sampled_s);ImGui::TableSetColumnIndex(4);ImGui::Text("%.3f",c.usable_s);
        }
        ImGui::EndTable();
    }
    if(!s.recording_error.empty())ImGui::TextWrapped("Recording error: %s",s.recording_error.c_str());
    if(s.captures)ImGui::Text("Last capture: #%llu | %.3f MHz | %.1f ksps | session %llu",
        (unsigned long long)s.latest.capture_seq,s.latest.center_hz/1e6,s.latest.rate_hz/1e3,(unsigned long long)s.latest.radio_session);
}

void draw_lora_packet_table(const std::vector<LoraPacketRow>& packets, float height) {
    if (packets.empty()) {
        ImGui::TextDisabled("No LoRa packets observed yet.");
        return;
    }

    size_t headers=0, verified_count=0, partial=0, failed=0;
    for (const auto& row : packets) {
        headers += row.header_valid;
        verified_count += row.crc == LoraPacketRow::Crc::Valid;
        partial += row.header_valid && !row.payload_complete;
        failed += row.crc == LoraPacketRow::Crc::Failed;
    }
    ImGui::Text("Retained observations: %zu | Valid headers: %zu | CRC valid: %zu | Partial: %zu | CRC failed: %zu",
                packets.size(), headers, verified_count, partial, failed);
    ImGui::TextDisabled("SF/BW/polarity hypotheses; counts are not unique transmitters or delivery rates.");
    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_ScrollX;
    ImVec2 outer_size(0.0f, height);
    if (!ImGui::BeginTable("lora_packets", 31, flags, outer_size)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Decode outcome", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("Decoder", ImGuiTableColumnFlags_WidthFixed, 235.0f);
    ImGui::TableSetupColumn("Header", ImGuiTableColumnFlags_WidthFixed, 85.0f);
    ImGui::TableSetupColumn("Freq (MHz)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("SF", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("BW guess (kHz)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("CR", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("LDRO", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Sync observed", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Header len", ImGuiTableColumnFlags_WidthFixed, 85.0f);
    ImGui::TableSetupColumn("Payload CRC", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("CFO (bins)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    // --- RF fingerprint (see fingerprint.hpp) - Tier-1 "stable core"
    // parameters, only populated on a "Detected" row (see
    // run_lora_listen_step()). SNR doubles as the gate-reason column
    // when extraction was rejected, since that's the most common gate.
    ImGui::TableSetupColumn("SNR (dB)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    // Fit-quality covariates (source spec §5/§1.6) - what the
    // LORA_EVM_CEILING_PCT/LORA_SYNC_CORR_FLOOR gate uses to reject a
    // wrong-hypothesis match that still had plenty of raw SNR behind
    // it. Shown even on a gated row when they were actually computed.
    ImGui::TableSetupColumn("EVM (%)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Sync corr", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("CFO (ppm)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("IRR (dB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ eps", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("IQ phi (deg)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("DC (dBc)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("DC ang (deg)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthFixed, 360.0f);
    ImGui::TableSetupColumn("IQ polarity", ImGuiTableColumnFlags_WidthFixed, 95.0f);
    ImGui::TableSetupColumn("Preamble peak", ImGuiTableColumnFlags_WidthFixed, 105.0f);
    ImGui::TableSetupColumn("SFD peak", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("CFO estimate (Hz)", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("Drift (Hz/symbol)", ImGuiTableColumnFlags_WidthFixed, 135.0f);
    ImGui::TableSetupColumn("Capture offset (s)", ImGuiTableColumnFlags_WidthFixed, 135.0f);
    ImGui::TableSetupColumn("FEC mismatches", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("LoRaWAN structure", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableHeadersRow();

    ImVec4 dim(0.55f, 0.55f, 0.58f, 1.0f);
    ImVec4 good(0.25f, 0.73f, 0.31f, 1.0f);
    ImVec4 bad(0.85f, 0.30f, 0.28f, 1.0f);

    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        const auto& d = *it;
        bool verified = d.crc == LoraPacketRow::Crc::Valid;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.time.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(verified ? good : d.crc == LoraPacketRow::Crc::Failed ? bad : dim, "%s", d.status.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.detail.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.decoder.c_str());
        ImGui::TableNextColumn();
        bool sync_bypassed = d.sync_check_skipped.value_or(false);
        if (d.header_valid && sync_bypassed) {
            // Distinct from `good` - this header is checksum-valid but
            // NOT sync-word-verified (the diagnostic toggle was on), so
            // it gets its own color rather than looking like a normal
            // fully-verified "Valid".
            ImVec4 caution(0.86f, 0.65f, 0.13f, 1.0f);
            ImGui::TextColored(caution, "Valid*");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Header checksum passed, but the sync-word check was bypassed "
                                  "(diagnostic toggle) - NOT cross-checked against the sync word. "
                                  "Weaker evidence than a normal \"Valid\".");
        } else {
            ImGui::TextUnformatted(d.header_valid ? "Valid" : "Unverified");
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", d.freq_mhz);
        ImGui::TableNextColumn();
        ImGui::Text("%d", d.sf);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", d.bandwidth_khz);
        ImGui::TableNextColumn();
        if (d.cr.has_value()) ImGui::Text("4/%d", 4 + *d.cr); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.ldro.has_value()) {
            ImGui::Text("%s%s", *d.ldro ? "On" : "Off", d.ldro_ambiguous ? "?" : "");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Payload LDRO is inferred. '?' means the selected hypothesis is not resolved by a valid payload CRC.");
        } else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.sync_word) ImGui::Text("0x%02X", *d.sync_word); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Observed sync symbols; not a protocol or device identity. Other values are accepted.");
        ImGui::TableNextColumn();
        if (d.payload_len.has_value()) ImGui::Text("%d", *d.payload_len); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        ImGui::TextColored(verified ? good : d.crc == LoraPacketRow::Crc::Failed ? bad : dim,
                           "%s", lora_crc_label(d.crc));
        ImGui::TableNextColumn();
        ImGui::Text("%d", d.cfo_bins);
        ImGui::TableNextColumn();
        if (d.fp_snr_db.has_value()) {
            ImGui::Text("%.1f", *d.fp_snr_db);
        } else if (d.fp_gate_reason.has_value()) {
            ImGui::TextColored(bad, "%s", d.fp_gate_reason->c_str());
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        if (d.fp_evm_pct.has_value()) {
            bool bad_evm = *d.fp_evm_pct > fingerprint::LORA_EVM_CEILING_PCT;
            ImGui::TextColored(bad_evm ? bad : good, "%.2f", *d.fp_evm_pct);
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        if (d.fp_sync_corr.has_value()) {
            bool bad_sync = *d.fp_sync_corr < fingerprint::LORA_SYNC_CORR_FLOOR;
            ImGui::TextColored(bad_sync ? bad : good, "%.3f", *d.fp_sync_corr);
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        if (d.fp_cfo_ppm.has_value()) ImGui::Text("%.3f", *d.fp_cfo_ppm); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_irr_db.has_value()) ImGui::Text("%.2f", *d.fp_irr_db); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_iq_eps.has_value()) ImGui::Text("%.5f", *d.fp_iq_eps); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_iq_phi_deg.has_value()) ImGui::Text("%.3f", *d.fp_iq_phi_deg); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_dc_dbc.has_value()) ImGui::Text("%.1f", *d.fp_dc_dbc); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.fp_dc_ang_deg.has_value()) ImGui::Text("%.1f", *d.fp_dc_ang_deg); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.payload_repr.has_value()) {
            ImGui::TextUnformatted(d.payload_hex.empty() ? "(empty payload)" : d.payload_hex.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("ASCII: %s\n%s", d.payload_repr->c_str(), d.detail.c_str());
        } else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.inverted_iq) ImGui::TextUnformatted(*d.inverted_iq ? "Inverted" : "Normal"); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Polarity hypothesis used to recover the header; does not establish uplink/downlink or LoRaWAN.");
        ImGui::TableNextColumn();
        if (d.preamble_peak_ratio) ImGui::Text("%.3f", *d.preamble_peak_ratio); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aligned dechirped FFT peak / sum of sample magnitudes. Correlation evidence, not probability or SNR.");
        ImGui::TableNextColumn();
        if (d.sfd_peak_ratio) ImGui::Text("%.3f", *d.sfd_peak_ratio); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Weaker peak ratio of the two aligned SFD downchirps.");
        ImGui::TableNextColumn();
        if (d.cfo_hz) ImGui::Text("%.2f", *d.cfo_hz); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fractional preamble offset estimate in captured-IQ convention; includes receiver and residual timing error. Not calibrated transmitter frequency.");
        ImGui::TableNextColumn();
        if (d.drift_hz_per_symbol) ImGui::Text("%.3f", *d.drift_hz_per_symbol); else ImGui::TextDisabled("--");
        ImGui::TableNextColumn();
        if (d.capture_offset_s) ImGui::Text("%.6f", *d.capture_offset_s); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coarse detected preamble position relative to this capture; not a hardware timestamp or precise packet onset.");
        ImGui::TableNextColumn();
        if (d.fec_disagreements) ImGui::Text("%d", *d.fec_disagreements); else ImGui::TextDisabled("--");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Codewords differing from their nearest valid codeword. Not corrected-bit count; zero does not guarantee integrity.");
        ImGui::TableNextColumn();
        if (d.lorawan_candidate) {
            ImGui::TextUnformatted(d.lorawan_candidate->c_str());
            if (ImGui::IsItemHovered() && d.lorawan_detail) ImGui::SetTooltip("%s", d.lorawan_detail->c_str());
        } else ImGui::TextDisabled("Unclassified");
    }
    ImGui::EndTable();
}

void draw_lora_replay_panel() {
    if (!ImGui::CollapsingHeader("Offline LoRa capture replay")) return;
    static char directory[1024] = "";
    struct Result { std::vector<LoraPacketRow> rows; std::string description; };
    static std::future<Result> pending;
    static Result result;
    static std::string error;
    if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { result = pending.get(); error.clear(); }
        catch (const std::exception& e) { error = e.what(); }
    }
    ImGui::TextWrapped("Replay is offline. Results stay separate from live packets and persistent emitter records. Enter a saved capture directory.");
    ImGui::InputText("Capture directory", directory, sizeof(directory));
    static bool laboratory_mode = false;
    ImGui::Checkbox("Legacy laboratory codecs (nonstandard)##replay", &laboratory_mode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Experimental legacy codecs only. Disable for standard over-the-air decoding.");
    }
    ImGui::BeginDisabled(pending.valid() || directory[0] == '\0');
    if (ImGui::Button("Replay capture")) {
        std::string path(directory);
        bool skip = laboratory_mode;
        result = {}; error.clear();
        pending = std::async(std::launch::async, [path, skip] {
            auto c = load_lora_capture(path);
            Result r;
            r.rows = analyze_lora_capture(c.iq, c.sample_rate_hz, c.requested_center_hz, skip);
            for (auto& row : r.rows) {
                row.time = "Replay";
                if (c.overflow) row.detail += " Capture overflow: sample continuity lost.";
            }
            r.description = path + " | " + std::to_string(c.iq.size()) + " samples | " +
                std::to_string(c.sample_rate_hz) + " samples/s | source: " + c.source +
                " | " + std::to_string(r.rows.size()) + " hypothesis results";
            r.description += "\nRequested center: " + std::to_string(c.requested_center_hz / 1e6) +
                " MHz | host capture-call Unix seconds: " + std::to_string(c.host_start_unix_s) +
                " | device: " + c.device_args + " | antenna: " + c.antenna;
            r.description += "\nRequested gain: " + (c.requested_gain_db ? std::to_string(*c.requested_gain_db) + " dB" : "AGC") +
                " | requested duration: " + std::to_string(c.requested_duration_s) + " s";
            if (c.iq.size() + 1 < c.requested_duration_s * c.sample_rate_hz)
                r.description += " | WARNING: capture shorter than requested";
            if (c.overflow) r.description += " | WARNING: overflow/discontinuous IQ";
            return r;
        });
    }
    ImGui::EndDisabled();
    if (pending.valid()) ImGui::TextUnformatted("Analyzing capture...");
    if (!error.empty()) ImGui::TextWrapped("Replay failed: %s", error.c_str());
    if (!result.description.empty()) {
        ImGui::TextWrapped("%s", result.description.c_str());
        ImGui::PushID("replay");
        draw_lora_packet_table(result.rows, 220);
        ImGui::PopID();
    }
}

} // namespace rfmon
