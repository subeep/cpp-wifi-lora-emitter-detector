// Correctness tests for src/wifi_master.cpp - the persistent, cross-run
// Wi-Fi master emitter list. Pure software: each test gets its own
// fresh temp directory, no hardware/SDR involved. Mirrors
// test_lora_master.cpp's structure, with the added MAC-vs-fingerprint-
// cluster keying this file's own header describes.
#include <cstdio>
#include <filesystem>
#include <string>

#include "wifi_fingerprint.hpp"
#include "wifi_master.hpp"

using namespace rfmon;
using namespace rfmon::wifi_master;

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

wifi_fingerprint::WifiFingerprint make_fp(double irr_db, double dc_dbc, double iq_eps,
                                           double iq_phi_deg, double cfo_ppm = 5.0) {
    wifi_fingerprint::WifiFingerprint fp;
    fp.gated_out = false;
    fp.cfo_ppm = cfo_ppm;
    fp.irr_db = irr_db;
    fp.dc_dbc = dc_dbc;
    fp.iq_eps = iq_eps;
    fp.iq_phi_deg = iq_phi_deg;
    fp.dc_ang_deg = 30.0;
    fp.snr_db = 35.0;
    fp.evm_pct = 4.0;
    fp.sync_corr = 0.97;
    fp.n_samp = 128;
    return fp;
}

std::string fresh_dir(const std::string& name) {
    std::string path = "test_wifi_master_tmp/" + name;
    std::filesystem::remove_all(path);
    return path;
}

}  // namespace

