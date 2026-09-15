// Correctness tests for src/lora_master.cpp - the persistent, cross-run
// LoRa master emitter list. Pure software: each test gets its own
// fresh temp directory (removed first in case a previous run left one
// behind), no hardware/SDR involved.
#include <cstdio>
#include <filesystem>
#include <string>

#include "fingerprint.hpp"
#include "lora_master.hpp"

using namespace rfmon;
using namespace rfmon::lora_master;

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

fingerprint::LoraFingerprint make_fp(double irr_db, double dc_dbc, double iq_eps,
                                      double iq_phi_deg, double cfo_ppm = 5.0) {
    fingerprint::LoraFingerprint fp;
    fp.gated_out = false;
    fp.cfo_ppm = cfo_ppm;
    fp.irr_db = irr_db;
    fp.dc_dbc = dc_dbc;
    fp.iq_eps = iq_eps;
    fp.iq_phi_deg = iq_phi_deg;
    fp.dc_ang_deg = 30.0;
    fp.snr_db = 40.0;
    fp.evm_pct = 5.0;
    fp.sync_corr = 0.98;
    fp.n_samp = 1000;
    return fp;
}

std::string fresh_dir(const std::string& name) {
    std::string path = "test_lora_master_tmp/" + name;
    std::filesystem::remove_all(path);
    return path;
}

}  // namespace

