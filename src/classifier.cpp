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

}  // namespace

std::string classify(const std::string& band, const Segment& segment) {
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
        if (bw >= WIFI_5G_CHANNEL_BW_LO_HZ && bw <= WIFI_5G_CHANNEL_BW_HI_HZ) {
            int ch = nearest_channel(wifi_5g_channels(), segment.center_hz);
            return "WiFi-like (channel " + std::to_string(ch) + ", ~20-80MHz)";
        }
        return "Unknown 5GHz emitter";
    }

    return "Unknown emitter";
}

}  // namespace rfmon
