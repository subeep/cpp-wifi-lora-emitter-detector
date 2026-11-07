#include "lora_observation.hpp"
#include "lora_decimator.hpp"
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
void trim_lora_packet_log(std::vector<LoraPacketRow>& log, size_t decoded_max, size_t other_max) {
    size_t decoded = 0, other = 0;
    for (const auto& r : log) (r.crc == LoraPacketRow::Crc::Valid ? decoded : other) += 1;
    if (decoded <= decoded_max && other <= other_max) return;
    size_t drop_decoded = decoded > decoded_max ? decoded - decoded_max : 0;
    size_t drop_other = other > other_max ? other - other_max : 0;
    std::vector<LoraPacketRow> kept;
    kept.reserve(log.size() - drop_decoded - drop_other);
    for (auto& r : log) {  // oldest first
        size_t& drop = r.crc == LoraPacketRow::Crc::Valid ? drop_decoded : drop_other;
        if (drop) { --drop; continue; }
        kept.push_back(std::move(r));
    }
    log.swap(kept);
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
                if(!candidate->device_id.empty()){row.lorawan_device_kind=candidate->device_kind;row.lorawan_device_id=candidate->device_id;row.lorawan_uplink=candidate->uplink;}
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
        // Same anti-alias decimation and sub-chip retry as the live scanner.
        const auto iq = lora_decimator::decimate(raw, int(factor));
        for (int sf : LORA_LISTEN_SF_LIST) {
            for (auto& row : analyze_lora_packets_phased(raw, iq, int(factor), sf, bw, laboratory_mode)) {
                row.freq_mhz = frequency / 1e6; row.bandwidth_khz = bw / 1e3;
                rows.push_back(std::move(row));
            }
        }
    }
    if(!laboratory_mode)annotate_lora_receptions(rows,rate,"capture-local");
    return rows;
}
std::vector<LoraPacketRow> analyze_lora_packets_phased(const std::vector<std::complex<float>>& raw,
                                                       const std::vector<std::complex<float>>& decimated, int factor,
                                                       int sf, double bandwidth, bool laboratory_mode) {
    auto rows = analyze_lora_packets(decimated, sf, bandwidth, laboratory_mode);
    if (laboratory_mode || factor <= 1 || sf < 7 || sf > 12) return rows;
    const size_t n = size_t(1) << sf, f = size_t(factor);
    for (auto& row : rows) {
        if (!row.header_valid || row.crc != LoraPacketRow::Crc::Failed || row.decoder != "LoRa explicit PHY" ||
            !row.decoder_sample_start || !row.decoder_data_start)
            continue;
        // Span: from two symbols before the preamble detection to the end of
        // the declared payload (LDRO width bound), plus margin.
        const size_t start = *row.decoder_sample_start, data = *row.decoder_data_start;
        const int len = row.payload_len.value_or(255), cr = row.cr.value_or(4), width = std::max(1, sf - 2);
        const size_t symbols = 8 + size_t((2 * (len + 2) + width - 1) / width) * size_t(cr + 4) + 4;
        const size_t lo = start > 2 * n ? start - 2 * n : 0;
        const size_t hi = std::min(decimated.size(), data + symbols * n + 2 * n);
        if (hi <= lo) continue;
        for (size_t phase = 1; phase < f; ++phase) {
            const size_t r0 = lo * f + phase, r1 = std::min(raw.size(), hi * f + phase);
            if (r1 <= r0 + f * n) break;
            const std::vector<std::complex<float>> segment(raw.begin() + long(r0), raw.begin() + long(r1));
            // Output k of this segment sits phase/f of a chip after phase-0
            // output lo + k; report it on the nearest phase-0 sample.
            const size_t shift = lo + (2 * phase >= f ? 1 : 0);
            bool replaced = false;
            for (auto& c : analyze_lora_packets(lora_decimator::decimate(segment, factor), sf, bandwidth, false)) {
                if (c.crc != LoraPacketRow::Crc::Valid || c.inverted_iq != row.inverted_iq || !c.decoder_data_start ||
                    !c.decoder_sample_start)
                    continue;
                const size_t c_data = *c.decoder_data_start + shift;
                if ((c_data > data ? c_data - data : data - c_data) > 2) continue;
                const long window_offset = long(*c.decoder_sample_start);
                c.decoder_sample_start = *c.decoder_sample_start + shift;
                c.decoder_data_start = c_data;
                if (c.decoder_sample_end) c.decoder_sample_end = *c.decoder_sample_end + shift;
                c.capture_offset_s = double(*c.decoder_sample_start) / bandwidth;
                const std::string from = "Sample offset " + std::to_string(window_offset) + ";";
                if (auto at = c.detail.find(from); at != std::string::npos)
                    c.detail.replace(at, from.size(), "Sample offset " + std::to_string(*c.decoder_sample_start) + ";");
                char note[160];
                std::snprintf(note, sizeof note,
                              "Recovered at sub-chip sampling phase %zu/%zu after the phase-0 decode failed CRC. ", phase, f);
                c.detail = note + c.detail;
                row = std::move(c);
                replaced = true;
                break;
            }
            if (replaced) break;
        }
    }
    return rows;
}
size_t tag_lora_iq_images(const std::vector<LoraRowGroup>& groups, double center_hz, double freq_tolerance_hz) {
    struct Ref { const LoraRowGroup* g; LoraPacketRow* r; double apparent_hz; };
    std::vector<Ref> rows;
    for (const auto& g : groups) {
        if (!g.rows) continue;
        for (auto& r : *g.rows)
            if (r.header_valid && r.inverted_iq && r.cfo_hz && r.capture_offset_s)
                rows.push_back({&g, &r, g.channel_hz + *r.cfo_hz});
    }
    size_t tagged = 0;
    for (auto& o : rows) {
        if (o.r->iq_image_of_mhz) continue;
        const bool o_valid = o.r->crc == LoraPacketRow::Crc::Valid;
        const double target = 2 * center_hz - o.apparent_hz;
        const double symbol_s = double(1 << o.r->sf) / o.g->bw_hz;
        for (const auto& p : rows) {
            if (p.r == o.r || p.r->sf != o.r->sf || p.g->bw_hz != o.g->bw_hz) continue;
            if (*p.r->inverted_iq == *o.r->inverted_iq) continue;
            if (std::abs(p.apparent_hz - target) > freq_tolerance_hz) continue;
            if (std::abs(*p.r->capture_offset_s - *o.r->capture_offset_s) > 4 * symbol_s) continue;
            const bool p_valid = p.r->crc == LoraPacketRow::Crc::Valid;
            const bool p_stronger = p.r->preamble_peak_ratio.value_or(0) > o.r->preamble_peak_ratio.value_or(0);
            if (o_valid ? !(p_valid && p_stronger && p.r->payload_hex == o.r->payload_hex) : !(p_valid || p_stronger))
                continue;
            o.r->iq_image_of_mhz = p.g->channel_hz / 1e6;
            char note[256];
            std::snprintf(note, sizeof note,
                          "Likely receiver IQ-imbalance image of the packet decoded on %.4f MHz at the same time "
                          "(mirrored about the %.4f MHz capture centre, opposite IQ polarity); not a separate "
                          "transmission. ",
                          p.g->channel_hz / 1e6, center_hz / 1e6);
            o.r->detail = note + o.r->detail;
            ++tagged;
            break;
        }
    }
    return tagged;
}
} // namespace rfmon