int main() {
    // 1. First reading creates exactly one new device, first_seen ==
    // last_seen == the reading's own timestamp.
    {
        std::string dir = fresh_dir("basic");
        LoraMasterList list(dir);
        list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.02, 2.0), 1000);
        auto rows = list.snapshot();
        check(rows.size() == 1, "first_reading_creates_one_device",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) {
            check(rows[0].device_id_str == "LORA-0001", "device_id_format",
                  "got '" + rows[0].device_id_str + "'");
            check(rows[0].first_seen_ts == 1000 && rows[0].last_seen_ts == 1000,
                  "first_seen_eq_last_seen_on_creation",
                  "first=" + std::to_string(rows[0].first_seen_ts) +
                      " last=" + std::to_string(rows[0].last_seen_ts));
            check(rows[0].reading_count == 1, "reading_count_one", "");
        }
    }

    // 2. A second, similar reading (within tolerance) matches the same
    // device - reading count grows, last_seen advances, first_seen
    // stays put.
    {
        std::string dir = fresh_dir("same_device");
        LoraMasterList list(dir);
        list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.020, 2.0), 1000);
        list.record_reading(866.9e6, 7, 125e3, make_fp(-35.5, -41.2, 0.023, 2.8), 1500);
        auto rows = list.snapshot();
        check(rows.size() == 1, "similar_reading_matches_same_device",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) {
            check(rows[0].reading_count == 2, "reading_count_two",
                  "got " + std::to_string(rows[0].reading_count));
            check(rows[0].first_seen_ts == 1000, "first_seen_unchanged",
                  "got " + std::to_string(rows[0].first_seen_ts));
            check(rows[0].last_seen_ts == 1500, "last_seen_advanced",
                  "got " + std::to_string(rows[0].last_seen_ts));
        }
    }

    // 2b. A wildly different CFO on an otherwise-identical reading must
    // NOT prevent a match - cfo_ppm is deliberately excluded from
    // comparison (see lora_master.hpp's file header for why).
    {
        std::string dir = fresh_dir("cfo_excluded");
        LoraMasterList list(dir);
        list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.02, 2.0, /*cfo_ppm=*/5.0),
                             1000);
        list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.02, 2.0, /*cfo_ppm=*/-80.0),
                             1500);
        auto rows = list.snapshot();
        check(rows.size() == 1, "cfo_swing_does_not_split_device",
              "got " + std::to_string(rows.size()) + " devices");
    }

    // 3. A reading well outside tolerance on the matched parameters
    // creates a second, separate device instead of merging.
    {
        std::string dir = fresh_dir("mismatch");
        LoraMasterList list(dir);
        list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.02, 2.0), 1000);
        list.record_reading(867.1e6, 7, 125e3, make_fp(10.0, -5.0, 0.08, -10.0), 1500);
        auto rows = list.snapshot();
        check(rows.size() == 2, "dissimilar_reading_creates_new_device",
              "got " + std::to_string(rows.size()) + " devices");
    }

    // 4. Reading-history cap: recording well past COMPACT_TRIGGER_READINGS
    // must never leave more than MAX_READINGS_PER_DEVICE in the
    // snapshot, and first_seen must survive the pruning.
    {
        std::string dir = fresh_dir("cap");
        LoraMasterList list(dir);
        int64_t ts = 0;
        for (int i = 0; i < MAX_READINGS_PER_DEVICE + 250; ++i) {
            list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.02, 2.0), ts);
            ts += 5;
        }
        auto rows = list.snapshot();
        check(rows.size() == 1, "cap_test_still_one_device",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) {
            // Compaction is deliberately lazy (only triggers once past
            // COMPACT_TRIGGER_READINGS, not the instant the count hits
            // MAX_READINGS_PER_DEVICE - see that constant's own
            // comment), so the count sits somewhere in
            // [MAX_READINGS_PER_DEVICE, COMPACT_TRIGGER_READINGS]
            // between compactions, not pinned to exactly MAX.
            check(rows[0].reading_count >= MAX_READINGS_PER_DEVICE &&
                      rows[0].reading_count <= COMPACT_TRIGGER_READINGS,
                  "reading_count_bounded",
                  "got " + std::to_string(rows[0].reading_count) + ", want in [" +
                      std::to_string(MAX_READINGS_PER_DEVICE) + ", " +
                      std::to_string(COMPACT_TRIGGER_READINGS) + "]");
            check(rows[0].first_seen_ts == 0, "first_seen_survives_pruning",
                  "got " + std::to_string(rows[0].first_seen_ts));
        }
    }

    // 5. Persistence across a simulated restart: a second LoraMasterList
    // pointed at the same directory must reload every device with its
    // original first_seen, its latest last_seen, and its full (capped)
    // reading count intact.
    {
        std::string dir = fresh_dir("persistence");
        {
            LoraMasterList list(dir);
            list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.02, 2.0), 1000);
            list.record_reading(866.9e6, 7, 125e3, make_fp(-35.0, -41.0, 0.021, 2.2), 2000);
            list.record_reading(867.5e6, 9, 500e3, make_fp(15.0, 0.0, 0.09, -20.0), 3000);
        }  // list goes out of scope - nothing left in memory
        LoraMasterList reloaded(dir);
        auto rows = reloaded.snapshot();
        check(rows.size() == 2, "reload_recovers_all_devices",
              "got " + std::to_string(rows.size()) + " devices");
        // rows sorted by last_seen_ts descending - the SF9/BW500 device
        // (last_seen=3000) should come first.
        if (rows.size() == 2) {
            check(rows[0].last_seen_ts == 3000 && rows[0].last_sf == 9,
                  "reload_ordering_most_recent_first",
                  "last_seen=" + std::to_string(rows[0].last_seen_ts) +
                      " sf=" + std::to_string(rows[0].last_sf));
            check(rows[1].first_seen_ts == 1000 && rows[1].last_seen_ts == 2000 &&
                      rows[1].reading_count == 2,
                  "reload_preserves_first_and_last_seen_and_count",
                  "first=" + std::to_string(rows[1].first_seen_ts) +
                      " last=" + std::to_string(rows[1].last_seen_ts) +
                      " count=" + std::to_string(rows[1].reading_count));
        }
    }

    // 6. A brand-new reading recorded against a RELOADED list must still
    // match correctly against history that came from disk, not just
    // in-process history.
    {
        std::string dir = fresh_dir("reload_then_match");
        {
            LoraMasterList list(dir);
            list.record_reading(866.9e6, 7, 125e3, make_fp(-34.0, -40.0, 0.02, 2.0), 1000);
        }
        LoraMasterList reloaded(dir);
        reloaded.record_reading(866.9e6, 7, 125e3, make_fp(-35.0, -41.0, 0.021, 2.3), 5000);
        auto rows = reloaded.snapshot();
        check(rows.size() == 1, "match_against_reloaded_history",
              "got " + std::to_string(rows.size()) + " devices");
        if (!rows.empty()) {
            check(rows[0].reading_count == 2, "reload_then_match_reading_count",
                  "got " + std::to_string(rows[0].reading_count));
        }
    }

    if (failures > 0) {
        std::printf("\n%d check(s) FAILED.\n", failures);
        return 1;
    }
    std::printf("\nAll LoRa master list checks passed.\n");
    return 0;
}
