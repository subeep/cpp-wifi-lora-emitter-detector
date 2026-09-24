// GUI for the passive Wi-Fi security monitor (package A step 4 output).
//
// Shows observations and coverage only. No detector rules exist yet, so
// nothing here is an alert, and the wording avoids implying one: a deauth
// in the list is a received frame, not an attack.
#pragma once

#include <functional>
#include <string>

#include "security/wifi_security_monitor.hpp"

namespace rfmon {

// Header label that stays informative while the section is collapsed.
std::string wifi_security_header_label(const wifi_security::SecuritySnapshot& s);

// Summary strip + Coverage / Frame types / Recent frames tabs. `recording`
// is the configured recording path ("" when recording is off).
// Baseline review actions ("baseline_freeze" / "baseline_unfreeze" /
// "baseline_reset", key "" = all) are sent through `command`; empty
// disables the buttons.
using SecurityCommandFn = std::function<void(const std::string& action, const std::string& key)>;

// `select_tab` ("Coverage", "Frame types", "Recent frames", "Baselines",
// "Incidents") switches tabs programmatically for one frame; nullptr leaves
// the user's choice alone.
void draw_wifi_security_panel(const wifi_security::SecuritySnapshot& s, const wifi_security::QueueStats& q,
                              const std::string& run_id, const std::string& recording, float height,
                              const char* select_tab = nullptr, const SecurityCommandFn& command = {});

// One-line key fields for a frame (reason/status/SSID/TSF/PN/EAPOL...),
// shared by the table and tests.
std::string wifi_security_key_info(const wifi_security::ProcessedFrame& p);

}  // namespace rfmon
