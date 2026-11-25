// The Drones page of the modern workspace: one status strip with a single Start/Stop, the
// newest drone alert, one row per drone ("Drones now") with a details panel, and tabs for
// history, evidence, coverage, diagnostics and offline review.
//
// Display layer only. It reads the same detection rows, tracks and snapshots as the previous
// page, and changes acquisition only through Start/Stop (the coordinated start/stop used
// before) and the existing controls it hosts (schedule editor, independent decoders, IQ
// retention). Detection, evidence rules and logs are unchanged. The previous layout is still
// drawn in the legacy presentation.
#pragma once
#ifdef RFMON_ENABLE_DJI_DRONEID
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coordinated_panel.hpp"
#include "drones/drone_tracks.hpp"
#include "drones/labelled_recorder.hpp"

namespace rfmon::drones_page {
using drone_claims::Clock;
using drone_claims::Detection;

enum Tab { TabHistory, TabEvidence, TabCoverage, TabDiagnostics, TabOffline };

// A deployable universal classifier package (weights + decision file).
struct Package {
    std::string path, label;  // label: "universal_v3  (18 Nov)"
    int version = 0;
};

struct State {
    int tier_filter = -1;          // -1: every level; otherwise a drone_claims::Tier value
    std::string selected;          // Detection::id shown in the details panel
    std::string dismissed_alert;   // alert_key() of the alert the user dismissed
    int select_tab = -1;           // Tab to bring forward on the next frame; -1: none
    float table_height = 0, history_height = 0;  // 0: default; changed by the drag handles
    bool per_drone = true;         // "Drones now": one line per drone group (false: every row)
    std::set<std::string> expanded;  // drone lines opened to their rows, keyed by the lead row's id
    char recorder_dir[512] = "";    // training recorder folder (filled from Inputs on first use)
    std::vector<Package> packages;  // classifier packages found on disk (rescanned every few seconds)
    double packages_scanned = -1;
};

struct Inputs {
    const std::map<std::string, Detection>* rows = nullptr;
    const drone_claims::DroneTracks* tracks = nullptr;
    Clock::time_point now{};
    std::string radio;                     // device label, e.g. "USRP X310"
    bool started = false;                  // a receiver session was started (Connect)
    bool connected = false;                // the receiver is delivering samples
    bool* classifier_only_rows = nullptr;  // development switch for classifier-only rows
    std::vector<std::string> diagnostics;  // extra module lines (timing drops, DroneID follow-up)
    std::vector<std::string> problems;     // shown as warnings (for example log write failures)
    bool select_offline = false;           // bring the Offline review tab forward (Captures link)
    std::string* classifier_package = nullptr;  // path of the package the classifier loads
    std::string default_package;              // marked "default" in the picker
    std::string packages_root;                // searched by find_packages()
    const drone_claims::LabelledRecorder* recorder = nullptr;  // training recorder (status only)
    std::string recorder_default_dir;
};

struct Actions {
    std::function<void()> open_setup;            // Monitoring setup, where the receiver is connected
    std::function<void()> open_model_inference;  // the classifier's scores and spectrograms
    std::function<void()> reload_classifier;     // reload a running classifier with *classifier_package
    std::function<void(const std::string&)> start_recording;  // training recorder: start in this folder
    std::function<void()> stop_recording;
};

void draw(State& state, coordinated::PanelState& coordinated, Scanner& scanner, drone::Addon& drones,
          ml::Inference& model, shortrange::Monitor* bluetooth, const Inputs& in, const Actions& actions);

// Pure helpers (no GUI), covered by tests.
// Rows at the chosen level (-1: all), strongest level first, then most recently seen.
std::vector<const Detection*> filter_rows(const std::map<std::string, Detection>& rows, int tier_filter,
                                          Clock::time_point now);
// Number of rows at each level (drone_claims::Tier value -> count).
std::map<int, std::size_t> tier_counts(const std::map<std::string, Detection>& rows);
// The kinds of evidence behind a row, in a fixed order: Link, DroneID, Remote ID, Classifier.
std::vector<std::string> evidence_kinds(const Detection& row);
// Deployable classifier packages under `root`: <dir>/universal_package/universal_v<N>.pt,
// newest version first. Experiment checkpoints (no decision file) are not listed.
std::vector<Package> find_packages(const std::string& root);
// Which rows probably describe the same aircraft (display only: rows are never merged).
//   certain:   the same serial number (DroneID, or Remote ID matching a DroneID serial);
//   likely:    DroneID packets on the same channel (within 1 MHz) within 10 s of each other,
//              for example one readable and one unreadable message from one drone;
//              or the same crystal (drone_claims::crystal_match): two DroneID rows within
//              0.1 ppm, or a link's 15 kHz grids fitting a DroneID row's ppm in 3+ captures,
//              on either band;
//              Also two link-only Lightbridge rows heard within 45 s of each other (Lightbridge
//              has no DroneID; such rows count as one aircraft unless their link clocks differ);
//   possible:  a looser crystal fit, or DroneID heard within 20 MHz of a measured link, both
//              seen within 45 s;
//   different: the crystals clearly differ (DroneID ppm, link grids, or link clocks from symbol
//              timing), or a decoded model's link type cannot show the other row's timing group
//              (a Mini 2 next to a Lightbridge link). Shown, never joined; it overrides the
//              channel, 20 MHz and Lightbridge rules for that pair.
// Certain and likely links form the drone groups D1, D2, ... (numbered in table order);
// possible links are reported but keep the groups apart.
struct Relation {
    std::string other;   // the related row's Detection::id
    std::string reason;  // in words
    enum Strength { Different, Possible, Likely, Certain } strength = Possible;
};
struct Grouping {
    std::map<std::string, int> drone;  // row id -> drone number, 1-based, numbered in `ordered` order
    std::map<std::string, std::vector<Relation>> related;
    std::size_t drones = 0;   // groups from certain and likely links
    std::size_t fewest = 0;   // groups if every possible link were the same drone too
};
Grouping group_rows(const std::vector<const Detection*>& ordered);
// One table line per drone group (display only: rows are never merged).
struct DroneLine {
    int drone = 0;                       // group number from group_rows (0: not grouped)
    std::vector<const Detection*> rows;  // in table order; rows.front() is the lead (strongest level)
    std::string bands;                   // every band heard, short: "2.4 + 5.8 GHz"
    Clock::time_point last{}, first{};   // newest and earliest sighting over its rows
};
// Groups `ordered` (filter_rows order) by drone number, lines in the order of their lead rows.
std::vector<DroneLine> drone_lines(const std::vector<const Detection*>& ordered, const Grouping& g, Clock::time_point now);
// Bands heard by rows, from their centres: "2.4 GHz", "2.4 + 5.2 + 5.8 GHz".
std::string bands_text(const std::vector<const Detection*>& rows);
// The model a row shows: its own answer, or its drone's lead answer when the lead's level is
// stronger, with what this row is: "Mini 2 · OcuSync 2 (link of D1)".
std::string row_name(const Detection& row, const Detection* lead, const Grouping& g);
// A track alert whose row joined a drone that other rows already showed before the track opened:
// not a new drone, so the page shows no "New drone" alert for it.
bool alert_for_known_drone(const drone_claims::TrackEvent& event, const std::map<std::string, Detection>& rows,
                           const Grouping& g, Clock::time_point now);
// Carrier offsets in words (measurement only; empty when not measured).
//   droneid_offset_text: the row's DroneID packets, in ppm of their documented channel, with
//                        the spread and, when both bands were heard, each band and the difference;
//   link_grid_text:      where the link's subcarrier grid sits, modulo the spacing;
//   link_clock_text:     the link's crystal ppm: Lightbridge frame timing chained over its captures when
//                        good, else symbol timing averaged over its captures.
std::string droneid_offset_text(const Detection& row);
std::string link_grid_text(const Detection& row);
std::string link_clock_text(const Detection& row);
// Identifies one alert so a dismissed alert stays dismissed until a different one arrives.
std::string alert_key(const drone_claims::TrackEvent& event);
// The row a track alert refers to, or "" when no current row matches.
std::string row_for_track(const std::map<std::string, Detection>& rows, const drone_claims::DroneTrack& track);

}  // namespace rfmon::drones_page
#endif
