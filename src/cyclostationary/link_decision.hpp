#pragma once
#include "waveform_review.hpp"
#include "wifi_protocol_observations.hpp"
#include <array>
#include <stdexcept>

namespace rfmon::cyclo {
// Development policy: no accepted drone catalogue exists. Protocol facts and
// whole-window DSP are separate axes, never independent probability votes.
inline constexpr const char* link_decision_version="scoped_link_decision_v1";
struct WindowProtocolEvidence {
    bool associated=false;
    std::size_t ofdm=0,dsss=0;
};
inline WindowProtocolEvidence protocol_for_window(const WifiProtocolObservations& p,
    const WifiFactContext& context,std::size_t source_samples,const TilePlan& plan,SampleTile tile) noexcept {
    WindowProtocolEvidence out;
    bool selected=false;
    for(std::size_t i=0;i<std::min(plan.count,tile_limit);++i)
        selected|=plan.tiles[i].source_offset==tile.source_offset && plan.tiles[i].samples==tile.samples;
    if(!selected || p.prefix_samples>source_samples || !valid_wifi_observations(p,context,p.prefix_samples,plan))return out;
    out.associated=true;
    for(std::size_t i=0;i<p.count;++i)if(contains_window(tile,p.facts[i].decoder_window)) {
        out.ofdm+=p.facts[i].phy==DecodedWifiPhy::legacy_ofdm;
        out.dsss+=p.facts[i].phy==DecodedWifiPhy::long_dsss_1mbps;
    }
    return out;
}
struct LinkDecision {
    const char* status="unknown";
    const char* waveform_state="unavailable";
    const char* protocol="unknown";
    const char* next_evidence="obtain_contiguous_measurement";
    std::size_t protocol_frames=0,reason_count=0;
    bool competing_patterns=false;
    std::array<const char*,12> reasons{};
};
inline LinkDecision decide_link(const WaveformReview& r,const WaveformReview& sweeps,
    const WaveformReview& background,WindowProtocolEvidence protocol={}) {
    if(r.candidates.size()>review_candidate_limit || sweeps.candidates.size()>review_candidate_limit ||
       background.candidates.size()>review_candidate_limit || protocol.ofdm>2 || protocol.dsss>2 ||
       protocol.ofdm+protocol.dsss>2 || (!protocol.associated && (protocol.ofdm || protocol.dsss)))
        throw std::invalid_argument("invalid bounded link-decision evidence");
    LinkDecision d;
    auto reason=[&](const char* text){d.reasons[d.reason_count++]=text;};
    reason("no_independently_validated_drone_link_catalogue");
    reason("emitter_separation_unresolved");
    const bool available=!r.status.empty() && r.status!="unavailable";
    d.competing_patterns=r.cp_code_ambiguous;
    bool matched=false;
    for(const auto* review:{&r,&sweeps,&background})
        for(const auto& p:review->candidates)matched|=p.pattern_consistent;
    if(!available)reason("waveform_measurement_unavailable");
    else if(!r.observed_quality_passed)reason("observed_quality_failed");
    if(r.passband_status!="fits_declared_band")reason(r.passband_status=="outside_declared_band"?
        "outside_declared_passband":"usable_passband_unknown");
    reason("receiver_profile_unqualified");
    if(d.competing_patterns)reason("competing_cp_and_barker_patterns");
    d.waveform_state=!available?"unavailable":!r.observed_quality_passed?"quality_rejected":
        r.passband_status=="outside_declared_band"?"coverage_rejected":d.competing_patterns?"competing_patterns":
        matched?"patterns_only":"no_supported_pattern";
    d.next_evidence=!available?"obtain_contiguous_measurement":!r.observed_quality_passed?"obtain_cleaner_measurement":
        r.passband_status!="fits_declared_band"?"qualify_receiver_passband":
        d.competing_patterns?"resolve_competing_patterns":"obtain_independent_link_references";
    d.protocol_frames=protocol.ofdm+protocol.dsss;
    if(d.protocol_frames) {
        d.protocol="wifi";d.status="protocol_observed_drone_link_unknown";
        reason("existing_decoder_verified_contained_wifi_frame");
        reason("wifi_does_not_determine_drone_relevance");
    } else {
        reason(protocol.associated?"no_verified_frame_in_selected_window":"associated_decoder_evidence_unavailable");
        d.status=d.competing_patterns?"ambiguous_waveform_link_unknown":!available?"unavailable":"unknown";
    }
    return d;
}
} // namespace rfmon::cyclo
