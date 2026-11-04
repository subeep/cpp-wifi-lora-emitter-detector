// Synthetic Bluetooth LE / 802.15.4 signals shared by the shortrange tests.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include "channelize.hpp"
#include "shortrange/ble.hpp"
#include "shortrange/ieee802154.hpp"

namespace shortrange_synth {
using namespace rfmon::shortrange;
using cf = std::complex<float>;

// 4 MSps baseband -> 20 MSps, shifted by offset_hz with an extra carrier offset.
inline void place(std::vector<cf>& wide, size_t at, const std::vector<cf>& bb, double offset_hz, double amplitude) {
    const auto h = rfmon::blackman_lowpass(0.1, 0.05);
    std::vector<cf> up(bb.size() * 5, {0, 0});
    for (size_t i = 0; i < bb.size(); ++i) up[i * 5] = bb[i] * 5.f;
    const ptrdiff_t half = ptrdiff_t(h.size() / 2);
    for (size_t n = 0; n < up.size() && at + n < wide.size(); ++n) {
        cf acc(0, 0);
        for (size_t j = 0; j < h.size(); ++j) {
            const ptrdiff_t k = ptrdiff_t(n) + half - ptrdiff_t(j);
            if (k >= 0 && size_t(k) < up.size()) acc += h[j] * up[size_t(k)];
        }
        const double ph = 2 * M_PI * offset_hz * double(at + n) / 20e6;
        wide[at + n] += float(amplitude) * acc * cf(float(std::cos(ph)), float(std::sin(ph)));
    }
}

inline std::vector<uint8_t> ble_adv_pdu() {
    std::vector<uint8_t> payload = {0x66, 0x55, 0x44, 0x33, 0x22, 0xC1};  // AdvA C1:22:33:44:55:66, random static
    const std::vector<uint8_t> ad = {
        0x02, 0x01, 0x06,                                                   // flags
        0x0B, 0x09, 'R', 'F', 'M', 'O', 'N', '-', 'T', 'E', 'S', 'T',     // complete name
        0x03, 0x19, 0xC1, 0x03,                                             // appearance 961 (keyboard)
        0x07, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x00, 0x01};                    // Apple Find My element
    payload.insert(payload.end(), ad.begin(), ad.end());
    std::vector<uint8_t> pdu = {0x40, uint8_t(payload.size())};  // ADV_IND, TxAdd random
    pdu.insert(pdu.end(), payload.begin(), payload.end());
    return pdu;
}

inline std::vector<uint8_t> with_fcs(std::vector<uint8_t> mac) {
    const uint16_t f = ieee802154::fcs(mac.data(), mac.size());
    mac.push_back(uint8_t(f & 0xFF));
    mac.push_back(uint8_t(f >> 8));
    return mac;
}
inline std::vector<uint8_t> zigbee_beacon() {
    // Beacon, short source 0x0000 on PAN 0x1A62; PAN coordinator, association permitted.
    std::vector<uint8_t> m = {0x00, 0x80, 0x42, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0xCF, 0x00, 0x00,
                              0x00, 0x22, 0x84, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xFF, 0xFF, 0x00};
    return with_fcs(m);
}
inline std::vector<uint8_t> zigbee_data() {
    // Data, PAN-compressed short addresses 0x796F -> 0x0000, Zigbee NWK v2 with
    // source IEEE, unsecured NWK command "link status".
    std::vector<uint8_t> m = {0x41, 0x88, 0x17, 0x62, 0x1A, 0x00, 0x00, 0x6F, 0x79,
                              0x09, 0x10, 0xFC, 0xFF, 0x6F, 0x79, 0x01, 0x22,
                              0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x00,  // NWK source IEEE 00:02:03:04:05:06:07:08... LE
                              0x08, 0x41};
    return with_fcs(m);
}

// 50 ms, 20 MSps capture centred at 2428.5 MHz (Wi-Fi ch 4 + 1.5 MHz): a BLE
// advert on ch 38 (+60 kHz CFO), a Zigbee beacon and a weaker Zigbee data
// frame on ch 16 (-70 / +40 kHz CFO), noise and a CW interferer.
constexpr double kCentre = 2428.5e6, kRate = 20e6;
inline std::vector<cf> capture(std::mt19937& rng) {
    std::normal_distribution<float> noise(0.f, 1.f);
    std::vector<cf> wide(size_t(kRate * 0.05), {0, 0});
    for (auto& v : wide) v = cf(noise(rng), noise(rng)) * 0.01f;
    place(wide, 50000, ble::modulate(ble_adv_pdu(), 38), 2426e6 + 60e3 - kCentre, 0.2);
    place(wide, 300000, ieee802154::modulate(zigbee_beacon()), 2430e6 - 70e3 - kCentre, 0.2);
    place(wide, 600000, ieee802154::modulate(zigbee_data()), 2430e6 + 40e3 - kCentre, 0.05);
    for (size_t n = 0; n < wide.size(); ++n) {
        const double ph = 2 * M_PI * (2422e6 - kCentre) * double(n) / kRate;
        wide[n] += 0.05f * cf(float(std::cos(ph)), float(std::sin(ph)));
    }
    return wide;
}

}  // namespace shortrange_synth
