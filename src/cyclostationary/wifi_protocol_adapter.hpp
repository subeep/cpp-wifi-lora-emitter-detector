#pragma once
#include "wifi_protocol_observations.hpp"
#include "wifi_burst_pipeline.hpp"

namespace rfmon::cyclo {
inline bool supported_mac_shape(const std::vector<std::uint8_t>& mpdu) noexcept {
    return mpdu.size()>=14 && (mpdu[0]&3)==0 && ((mpdu[0]>>2)&3)!=3;
}
// Reuses decoder outcomes only. The OFDM support is the same padded search
// window as decode_ofdm_with_context; DSSS uses the detector's exact window.
inline WifiDecodeFact wifi_fact_from_existing_decode(const wifi::BurstWindow& b,
    const wifi::BurstWork& w,std::size_t capture_samples,double rate,double channel) noexcept {
    WifiDecodeFact f;
    if(!b.length || b.start>=capture_samples || b.length>capture_samples-b.start ||
       !std::isfinite(rate) || rate<=0 || rate>1e9 ||
       w.cls.outcome!=wifi::BurstClassification::Outcome::Accepted) return f;
    f.channel_hz=channel;
    if(w.cls.result.mod==wifi::ModClass::OFDM && w.ofdm) {
        const auto& d=*w.ofdm;
        const auto pad=std::size_t(rate*4e-6),start=b.start>pad?b.start-pad:0;
        const auto end=b.start+b.length+std::min(pad,capture_samples-(b.start+b.length));
        f.decoder_window={start,end-start};f.phy=DecodedWifiPhy::legacy_ofdm;
        f.rate_mbps=d.rate_mbps>0?unsigned(d.rate_mbps):0;
        f.verified=d.preamble_found && d.header_valid && d.fcs_valid && supported_mac_shape(d.mpdu) &&
            d.mpdu.size()==d.psdu_length && d.samples_required && d.samples_required<=f.decoder_window.samples;
    } else if(w.cls.result.mod==wifi::ModClass::DSSS && w.dsss.policy.decode && w.dsss.result) {
        const auto& d=*w.dsss.result;
        f.decoder_window={b.start,b.length};f.phy=DecodedWifiPhy::long_dsss_1mbps;f.rate_mbps=1;
        f.verified=d.preamble_found && d.plcp && d.plcp->crc_valid && d.plcp->signal==0x0a && d.rate_supported &&
            d.psdu_complete && d.fcs_valid && supported_mac_shape(d.mpdu) && d.mpdu.size()==d.mpdu_len;
    }
    return f;
}
} // namespace rfmon::cyclo
