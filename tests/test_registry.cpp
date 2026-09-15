// Correctness tests for DeviceRegistry - permanence (no more cycle-
// based expiry) and RF-fingerprint-based matching for Sub-GHz/LoRa
// entries. Pure software, no hardware.
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "config.hpp"
#include "registry.hpp"

using namespace rfmon;

namespace {

int failures = 0;

void check(bool cond, const std::string& name, const std::string& detail) {
    if (cond) {
        std::printf("PASS [%s]: %s\n", name.c_str(), detail.c_str());
    } else {
        std::printf("FAIL [%s]: %s\n", name.c_str(), detail.c_str());
        ++failures;
    }
}

Detection make_detection(const std::string& band, double freq_hz, double bw_hz, double peak_db,
                          const std::string& label) {
    Segment seg{freq_hz, bw_hz, peak_db};
    return Detection{band, seg, label, std::nullopt};
}

Detection make_fingerprinted(const std::string& band, double freq_hz, double bw_hz,
                              double peak_db, const std::string& label, double irr_db,
                              double dc_dbc, double iq_eps = 0.01, double iq_phi_deg = 1.0,
                              double cfo_ppm = 2.0) {
    Segment seg{freq_hz, bw_hz, peak_db};
    FingerprintSnapshot fp;
    fp.cfo_ppm = cfo_ppm;
    fp.irr_db = irr_db;
    fp.iq_eps = iq_eps;
    fp.iq_phi_deg = iq_phi_deg;
    fp.dc_dbc = dc_dbc;
    fp.dc_ang_deg = 30.0;
    return Detection{band, seg, label, fp};
}

}  // namespace

