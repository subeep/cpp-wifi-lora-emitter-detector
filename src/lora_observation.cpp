#include "lora_observation.hpp"
#include "lora_phy.hpp"
#include "lora_receiver.hpp"
#include "lorawan_inspect.hpp"
#include "config.hpp"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <limits>

namespace rfmon {
namespace {
struct ReceptionCoordinates {
    bool valid = false;
    size_t start = 0, end = 0, uncertainty = 0, symbol = 0;
    std::optional<size_t> data;
};
ReceptionCoordinates reception_coordinates(const LoraPacketRow& r, double rate) {
    ReceptionCoordinates c;
    if(!r.header_valid || r.decoder!="LoRa explicit PHY" || r.sf<7 || r.sf>12 ||
       !r.decoder_sample_start || !r.decoder_sample_end || *r.decoder_sample_start>=*r.decoder_sample_end ||
       !std::isfinite(rate) || rate<=0 || !std::isfinite(r.freq_mhz) ||
       (r.bandwidth_khz!=125 && r.bandwidth_khz!=250 && r.bandwidth_khz!=500))return c;
    const double ratio=rate/(r.bandwidth_khz*1000);
    if(!std::isfinite(ratio)||ratio<1||ratio>1024||std::abs(ratio-std::round(ratio))>1e-6)return c;
    const size_t factor=size_t(std::llround(ratio)), max=std::numeric_limits<size_t>::max()/factor;
    if(*r.decoder_sample_end>max || r.start_uncertainty_samples>max)return c;
    if(r.decoder_data_start) {
        if(*r.decoder_data_start<*r.decoder_sample_start || *r.decoder_data_start>=*r.decoder_sample_end)return c;
        c.data=*r.decoder_data_start*factor;
    }
    c.start=*r.decoder_sample_start*factor;c.end=*r.decoder_sample_end*factor;
    c.uncertainty=r.start_uncertainty_samples*factor;c.symbol=(size_t(1)<<r.sf)*factor;
    c.valid=true;return c;
}
size_t sample_distance(size_t a,size_t b) {return a>b?a-b:b-a;}
bool same_reception(const LoraPacketRow& a,const ReceptionCoordinates& ca,
                    const LoraPacketRow& b,const ReceptionCoordinates& cb) {
    if(!ca.valid || !cb.valid || a.sf!=b.sf || a.bandwidth_khz!=b.bandwidth_khz ||
       a.freq_mhz!=b.freq_mhz || a.cr!=b.cr || a.payload_len!=b.payload_len ||
       a.payload_complete!=b.payload_complete || a.crc!=b.crc || a.payload_hex!=b.payload_hex)return false;
    // Opposite-polarity evidence is compatible only when both independently
    // recovered the same complete CRC-valid bytes. Never resolve ambiguity by
    // picking one unverified polarity interpretation.
    if(a.inverted_iq!=b.inverted_iq && !(a.payload_complete && a.crc==LoraPacketRow::Crc::Valid))return false;
    if(ca.data && cb.data) {
        // An aligned SFD/data anchor is stronger than overlap/coarse starts.
        // Keep the tolerance below a symbol and compare to the FIRST member,
        // not an expanding/transitive union that could swallow later packets.
        const size_t tolerance=std::min({ca.symbol/4,cb.symbol/4,ca.uncertainty,cb.uncertainty});
        return sample_distance(*ca.data,*cb.data)<=tolerance &&
               sample_distance(ca.start,cb.start)<=std::min(ca.symbol,cb.symbol) &&
               sample_distance(ca.end,cb.end)<=std::min(ca.symbol,cb.symbol);
    }
    // Old rows do not have an aligned anchor. Preserve the old exact-span
    // behavior rather than guessing equivalence from payload bytes or overlap.
    return !ca.data && !cb.data && ca.start==cb.start && ca.end==cb.end;
}
}
LoraReceptionGrouping group_lora_receptions(const std::vector<LoraPacketRow>& rows,double rate) {
    LoraReceptionGrouping result;
    std::vector<ReceptionCoordinates> coordinates;coordinates.reserve(rows.size());
    for(const auto& r:rows)coordinates.push_back(reception_coordinates(r,rate));
    size_t remaining_comparisons=1u<<20; // conservative fall-back keeps rows separate
    for(size_t i=0;i<rows.size();++i) {
        const auto& c=coordinates[i];
        if(!c.valid)++result.unlocated;
        bool grouped=false;
        if(c.valid && remaining_comparisons)for(auto& g:result.groups) {
            if(!remaining_comparisons){result.comparison_limited=true;break;}
            --remaining_comparisons;
            if(!g.located)continue;
            const size_t anchor=g.hypotheses.front();
            if(!same_reception(rows[anchor],coordinates[anchor],rows[i],c))continue;
            g.hypotheses.push_back(i);
            // Stable representative selection: no change to any member's bytes,
            // integrity, provenance or quality measurements.
            if(rows[i].fec_disagreements && (!rows[g.representative].fec_disagreements ||
               *rows[i].fec_disagreements<*rows[g.representative].fec_disagreements))g.representative=i;
            grouped=true;break;
        }
        if(!grouped)result.groups.push_back({{i},i,c.valid});
        if(c.valid && !remaining_comparisons)result.comparison_limited=true;
    }
    return result;
}
void annotate_lora_receptions(std::vector<LoraPacketRow>& rows,double rate,const std::string& scope) {
    const auto grouped=group_lora_receptions(rows,rate);
    for(auto& row:rows){row.reception_id.clear();row.reception_hypotheses=0;row.reception_grouping_limited=grouped.comparison_limited;}
    size_t ordinal=0;
    for(const auto& g:grouped.groups) {
        if(!g.located)continue;
        const std::string id=scope+"/reception-"+std::to_string(++ordinal);
        for(size_t i:g.hypotheses){rows[i].reception_id=id;rows[i].reception_hypotheses=g.hypotheses.size();}
    }
}
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
static std::optional<LoraPacketRow> analyze_lora_laboratory(const std::vector<std::complex<float>>& iq, int sf,
                                                       bool skip_sync_check) {
    auto standard = lora::std_phy::demodulate(iq, sf, skip_sync_check);
    if (standard && standard->header_valid) {
        LoraPacketRow row;
        row.decoder = "Legacy SX reference (laboratory)";
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
        row.decoder = "Legacy internal codec (nonstandard)";
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
std::vector<LoraPacketRow> analyze_lora_packets(const std::vector<std::complex<float>>& iq,
                                                int sf, double bandwidth, bool laboratory_mode) {
    if (laboratory_mode) {
        auto row = analyze_lora_laboratory(iq, sf, true);
        return row ? std::vector<LoraPacketRow>{*row} : std::vector<LoraPacketRow>{};
    }
    std::vector<LoraPacketRow> rows;
    lora::receiver::Diagnostics diagnostics;
    const auto packets=lora::receiver::demodulate(iq, sf, bandwidth, {}, &diagnostics);
    for (const auto& p : packets) {
        LoraPacketRow row;
        row.decoder = "LoRa explicit PHY";
        row.inverted_iq = p.inverted_iq;
        row.preamble_peak_ratio = p.preamble_peak_ratio;
        row.sfd_peak_ratio = p.sfd_peak_ratio;
        row.cfo_hz = p.cfo_bins * bandwidth / (1 << sf);
        row.drift_hz_per_symbol = p.drift_bins_per_symbol * bandwidth / (1 << sf);
        row.capture_offset_s = p.start_sample / bandwidth;
        if (p.start_sample>=0 && p.end_sample>p.start_sample) {
            row.decoder_sample_start=size_t(p.start_sample);row.decoder_sample_end=size_t(p.end_sample);
            if(p.data_start_sample>=p.start_sample && p.data_start_sample<p.end_sample)row.decoder_data_start=size_t(p.data_start_sample);
            row.start_uncertainty_samples=size_t(1)<<sf;
        }
        row.analysis_limited=diagnostics.packet_limit_reached; // per polarity, not combined count
        row.fec_disagreements = p.fec_disagreements;
        row.sf = sf; row.cr = p.cr; row.payload_len = p.declared_payload_len;
        row.cfo_bins = int(std::lround(p.cfo_bins)); row.ldro = p.ldro;
        row.ldro_ambiguous = p.ldro_ambiguous; row.sync_word = p.sync_word;
        set_lora_integrity(row, p.payload_complete, p.crc_on, p.crc_valid);
        row.detail += " Sample offset " + std::to_string(p.start_sample) +
            "; preamble drift " + std::to_string(p.drift_bins_per_symbol) + " bins/symbol.";
        if (p.sync_word) {
            char text[32]; std::snprintf(text, sizeof(text), " Observed sync 0x%02X.", *p.sync_word);
            row.detail += text;
        } else row.detail += " Sync bins " + std::to_string(p.sync_bins[0]) + ", " +
                              std::to_string(p.sync_bins[1]) + " (not quantized).";
        if (p.ldro_ambiguous) row.detail += " LDRO hypothesis unresolved by CRC.";
        if(!p.payload_complete || !p.crc_on || !p.crc_valid)
            row.detail += " Unverified candidate: search resumed after its header instead of trusting the payload span.";
        if (p.payload_complete) payload_text(row, p.payload);
        if (p.payload_complete && (!p.crc_on || p.crc_valid)) {
            if (auto candidate=inspect_lorawan(p.payload)) {
                row.lorawan_candidate=candidate->type;
                row.lorawan_detail=candidate->detail;
                if (!p.crc_on) *row.lorawan_detail += " Physical payload CRC absent; bytes also unverified.";
            }
        }
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        auto burst = lora::detect_burst(iq, sf);
        if (burst || diagnostics.preamble_candidates) {
            LoraPacketRow row; row.sf = sf; if (burst) row.cfo_bins = burst->cfo_bins;
            row.status = "Detected only";
            row.detail = "Preamble evidence; no valid supported explicit header. May be truncated, implicit, unsupported or interference.";
            row.detail += " Search attempts across IQ polarities: preamble=" + std::to_string(diagnostics.preamble_candidates) +
                ", SFD=" + std::to_string(diagnostics.sfd_candidates) + ", aligned=" +
                std::to_string(diagnostics.aligned_candidates) + ", header rejected=" +
                std::to_string(diagnostics.headers_rejected) + ". These are not packet counts.";
            rows.push_back(std::move(row));
        }
    }
    return rows;
}
std::optional<LoraPacketRow> analyze_lora_hypothesis(const std::vector<std::complex<float>>& iq,
                                                       int sf, bool laboratory_mode) {
    auto rows = analyze_lora_packets(iq, sf, 125000, laboratory_mode);
    if (rows.empty()) return std::nullopt;
    return std::move(rows.front());
}
std::vector<LoraPacketRow> analyze_lora_capture(const std::vector<std::complex<float>>& raw, double rate,
                                                 double frequency, bool laboratory_mode) {
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
            for (auto& row : analyze_lora_packets(iq, sf, bw, laboratory_mode)) {
                row.freq_mhz = frequency / 1e6; row.bandwidth_khz = bw / 1e3;
                rows.push_back(std::move(row));
            }
        }
    }
    if(!laboratory_mode)annotate_lora_receptions(rows,rate,"capture-local");
    return rows;
}
} // namespace rfmon
