// Coordinated detection section of the Drones page (upgradedplan.md §3, P2):
// start/stop, the sub-GHz -> 2.4 -> 5 GHz schedule strip, per-band coverage,
// the schedule editor, one combined table of drone-relevant entries from
// every detector with coverage-aware freshness, and an inspector with
// provenance, relationships/conflicts and evidence export. Display only: it
// reads snapshots and changes acquisition only through explicit controls.
#pragma once
#ifdef RFMON_ENABLE_DJI_DRONEID
#include <deque>
#include <future>
#include <string>
#include <vector>

#include "coordinated_schedule.hpp"
#include "drone_evidence.hpp"
#include "drones/addon.hpp"
#include "ml_inference.hpp"
#include "scanner.hpp"

namespace rfmon::coordinated {

using Entry = evidence::Entry;

// Builds the rows from detector snapshots, sorted newest first, with
// freshness and provisional relationships filled in.
std::vector<Entry> build_entries(const drone::Snapshot& native, const drone::Snapshot& legacy, const drone::RidJson& rid,
                                 const ml::Snapshot& model, const std::deque<coord::VisitRecord>& visits, double now_s,
                                 double expire_s);

struct PanelState {
    std::string previous_band = BAND_WIFI_2G4;  // restored by Stop
    bool started_unified = false;
    bool editor_loaded = false;
    coord::Schedule editor;
    std::string editor_message;
    std::string selected_key;  // inspector selection (Entry::key)
    std::string export_dir;    // parent directory for evidence exports
    int retention_captures = 4;
    std::future<std::string> export_job;
    std::string export_message;
};

void draw_section(PanelState& state, Scanner& scanner, drone::Addon& drones, ml::Inference& model, bool connected);
// The combined table plus the selected entry's inspector (part of draw_section).
void draw_entry_views(PanelState& state, Scanner& scanner, const std::vector<Entry>& entries,
                      const std::deque<coord::VisitRecord>& visits, const std::string& schedule_hash, double now);

}  // namespace rfmon::coordinated
#endif