int main() {
    // 1. Permanence: a device seen once must still be there after many
    // cycles with zero detections - the whole point of dropping the
    // old cycle-based expiry.
    {
        DeviceRegistry reg;
        reg.update_cycle({make_detection(BAND_WIFI_2G4, 2412e6, 20e6, -50.0, "WiFi-like")}, 0.0);
        for (int i = 1; i <= 20; ++i) {
            reg.update_cycle({}, double(i));  // empty cycles - no expiry call exists anymore
        }
        auto rows = reg.snapshot(20.0);
        check(rows.size() == 1, "permanence_survives_empty_cycles",
              "expected 1 row after 20 empty cycles, got " + std::to_string(rows.size()));
        if (!rows.empty()) {
            check(rows[0].age_s == 20.0, "permanence_age_from_first_seen",
                  "age_s=" + std::to_string(rows[0].age_s));
        }
    }

    // 2. Plain frequency-bucket matching (no fingerprint, e.g. WiFi):
    // repeated detections in the same bucket update one row, not many.
    {
        DeviceRegistry reg;
        for (int i = 0; i < 5; ++i) {
            reg.update_cycle({make_detection(BAND_WIFI_2G4, 2412e6, 20e6, -50.0, "WiFi-like")},
                              double(i));
        }
        auto rows = reg.snapshot(5.0);
        check(rows.size() == 1, "bucket_matching_one_row", "got " + std::to_string(rows.size()));
        if (!rows.empty()) {
            check(rows[0].hit_count == 5, "bucket_matching_hit_count",
                  "hit_count=" + std::to_string(rows[0].hit_count));
        }
    }

    // 3. Fingerprint match across a DIFFERENT frequency bucket: same
    // device's fingerprint seen again at a different frequency should
    // merge into the same entry, not create a duplicate - this is the
    // whole reason fingerprint matching exists.
    {
        DeviceRegistry reg;
        // 3 readings at 866.9 MHz to establish a fingerprint (>=
        // MIN_READINGS_TO_MATCH).
        for (int i = 0; i < 3; ++i) {
            reg.update_cycle(
                {make_fingerprinted(BAND_SUB_GHZ, 866.9e6, 125e3, 50.0, "LoRa-like", -33.5,
                                     -45.0)},
                double(i));
        }
        auto rows_before = reg.snapshot(3.0);
        check(rows_before.size() == 1, "fingerprint_match_setup",
              "expected 1 row after 3 readings, got " + std::to_string(rows_before.size()));

        // A 4th reading at a DIFFERENT frequency (e.g. the listener
        // rotated, or CFO drift moved the measured center) but a
        // closely matching fingerprint - should merge, not duplicate.
        reg.update_cycle(
            {make_fingerprinted(BAND_SUB_GHZ, 865.4025e6, 125e3, 50.0, "LoRa-like", -34.0,
                                 -44.0)},
            4.0);
        auto rows_after = reg.snapshot(4.0);
        check(rows_after.size() == 1, "fingerprint_match_merges_across_frequency",
              "expected still 1 row after a fingerprint-matched reading at a different "
              "frequency, got " +
                  std::to_string(rows_after.size()));
        if (rows_after.size() == 1) {
            check(std::abs(rows_after[0].freq_mhz - 865.4025) < 1e-6,
                  "fingerprint_match_updates_to_latest_frequency",
                  "freq_mhz=" + std::to_string(rows_after[0].freq_mhz));
            check(rows_after[0].hit_count == 4, "fingerprint_match_hit_count",
                  "hit_count=" + std::to_string(rows_after[0].hit_count));
        }
    }

    // 4. A genuinely different fingerprint at a different frequency
    // must NOT merge - two distinct devices stay distinct.
    {
        DeviceRegistry reg;
        for (int i = 0; i < 3; ++i) {
            reg.update_cycle(
                {make_fingerprinted(BAND_SUB_GHZ, 866.9e6, 125e3, 50.0, "LoRa-like", -33.5,
                                     -45.0)},
                double(i));
        }
        // Far outside the matching tolerances (IRR/DC/eps/phi all way off).
        reg.update_cycle(
            {make_fingerprinted(BAND_SUB_GHZ, 865.985e6, 125e3, 50.0, "LoRa-like", -5.0, -10.0,
                                 0.15, 20.0)},
            4.0);
        auto rows = reg.snapshot(4.0);
        check(rows.size() == 2, "mismatched_fingerprint_stays_separate",
              "expected 2 distinct devices, got " + std::to_string(rows.size()));
    }

    // 5. Too few readings to trust a match yet: a device with only 1-2
    // readings shouldn't be used as a merge target - falls back to
    // bucket-key (creating a second entry at the new frequency) rather
    // than merging prematurely off a single noisy reading.
    {
        DeviceRegistry reg;
        reg.update_cycle(
            {make_fingerprinted(BAND_SUB_GHZ, 866.9e6, 125e3, 50.0, "LoRa-like", -33.5, -45.0)},
            0.0);
        reg.update_cycle(
            {make_fingerprinted(BAND_SUB_GHZ, 865.4025e6, 125e3, 50.0, "LoRa-like", -34.0,
                                 -44.0)},
            1.0);
        auto rows = reg.snapshot(1.0);
        check(rows.size() == 2, "insufficient_history_falls_back_to_bucket",
              "expected 2 rows (not enough history to trust a fingerprint match yet), got " +
                  std::to_string(rows.size()));
    }

    // 6. A correlator-confirmed Wi-Fi modulation and a bare energy
    // segment on the SAME channel land in the same frequency bucket.
    // The modulation label must survive even though its peak_db comes
    // from a time-domain mean (~-40dB) while the energy segment's comes
    // from an un-normalized |FFT|^2 (~+14dB), so the energy one wins
    // any direct numeric comparison between them. Before
    // Detection::modulation_confirmed existed the label was discarded
    // here every cycle and never reached the GUI at all.
    {
        DeviceRegistry reg;
        Detection energy;
        energy.band = BAND_WIFI_2G4;
        energy.segment = Segment{2437e6, 3e6, 14.0};  // strong, but unlabelled
        energy.protocol_guess = "Unknown 2.4GHz emitter";

        Detection modulated;
        modulated.band = BAND_WIFI_2G4;
        modulated.segment = Segment{2437e6, 20e6, -40.0};  // far "weaker" by peak_db
        modulated.protocol_guess = "WiFi-like (802.11 OFDM, channel 6, ~20MHz)";
        modulated.modulation_confirmed = true;

        reg.update_cycle({energy, modulated}, 0.0);
        auto rows = reg.snapshot(0.0);
        check(rows.size() == 1, "same_channel_collapses_to_one_row",
              "expected 1 row for one channel, got " + std::to_string(rows.size()));
        if (rows.size() == 1) {
            check(rows[0].protocol_guess.find("OFDM") != std::string::npos,
                  "modulation_label_beats_stronger_energy_segment",
                  "expected the correlator-confirmed label to survive, got '" +
                      rows[0].protocol_guess + "'");
        }
    }

    // 6b. The converse must still hold: with no modulation evidence on
    // either side, the stronger energy segment wins exactly as before.
    {
        DeviceRegistry reg;
        Detection weak;
        weak.band = BAND_WIFI_2G4;
        weak.segment = Segment{2437e6, 3e6, -10.0};
        weak.protocol_guess = "weak";

        Detection strong;
        strong.band = BAND_WIFI_2G4;
        strong.segment = Segment{2437e6, 5e6, 20.0};
        strong.protocol_guess = "strong";

        reg.update_cycle({weak, strong}, 0.0);
        auto rows = reg.snapshot(0.0);
        check(rows.size() == 1 && !rows.empty() && rows[0].protocol_guess == "strong",
              "strongest_energy_still_wins_without_modulation_evidence",
              "expected 'strong', got '" + (rows.empty() ? std::string("(none)")
                                                          : rows[0].protocol_guess) + "'");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll registry checks passed.\n");
    return 0;
}
