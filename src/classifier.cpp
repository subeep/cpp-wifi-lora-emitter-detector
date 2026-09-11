#include "classifier.hpp"

#include <algorithm>
#include <cmath>

#include "config.hpp"

namespace rfmon {

namespace {

template <typename MapT>
int nearest_channel(const MapT& channels, double freq_hz) {
    int best_ch = channels.begin()->first;
    double best_dist = std::abs(channels.begin()->second - freq_hz);
    for (const auto& [ch, f] : channels) {
        double dist = std::abs(f - freq_hz);
        if (dist < best_dist) {
            best_dist = dist;
            best_ch = ch;
        }
    }
    return best_ch;
}

std::string mod_tag(wifi::ModClass mod) {
    if (mod == wifi::ModClass::DSSS) return "802.11b DSSS, ";
    if (mod == wifi::ModClass::OFDM) return "802.11 OFDM, ";
    return "";
}

}  // namespace

std::string classify(const std::string& band, const Segment& segment, wifi::ModClass mod) {
    double bw = segment.bandwidth_hz;

    if (band == BAND_SUB_GHZ) {
        for (const auto& [nominal_hz, tolerance_hz] : lora_bandwidths_hz()) {
            if (std::abs(bw - nominal_hz) <= tolerance_hz) {
                return "LoRa-like (BW~" + std::to_string(int(nominal_hz / 1e3)) + "kHz)";
            }
        }
        return "Unknown sub-GHz emitter";
    }

    if (band == BAND_WIFI_2G4) {
        // A correlator confirmation (mod != Unknown) is its own
        // detection evidence - always "WiFi-like" regardless of the
        // measured bandwidth bucket (see wifi_phy.hpp's file header).
        // Bandwidth is shown for real here (not the old fixed "~20MHz"
        // text), since a correlator-confirmed hit's own measured width
        // is genuinely informative now, not just a bucket membership.
        if (mod != wifi::ModClass::Unknown) {
            int ch = nearest_channel(wifi_2g4_channels(), segment.center_hz);
            return "WiFi-like (" + mod_tag(mod) + "channel " + std::to_string(ch) + ", ~" +
                   std::to_string(int(bw / 1e6)) + "MHz)";
        }
        if (bw >= WIFI_2G4_CHANNEL_BW_LO_HZ && bw <= WIFI_2G4_CHANNEL_BW_HI_HZ) {
            int ch = nearest_channel(wifi_2g4_channels(), segment.center_hz);
            return "WiFi-like (channel " + std::to_string(ch) + ", ~20MHz)";
        }
        if (bw >= NARROWBAND_2G4_LO_HZ && bw <= NARROWBAND_2G4_HI_HZ) {
            return "Unknown 2.4GHz narrowband (possible BLE/Zigbee)";
        }
        return "Unknown 2.4GHz emitter";
    }

    if (band == BAND_WIFI_5G) {
        if (mod != wifi::ModClass::Unknown) {
            int ch = nearest_channel(wifi_5g_channels(), segment.center_hz);
            return "WiFi-like (" + mod_tag(mod) + "channel " + std::to_string(ch) + ", ~" +
                   std::to_string(int(bw / 1e6)) + "MHz)";
        }
        if (bw >= WIFI_5G_CHANNEL_BW_LO_HZ && bw <= WIFI_5G_CHANNEL_BW_HI_HZ) {
            int ch = nearest_channel(wifi_5g_channels(), segment.center_hz);
            return "WiFi-like (channel " + std::to_string(ch) + ", ~20-80MHz)";
        }
        return "Unknown 5GHz emitter";
    }

    return "Unknown emitter";
}

}  // namespace rfmon
