#pragma once
#include "sample_tiles.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace rfmon::cyclo {
inline constexpr std::size_t wifi_fact_limit=8, wifi_fact_scan_limit=512, wifi_facts_per_tile=2;
enum class DecodedWifiPhy : unsigned { unknown=0, legacy_ofdm=1, long_dsss_1mbps=2 };
struct WifiDecodeFact {
    SampleTile decoder_window; // full window supplied to the existing decoder
    double channel_hz=0;
    unsigned rate_mbps=0;
    DecodedWifiPhy phy=DecodedWifiPhy::unknown;
    bool verified=false; // existing preamble/header/complete-PSDU/FCS checks
};
struct WifiFactContext {
    std::uint64_t radio_session=0,capture_sequence=0;
    int band_ghz=0;
    double sample_rate_hz=0,capture_center_hz=0;
};
struct WifiProtocolObservations {
    WifiFactContext context;
    bool collected=false;
    std::size_t prefix_samples=0,reported=0,examined=0,count=0;
    std::size_t no_verified_frame=0,invalid_evidence=0,outside_sampled_windows=0,budget_omitted=0;
    std::array<WifiDecodeFact,wifi_fact_limit> facts{};
};
static_assert(sizeof(WifiProtocolObservations)<=2048,"protocol snapshot must remain bounded metadata");
inline bool contains_window(SampleTile outer,SampleTile inner) noexcept {
    return inner.samples && inner.source_offset>=outer.source_offset &&
        inner.source_offset-outer.source_offset<=outer.samples &&
        inner.samples<=outer.samples-(inner.source_offset-outer.source_offset);
}
inline bool valid_wifi_fact(const WifiDecodeFact& f,const WifiFactContext& c,std::size_t prefix) noexcept {
    if(!f.verified || !contains_window({0,prefix},f.decoder_window) ||
       !std::isfinite(c.sample_rate_hz) || c.sample_rate_hz<=0 || c.sample_rate_hz>1e9 ||
       !std::isfinite(c.capture_center_hz) || c.capture_center_hz<=0 ||
       !std::isfinite(f.channel_hz) || f.channel_hz<=0 ||
       std::abs(f.channel_hz-c.capture_center_hz)>=c.sample_rate_hz/2 ||
       (c.band_ghz!=2 && c.band_ghz!=5)) return false;
    if(c.band_ghz==2 && (f.channel_hz<2.4e9 || f.channel_hz>=2.5e9)) return false;
    if(c.band_ghz==5 && (f.channel_hz<5e9 || f.channel_hz>=6e9)) return false;
    if(f.phy==DecodedWifiPhy::long_dsss_1mbps) return c.band_ghz==2 && f.rate_mbps==1;
    if(f.phy!=DecodedWifiPhy::legacy_ofdm) return false;
    for(unsigned rate:{6,9,12,18,24,36,48,54}) if(f.rate_mbps==rate) return true;
    return false;
}
// Fixed storage, bounded existing metadata only. No IQ, payload, allocation,
// decoder invocation or source identity is introduced on the capture path.
template<class Getter>
WifiProtocolObservations collect_wifi_protocol_observations(std::size_t reported,
    std::size_t prefix,const TilePlan& plan,const WifiFactContext& context,Getter get) noexcept {
    WifiProtocolObservations r;r.context=context;r.collected=true;r.prefix_samples=prefix;
    r.reported=reported;r.examined=std::min(reported,wifi_fact_scan_limit);
    std::array<std::size_t,tile_limit> per_tile{};
    for(std::size_t i=0;i<r.examined;++i) {
        const auto divisor=r.examined>1?r.examined-1:1,span=reported?reported-1:0;
        const auto f=get((span/divisor)*i+(span%divisor)*i/divisor);
        if(!f.verified) {++r.no_verified_frame;continue;}
        if(!valid_wifi_fact(f,context,prefix)) {++r.invalid_evidence;continue;}
        std::size_t tile=0;
        for(;tile<std::min(plan.count,tile_limit);++tile)
            if(contains_window(plan.tiles[tile],f.decoder_window)) break;
        if(tile==std::min(plan.count,tile_limit)) {++r.outside_sampled_windows;continue;}
        if(per_tile[tile]>=wifi_facts_per_tile || r.count==wifi_fact_limit) {++r.budget_omitted;continue;}
        ++per_tile[tile];r.facts[r.count++]=f;
    }
    return r;
}
inline bool valid_wifi_observations(const WifiProtocolObservations& r,const WifiFactContext& c,
                                    std::size_t prefix,const TilePlan& plan) noexcept {
    if(!r.collected || r.context.radio_session!=c.radio_session || r.context.capture_sequence!=c.capture_sequence ||
       r.context.band_ghz!=c.band_ghz || r.context.sample_rate_hz!=c.sample_rate_hz ||
       r.context.capture_center_hz!=c.capture_center_hz || r.prefix_samples!=prefix ||
       r.examined!=std::min(r.reported,wifi_fact_scan_limit) || r.count>wifi_fact_limit || plan.count>tile_limit ||
       r.count>r.examined || r.no_verified_frame>r.examined || r.invalid_evidence>r.examined ||
       r.outside_sampled_windows>r.examined || r.budget_omitted>r.examined ||
       r.count+r.no_verified_frame+r.invalid_evidence+r.outside_sampled_windows+r.budget_omitted!=r.examined) return false;
    std::array<std::size_t,tile_limit> per_tile{};
    for(std::size_t i=0;i<r.count;++i) {
        const auto& f=r.facts[i];if(!valid_wifi_fact(f,c,prefix)) return false;
        std::size_t tile=0;for(;tile<plan.count;++tile) if(contains_window(plan.tiles[tile],f.decoder_window)) break;
        if(tile==plan.count || ++per_tile[tile]>wifi_facts_per_tile) return false;
    }
    return true;
}
} // namespace rfmon::cyclo