int main() {
    // 1. First reading with no MAC creates one fingerprint-cluster
    // device, WIFI-FP-0001.
    {
        std::string dir = fresh_dir("basic_fp");
        WifiMasterList list(dir);
        list.record_reading(std::nullopt, "OFDM", 2437e6, 20e6, make_fp(-34.0, -40.0, 0.02, 2.0),
                             1000);
        auto rows = list.snapshot();
        check(rows.size() == 1, "first_fp_reading_creates_one_device",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) {
            check(rows[0].device_key == "WIFI-FP-0001", "fp_device_key_format",
                  "got '" + rows[0].device_key + "'");
            check(!rows[0].key_is_mac, "fp_device_not_mac", "");
            check(rows[0].reading_count == 1, "fp_reading_count_one", "");
        }
    }

    // 2. A reading WITH a MAC creates a MAC-keyed device instead.
    {
        std::string dir = fresh_dir("basic_mac");
        WifiMasterList list(dir);
        list.record_reading("aa:bb:cc:dd:ee:ff", "DSSS", 2437e6, 22e6,
                             make_fp(-30.0, -35.0, 0.01, 1.0), 2000);
        auto rows = list.snapshot();
        check(rows.size() == 1, "first_mac_reading_creates_one_device",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) {
            check(rows[0].device_key == "aa:bb:cc:dd:ee:ff", "mac_device_key",
                  "got '" + rows[0].device_key + "'");
            check(rows[0].key_is_mac, "mac_device_is_mac", "");
        }
    }

    // 3. A second reading with the SAME MAC matches exactly, even with
    // wildly different fingerprint parameters - MAC is ground truth,
    // no fuzzy tolerance involved.
    {
        std::string dir = fresh_dir("mac_exact_overrides_fingerprint");
        WifiMasterList list(dir);
        list.record_reading("11:22:33:44:55:66", "OFDM", 2412e6, 20e6,
                             make_fp(-34.0, -40.0, 0.02, 2.0), 1000);
        list.record_reading("11:22:33:44:55:66", "OFDM", 2412e6, 20e6,
                             make_fp(10.0, 5.0, 0.09, -15.0), 1500);
        auto rows = list.snapshot();
        check(rows.size() == 1, "same_mac_matches_despite_fingerprint_mismatch",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) check(rows[0].reading_count == 2, "mac_reading_count_two", "");
    }

    // 4. A DIFFERENT MAC never merges with an existing one, even with
    // an identical fingerprint - decoded identity wins over similarity.
    {
        std::string dir = fresh_dir("different_mac_never_merges");
        WifiMasterList list(dir);
        list.record_reading("aa:aa:aa:aa:aa:aa", "DSSS", 2437e6, 22e6,
                             make_fp(-34.0, -40.0, 0.02, 2.0), 1000);
        list.record_reading("bb:bb:bb:bb:bb:bb", "DSSS", 2437e6, 22e6,
                             make_fp(-34.0, -40.0, 0.02, 2.0), 1500);
        auto rows = list.snapshot();
        check(rows.size() == 2, "different_mac_creates_second_device",
              "got " + std::to_string(rows.size()) + " devices");
    }

    // 5. Fingerprint-cluster matching (no MAC): a similar reading
    // matches the same cluster, a dissimilar one creates a new one -
    // same behavior as LoRa's matcher.
    {
        std::string dir = fresh_dir("fp_cluster_matching");
        WifiMasterList list(dir);
        list.record_reading(std::nullopt, "OFDM", 2437e6, 20e6, make_fp(-34.0, -40.0, 0.02, 2.0),
                             1000);
        list.record_reading(std::nullopt, "OFDM", 2437e6, 20e6, make_fp(-35.5, -41.2, 0.023, 2.8),
                             1500);
        list.record_reading(std::nullopt, "OFDM", 2437e6, 20e6, make_fp(15.0, 5.0, 0.09, -15.0),
                             2000);
        auto rows = list.snapshot();
        check(rows.size() == 2, "fp_cluster_matches_similar_splits_dissimilar",
              "got " + std::to_string(rows.size()) + " devices");
    }

    // 6. cfo_ppm swings must not prevent a fingerprint-cluster match -
    // same exclusion as LoRa's matcher (see this file's own header for
    // why this specific choice needs its own WiFi validation).
    {
        std::string dir = fresh_dir("cfo_excluded");
        WifiMasterList list(dir);
        list.record_reading(std::nullopt, "OFDM", 2437e6, 20e6,
                             make_fp(-34.0, -40.0, 0.02, 2.0, /*cfo_ppm=*/5.0), 1000);
        list.record_reading(std::nullopt, "OFDM", 2437e6, 20e6,
                             make_fp(-34.0, -40.0, 0.02, 2.0, /*cfo_ppm=*/-90.0), 1500);
        auto rows = list.snapshot();
        check(rows.size() == 1, "cfo_swing_does_not_split_fp_cluster",
              "got " + std::to_string(rows.size()) + " devices");
    }

    // 7. Reading-history cap, same shape as LoRa's.
    {
        std::string dir = fresh_dir("cap");
        WifiMasterList list(dir);
        int64_t ts = 0;
        for (int i = 0; i < MAX_READINGS_PER_DEVICE + 250; ++i) {
            list.record_reading("cc:cc:cc:cc:cc:cc", "OFDM", 2437e6, 20e6,
                                 make_fp(-34.0, -40.0, 0.02, 2.0), ts);
            ts += 1;
        }
        auto rows = list.snapshot();
        check(rows.size() == 1, "cap_test_still_one_device",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) {
            check(rows[0].reading_count >= MAX_READINGS_PER_DEVICE &&
                      rows[0].reading_count <= COMPACT_TRIGGER_READINGS,
                  "reading_count_bounded",
                  "got " + std::to_string(rows[0].reading_count));
            check(rows[0].first_seen_ts == 0, "first_seen_survives_pruning",
                  "got " + std::to_string(rows[0].first_seen_ts));
        }
    }

    // 8. Persistence across a simulated restart - both a MAC-keyed and
    // a fingerprint-cluster device must reload correctly, including
    // next_fp_id_ being re-seeded so a NEW fp-cluster device after
    // reload doesn't collide with the reloaded one's id.
    {
        std::string dir = fresh_dir("persistence");
        {
            WifiMasterList list(dir);
            list.record_reading("dd:dd:dd:dd:dd:dd", "DSSS", 2437e6, 22e6,
                                 make_fp(-30.0, -35.0, 0.01, 1.0), 1000);
            list.record_reading(std::nullopt, "OFDM", 2412e6, 20e6, make_fp(-34.0, -40.0, 0.02, 2.0),
                                 2000);
        }
        WifiMasterList reloaded(dir);
        auto rows = reloaded.snapshot();
        check(rows.size() == 2, "reload_recovers_both_devices",
              "got " + std::to_string(rows.size()) + " devices");

        // A brand new fp-cluster reading after reload must not collide
        // with WIFI-FP-0001's id.
        reloaded.record_reading(std::nullopt, "OFDM", 2462e6, 20e6, make_fp(20.0, 15.0, 0.15, -25.0),
                                 3000);
        auto rows2 = reloaded.snapshot();
        check(rows2.size() == 3, "next_fp_id_reseeded_no_collision",
              "got " + std::to_string(rows2.size()) + " devices");
        bool saw_0001 = false, saw_0002 = false;
        for (const auto& r : rows2) {
            if (r.device_key == "WIFI-FP-0001") saw_0001 = true;
            if (r.device_key == "WIFI-FP-0002") saw_0002 = true;
        }
        check(saw_0001 && saw_0002, "fp_cluster_ids_distinct_after_reload",
              "0001=" + std::to_string(saw_0001) + " 0002=" + std::to_string(saw_0002));

        // The MAC device must still match against its reloaded history.
        reloaded.record_reading("dd:dd:dd:dd:dd:dd", "DSSS", 2437e6, 22e6,
                                 make_fp(-31.0, -36.0, 0.011, 1.1), 4000);
        auto rows3 = reloaded.snapshot();
        for (const auto& r : rows3) {
            if (r.device_key == "dd:dd:dd:dd:dd:dd") {
                check(r.reading_count == 2, "mac_match_against_reloaded_history",
                      "got " + std::to_string(r.reading_count));
            }
        }
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll Wi-Fi master list checks passed.\n");
    return 0;
}
