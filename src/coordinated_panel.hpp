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
#include <set>
#include <string>
#include <vector>

#include "coordinated_schedule.hpp"
#include "drone_evidence.hpp"
#include "drones/addon.hpp"
#include "ml_inference.hpp"
#include "scanner.hpp"
#include "shortrange/monitor.hpp"

namespace rfmon::coordinated {

using Entry = evidence::Entry;

// Builds the rows from detector snapshots, sorted newest first, with
// freshness and provisional relationships filled in. Sources: DJI native
// observations, packets and O4 structure evidence; the DJI fallback; Wi-Fi
// Remote ID; Bluetooth Remote ID (when `bluetooth` is given); the model.
std::vector<Entry> build_entries(const drone::Snapshot& native, const drone::Snapshot& legacy, const drone::RidJson& rid,
                                 const ml::Snapshot& model, const std::deque<coord::VisitRecord>& visits, double now_s,
                                 double expire_s, const shortrange::Snapshot* bluetooth = nullptr);

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
    float entries_height = 0;          // 0: default; dragged by the handle under the table
    char entries_filter[128] = "";
    std::set<std::string> hidden_sources;  // Entry::source values left out of the table (Evidence tab filter)
};

// Start / Stop coordinated detection: switch the scanner to the coordinated
// schedule (starting the unified drone service if it is not running), or
// return to the previous band and stop what Start started.
void start_detection(PanelState& state, Scanner& scanner, drone::Addon& drones);
void stop_detection(PanelState& state, Scanner& scanner, drone::Addon& drones);

// Pieces of draw_section, also used by the Drones page (drones_page.hpp):
// schedule conflicts / errors with their clear buttons; the schedule strip,
// per-band coverage and schedule editor; the combined entries with a source
// filter and the inspector; and the module health lines.
void draw_schedule_problems(Scanner& scanner);
void draw_coverage_view(PanelState& state, Scanner& scanner);
void draw_evidence_view(PanelState& state, Scanner& scanner, drone::Addon& drones, shortrange::Monitor* bluetooth);
void draw_module_health(drone::Addon& drones, ml::Inference& model, shortrange::Monitor* bluetooth);

void draw_section(PanelState& state, Scanner& scanner, drone::Addon& drones, ml::Inference& model, bool connected,
                  shortrange::Monitor* bluetooth = nullptr);
// The combined table plus the selected entry's inspector (part of draw_section).
void draw_entry_views(PanelState& state, Scanner& scanner, const std::vector<Entry>& entries,
                      const std::deque<coord::VisitRecord>& visits, const std::string& schedule_hash, double now);

}  // namespace rfmon::coordinated
#endif
