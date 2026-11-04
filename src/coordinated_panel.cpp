#ifdef RFMON_ENABLE_DJI_DRONEID
#include "coordinated_panel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <map>

#include "gui/app_shell.hpp"
#include "imgui.h"

namespace rfmon::coordinated {
namespace {
double steady_s(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}
const char* slot_name(const std::string& band) {
    return band == BAND_SUB_GHZ ? "Sub-GHz" : band == BAND_WIFI_2G4 ? "2.4 GHz" : band == BAND_WIFI_5G ? "5 GHz" : band.c_str();
}
std::string band_of(double hz) {
    if (hz > 800e6 && hz < 1e9) return "Sub-GHz";
    if (hz > 2.3e9 && hz < 2.6e9) return "2.4 GHz";
    if (hz > 5e9 && hz < 6e9) return "5 GHz";
    return "Other";
}
void add_detector(std::vector<Entry>& out, const drone::Snapshot& s, const char* source) {
    for (const auto& o : s.observations) {
        Entry e;
        e.source = source;
        e.key = std::string(source) + "/" + (o.confirmed ? "serial:" + o.serial
                                             : !o.transmitter.empty() ? "tx:" + o.protocol + "/" + o.transmitter
                                                                      : "f:" + o.protocol + "/" + std::to_string(std::llround(o.frequency / 1e3)));
        e.evidence = o.protocol.empty() ? o.evidence : o.protocol + (o.evidence.empty() ? "" : " - " + o.evidence);
        e.identity = (o.serial.empty() ? std::string("serial not reported") : "reported serial " + o.serial) +
                     (o.model.empty() ? "" : " / reported model " + o.model) + " (unauthenticated)";
        if (o.confirmed) e.reported_id = o.serial;
        e.reported_model = o.model;
        e.frequency_hz = o.frequency;
        e.last_seen_s = steady_s(o.acquired_at);
        e.count = o.count;
        e.capture_seq = evidence::capture_from_source(o.source);
        if (std::isfinite(o.timestamp) && o.timestamp > 0) e.event_device_time_s = o.timestamp;
        e.fields = {{"Link", o.link}, {"Checksum confirmed", o.confirmed ? "yes" : "no"},
                    {"Packet sequence", o.sequence >= 0 ? std::to_string(o.sequence) : "not reported"},
                    {"Relative power (dB, uncalibrated)", std::to_string(o.power)},
                    {"Carrier correction (Hz)", std::to_string(o.carrier_correction_hz)},
                    {"Reported position", o.position_status}, {"Receiver source", o.source}};
        for (const auto& f : o.telemetry) e.fields.push_back({f.group + " / " + f.name, f.value + (f.status.empty() ? "" : " (" + f.status + ")")});
        e.raw_json = o.packet_json.empty() ? o.remote_id_json : o.packet_json;
        out.push_back(std::move(e));
    }
}

// Native worker JSON (written by boost::property_tree, so values are strings).
std::string jtext(const nlohmann::json& j, const char* k) {
    if (!j.is_object() || !j.contains(k)) return "";
    return j[k].is_string() ? j[k].get<std::string>() : j[k].dump();
}
double jnum(const nlohmann::json& j, const char* k) {
    const auto t = jtext(j, k);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    return end != t.c_str() && std::isfinite(v) ? v : 0;
}
bool jbool(const nlohmann::json& j, const char* k) { const auto t = jtext(j, k); return t == "true" || t == "1"; }
std::vector<std::pair<std::string, std::string>> all_fields(const nlohmann::json& j) {
    std::vector<std::pair<std::string, std::string>> f;
    for (const auto& [k, v] : j.items()) f.push_back({k, v.is_string() ? v.get<std::string>() : v.dump()});
    return f;
}
// End of the visit that produced capture `seq` (the evidence's acquisition time).
double capture_time(const std::optional<uint64_t>& seq, const std::deque<coord::VisitRecord>& visits) {
    if (seq)
        for (const auto& v : visits)
            if (v.capture_seq == *seq) return v.in_progress ? v.start_s : v.end_s;
    return 0;
}

// CRC-valid DJI DroneID packets from the native worker (it reports them as
// packets, not observations): one row per reported serial, or per envelope
// and centre when the envelope carries no serial.
void add_native_packets(std::vector<Entry>& out, const drone::Snapshot& s, const std::deque<coord::VisitRecord>& visits) {
    std::map<std::string, Entry> rows;
    std::vector<std::string> order;
    for (const auto& text : s.native_packets) {
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded()) continue;
        const std::string serial = jtext(j, "reported_serial"), model = jtext(j, "reported_model"), envelope = jtext(j, "envelope");
        const double center = jnum(j, "center_hz");
        const std::string key = "native-packet/" + (serial.empty() ? envelope + "/" + std::to_string(std::llround(center / 1e3)) : "serial:" + serial);
        if (!rows.count(key)) order.push_back(key);
        Entry& e = rows[key];
        e.key = key;
        e.source = "DJI native packets";
        ++e.count;
        e.evidence = std::string("DroneID packet: CRC24 OK") + (jbool(j, "inner_crc16") ? ", CRC16 OK" : "") + ", envelope " +
                     (envelope.empty() ? "?" : envelope) + (jbool(j, "telemetry_parsed") ? ", telemetry parsed" : ", telemetry not parsed");
        e.identity = serial.empty() ? "no serial in this envelope; identity not decoded"
                                    : "reported serial " + serial + (model.empty() ? "" : " / reported model " + model) + " (unauthenticated)";
        e.reported_id = serial;
        e.reported_model = model;
        e.frequency_hz = center;
        e.capture_seq = evidence::capture_from_source(jtext(j, "receiver_source"));
        const double visit = capture_time(e.capture_seq, visits);
        e.last_seen_s = visit > 0 ? visit : jnum(j, "acquired_steady_s");
        if (const double t = jnum(j, "timestamp"); t > 0) e.event_device_time_s = t;
        e.fields = all_fields(j);
        e.raw_json = text;
    }
    for (const auto& k : order) out.push_back(std::move(rows[k]));
}

// Experimental O4 structure evidence: one row while any window has been a
// candidate or the latest window shows structure. Never an identity.
void add_native_o4(std::vector<Entry>& out, const drone::Snapshot& s, const std::deque<coord::VisitRecord>& visits) {
    if (s.o4_latest.empty()) return;
    const auto j = nlohmann::json::parse(s.o4_latest, nullptr, false);
    if (j.is_discarded()) return;
    const bool structure = jbool(j, "candidate") || jbool(j, "structure_match") || jbool(j, "weak_structure_match");
    if (s.o4_candidates == 0 && !structure) return;
    Entry e;
    e.key = "native-o4";
    e.source = "DJI native O4 structure";
    e.evidence = std::to_string(s.o4_candidates) + " candidate windows of " + std::to_string(s.o4_windows) +
                 " (experimental O4 structure); latest: " + jtext(j, "reason");
    e.identity = "no identity (structure evidence only)";
    e.frequency_hz = jnum(j, "center_hz");
    e.count = s.o4_candidates;
    e.capture_seq = evidence::capture_from_source(jtext(j, "receiver_source"));
    const double visit = capture_time(e.capture_seq, visits);
    e.last_seen_s = visit > 0 ? visit : jnum(j, "acquired_steady_s");
    if (const double t = jnum(j, "timestamp"); t > 0) e.event_device_time_s = t;
    e.fields = all_fields(j);
    e.raw_json = s.o4_latest;
    out.push_back(std::move(e));
}

// ASTM F3411 over Bluetooth LE from the Bluetooth & Zigbee decoder.
void add_bluetooth_rid(std::vector<Entry>& out, const shortrange::Snapshot& b) {
    for (const auto& d : b.ble) {
        if (d.remote_id.empty()) continue;
        Entry e;
        e.key = "ble-rid/" + d.key;
        e.source = "Bluetooth Remote ID";
        e.evidence = "CRC-valid BLE legacy advertising, ASTM F3411, " + std::to_string(d.remote_id.size()) + " message types from " +
                     d.address + " (" + d.address_kind + ")";
        e.identity = "ID unavailable";
        nlohmann::json raw = nlohmann::json::object();
        for (const auto& [type, text] : d.remote_id) {
            const auto m = nlohmann::json::parse(text, nullptr, false);
            raw[type] = m.is_discarded() ? nlohmann::json(text) : m;
            if (m.is_discarded()) { e.fields.push_back({type, text}); continue; }
            if (m.value("type", std::string()) == "Basic ID" && e.reported_id.empty()) {
                e.reported_id = m.value("reported_id", m.value("reported_id_hex", std::string()));
                e.identity = "reported ID " + e.reported_id + " (" + m.value("ua_type_name", std::string("category not reported")) +
                             ", unauthenticated)";
            }
            std::string summary;
            for (const auto& [k, v] : m.items())
                if (k != "type" && k != "raw_hex" && !v.is_object() && !v.is_array())
                    summary += (summary.empty() ? "" : ", ") + k + "=" + (v.is_string() ? v.get<std::string>() : v.dump());
            e.fields.push_back({type, summary});
        }
        int best = 0;
        uint64_t most = 0;
        for (const auto& [c, k] : d.channel_counts)
            if (k > most) { most = k; best = c; }
        e.frequency_hz = shortrange::ble::adv_channel_frequency(best);
        e.last_seen_s = steady_s(d.last_seen);
        e.count = d.packets;
        e.capture_seq = evidence::capture_from_source(d.last_source);
        e.fields.push_back({"Advertiser", d.address + " (" + d.address_kind + ")"});
        e.fields.push_back({"Power last / max (dBFS, relative)", std::to_string(d.last_power_db) + " / " + std::to_string(d.max_power_db)});
        e.fields.push_back({"Receiver source", d.last_source});
        e.raw_json = raw.dump();
        out.push_back(std::move(e));
    }
}
}  // namespace

std::vector<Entry> build_entries(const drone::Snapshot& native, const drone::Snapshot& legacy, const drone::RidJson& rid,
                                 const ml::Snapshot& model, const std::deque<coord::VisitRecord>& visits, double now_s,
                                 double expire_s, const shortrange::Snapshot* bluetooth) {
    std::vector<Entry> out;
    add_detector(out, native, "DJI native");
    add_native_packets(out, native, visits);
    add_native_o4(out, native, visits);
    if (bluetooth) add_bluetooth_rid(out, *bluetooth);
    add_detector(out, legacy, "DJI fallback");
    if (rid.is_object() && rid.contains("tracks"))
        for (const auto& t : rid["tracks"]) {
            Entry e;
            e.source = "Wi-Fi Remote ID";
            e.key = "rid/" + t.value("key", std::string());
            e.evidence = "FCS-valid " + t.value("transport", std::string("Wi-Fi")) + " Remote ID v" +
                         std::to_string(t.value("protocol_version", 0)) + " from " + t.value("transmitter", std::string("?"));
            e.identity = "ID unavailable";
            double youngest = 1e18;
            for (const auto& m : t.value("messages", drone::RidJson::array())) {
                const std::string type = m.value("type", std::string());
                if (type == "Basic ID" && e.reported_id.empty()) {
                    e.reported_id = m.value("reported_id", m.value("reported_id_hex", std::string()));
                    e.identity = "reported ID " + e.reported_id + " (" +
                                 m.value("ua_type_name", std::string("category not reported")) + ", unauthenticated)";
                }
                const double age = m.value("age_s", 1e18);
                if (age < youngest && m.contains("source") && m["source"].is_object()) {
                    const auto& src = m["source"];
                    e.frequency_hz = src.value("channel_hz", 0.0);
                    if (src.contains("capture_seq") && src["capture_seq"].is_number()) e.capture_seq = src["capture_seq"].get<uint64_t>();
                    if (src.contains("device_time_ns") && src["device_time_ns"].is_number())
                        e.event_device_time_s = src["device_time_ns"].get<double>() * 1e-9;
                }
                youngest = std::min(youngest, age);
                std::string summary;
                for (const auto& [k, v] : m.items())
                    if (k != "source" && k != "type" && !v.is_object() && !v.is_array())
                        summary += (summary.empty() ? "" : ", ") + k + "=" + (v.is_string() ? v.get<std::string>() : v.dump());
                e.fields.push_back({type, summary});
            }
            e.last_seen_s = youngest < 1e17 ? now_s - youngest : 0;
            e.count = t.value("packs", uint64_t(0));
            e.raw_json = t.dump();
            out.push_back(std::move(e));
        }
    // The model has no qualified presence decision: its latest result is shown
    // as a diagnostic candidate only, never as a detection.
    if (!model.history.empty()) {
        const auto& r = model.history.front();
        Entry e;
        e.source = "Model (diagnostic)";
        e.key = "model/latest";
        e.evidence = ml::observation_decision(r);
        e.identity = r.candidate.empty() ? "no candidate" : "candidate " + r.candidate + " (not accepted)";
        e.frequency_hz = r.center;
        e.last_seen_s = steady_s(r.completed);
        e.count = 1;
        if (r.capture) e.capture_seq = r.capture;
        e.fields = {{"Status", r.status}, {"Reason", r.reason}, {"Model", r.model_hash.substr(0, 12) + " / " + r.architecture},
                    {"Interval", std::to_string(r.interval_index + 1) + " of " + std::to_string(r.interval_count) +
                                     " at sample " + std::to_string(r.offset_samples)},
                    {"Processing (ms)", std::to_string(r.processing_ms)}};
        for (const auto& c : r.top) e.fields.push_back({"Score: " + c.label, std::to_string(c.score) + " (relative, not a probability)"});
        e.raw_json = r.raw;
        out.push_back(std::move(e));
    }
    for (auto& e : out) e.freshness = coord::freshness(e.frequency_hz, e.last_seen_s, now_s, visits, expire_s);
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.last_seen_s > b.last_seen_s; });
    evidence::associate(out);
    return out;
}

namespace {
void draw_strip(const CoordinatedStatus& st, const coord::Schedule& schedule, double now) {
    const std::string current_band = st.current ? st.current->band : std::string();
    for (size_t i = 0; i < schedule.slots.size(); ++i) {
        const auto& slot = schedule.slots[i];
        if (i) { ImGui::SameLine(); ImGui::TextUnformatted("->"); ImGui::SameLine(); }
        char label[96];
        std::snprintf(label, sizeof label, "%s  %d x %.0f ms%s", slot_name(slot.band), slot.visits_per_cycle,
                      slot.dwell_s * 1e3, slot.enabled ? "" : " (off)");
        const bool now_here = slot.band == current_band;
        ImGui::PushStyleColor(ImGuiCol_Button, now_here ? ImVec4(.16f, .45f, .28f, 1) : ImVec4(.22f, .24f, .28f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, now_here ? ImVec4(.16f, .45f, .28f, 1) : ImVec4(.22f, .24f, .28f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, now_here ? ImVec4(.16f, .45f, .28f, 1) : ImVec4(.22f, .24f, .28f, 1));
        ImGui::Button(label);
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("-> repeat");
    if (st.current) {
        const auto& v = *st.current;
        double dwell = 0;
        for (const auto& slot : schedule.slots) if (slot.band == v.band) dwell = slot.dwell_s + schedule.preroll_s + schedule.settle_s;
        ImGui::Text("Receiving: %s | centre %.3f MHz | %.3f MSps | usable +/-%.1f MHz | visit time left %.2f s",
                    v.label.c_str(), v.center_hz / 1e6, v.rate_hz / 1e6, v.usable_half_span_hz / 1e6,
                    std::max(0.0, dwell - (now - v.start_s)));
    } else {
        ImGui::TextDisabled("No visit in progress.");
    }
    ImGui::Text("Cycle %llu | last cycle %.2f s measured | configured %.2f s (%.3f s valid + %.3f s pre-roll/settle, host overhead excluded)",
                static_cast<unsigned long long>(st.cycle), st.last_cycle_s, st.estimate.cycle_s, st.estimate.valid_s,
                st.estimate.overhead_s);
}

void draw_coverage(const CoordinatedStatus& st, double now) {
    if (!ImGui::BeginTable("Coordinated coverage", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        return;
    for (const char* h : {"Band", "Visits", "Valid", "Analysed", "Lane drops", "Valid time (s)", "Analysed %", "Revisit per centre: configured / measured", "Last visit"})
        ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < st.slots.size(); ++i) {
        const auto& s = st.slots[i];
        const auto est = i < st.estimate.slots.size() ? st.estimate.slots[i] : coord::SlotEstimate{};
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s%s", slot_name(s.band), s.enabled ? "" : " (off)");
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(s.visits));
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(s.valid_visits));
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(s.analysed_visits));
        ImGui::TableNextColumn();
        if (s.lane_drops) ImGui::TextColored(ImVec4(1, .7f, .35f, 1), "%llu", static_cast<unsigned long long>(s.lane_drops));
        else ImGui::TextUnformatted("0");
        ImGui::TableNextColumn(); ImGui::Text("%.2f", s.acquired_s);
        ImGui::TableNextColumn(); ImGui::Text("%.0f%%", s.acquired_s > 0 ? 100 * s.analysed_s / s.acquired_s : 0.0);
        ImGui::TableNextColumn();
        if (s.last_sweep_s > 0) ImGui::Text("%.1f s / %.1f s (%zu centres)", est.sweep_s, s.last_sweep_s, s.list_size);
        else ImGui::Text("%.1f s / pending (%zu centres)", est.sweep_s, s.list_size);
        ImGui::TableNextColumn();
        if (s.last_visit_end_s > 0) ImGui::Text("%.1f s ago", now - s.last_visit_end_s);
        else ImGui::TextDisabled("never");
    }
    ImGui::EndTable();
}

void draw_editor(PanelState& state, Scanner& scanner, double max_rate) {
    if (!ImGui::CollapsingHeader("Schedule settings (applies at the next cycle boundary)")) return;
    if (!state.editor_loaded) { state.editor = scanner.effective_coordinated_schedule(); state.editor_loaded = true; }
    auto& s = state.editor;
    ImGui::PushID("coordinated_editor");
    for (auto& slot : s.slots) {
        ImGui::PushID(slot.band.c_str());
        ImGui::Checkbox(slot_name(slot.band), &slot.enabled);
        ImGui::SameLine(140);
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("visits / cycle", &slot.visits_per_cycle);
        ImGui::SameLine();
        double dwell_ms = slot.dwell_s * 1e3;
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputDouble("dwell (ms)", &dwell_ms, 0, 0, "%.0f")) slot.dwell_s = dwell_ms / 1e3;
        ImGui::SameLine();
        ImGui::TextDisabled("%zu centres", slot.visits.size());
        ImGui::PopID();
    }
    double settle_ms = s.settle_s * 1e3, preroll_ms = s.preroll_s * 1e3;
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputDouble("settle per visit (ms)", &settle_ms, 0, 0, "%.0f")) s.settle_s = settle_ms / 1e3;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputDouble("pre-roll (ms)", &preroll_ms, 0, 0, "%.0f")) s.preroll_s = preroll_ms / 1e3;
    const auto e = coord::estimate(s);
    const auto error = coord::validate(s, max_rate);
    ImGui::Text("Preview: cycle %.2f s (%.3f s valid). Average revisit per centre:", e.cycle_s, e.valid_s);
    for (const auto& se : e.slots) { ImGui::SameLine(); ImGui::Text("%s %.1f s", slot_name(se.band), se.sweep_s); }
    if (!error.empty()) gui::note(error.c_str(), true);
    ImGui::BeginDisabled(!error.empty());
    if (ImGui::Button("Apply schedule")) { scanner.set_coordinated_schedule(s); state.editor_message = "Applied; takes effect at the next cycle."; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Restore default")) {
        scanner.set_coordinated_schedule(std::nullopt);
        state.editor = scanner.effective_coordinated_schedule();
        state.editor_message = "Default restored.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload current")) state.editor_loaded = false;
    if (!state.editor_message.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", state.editor_message.c_str()); }
    gui::note("A longer dwell catches slower periodic broadcasts but lengthens every other band's revisit time. No schedule guarantees detection on one time-shared receiver.");
    ImGui::PopID();
}

std::string lower_text(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Drag handle under a table: changes `height`.
void draw_entries(PanelState& state, const std::vector<Entry>& entries, double now) {
    std::map<std::string, size_t> per_source;
    for (const auto& e : entries) ++per_source[e.source];
    ImGui::Text("Entries: %zu (rows are observations, not a count of physical aircraft). Select a row to inspect it.", entries.size());
    std::string sources;
    for (const auto& [src, n] : per_source) sources += (sources.empty() ? "" : " | ") + src + " " + std::to_string(n);
    ImGui::TextDisabled("%s", sources.empty() ? "No source has reported anything yet." : sources.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##entries filter", "Search source, evidence, identity, frequency", state.entries_filter,
                             sizeof state.entries_filter);
    const std::string filter = lower_text(state.entries_filter);
    if (state.entries_height <= 0) state.entries_height = 260;
    // Resizable / reorderable / hideable columns (right-click a header); the
    // drag handle below sets the height.
    if (!ImGui::BeginTable("Combined drone entries", 8,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                           ImVec2(0, state.entries_height)))
        return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 50);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 160);
    ImGui::TableSetupColumn("Evidence", ImGuiTableColumnFlags_WidthFixed, 340);
    ImGui::TableSetupColumn("Identity (claim / candidate)", ImGuiTableColumnFlags_WidthFixed, 300);
    ImGui::TableSetupColumn("Band / MHz", ImGuiTableColumnFlags_WidthFixed, 140);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("Freshness", ImGuiTableColumnFlags_WidthFixed, 200);
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (!filter.empty()) {
            char f[32];
            std::snprintf(f, sizeof f, "%.3f", e.frequency_hz / 1e6);
            const std::string hay = lower_text(e.source + " " + e.evidence + " " + e.identity + " " + band_of(e.frequency_hz) + " " + f);
            if (hay.find(filter) == std::string::npos) continue;
        }
        ImGui::PushID(e.key.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char label[32];
        std::snprintf(label, sizeof label, "%zu%s%s", i + 1, e.relations.empty() ? "" : " ~", e.conflicts.empty() ? "" : " !");
        if (ImGui::Selectable(label, state.selected_key == e.key, ImGuiSelectableFlags_SpanAllColumns)) state.selected_key = e.key;
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.source.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", e.evidence.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", e.identity.c_str());
        ImGui::TableNextColumn();
        if (e.frequency_hz > 0) ImGui::Text("%s / %.3f", band_of(e.frequency_hz).c_str(), e.frequency_hz / 1e6);
        else ImGui::TextDisabled("unknown");
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(e.count));
        ImGui::TableNextColumn();
        if (e.last_seen_s > 0) ImGui::Text("%.1f s ago", now - e.last_seen_s);
        else ImGui::TextDisabled("unknown");
        ImGui::TableNextColumn();
        const bool live = e.freshness == coord::Freshness::ReceivingNow;
        if (live) ImGui::TextColored(ImVec4(.45f, .85f, .55f, 1), "%s", coord::freshness_name(e.freshness));
        else ImGui::TextWrapped("%s", coord::freshness_name(e.freshness));
        ImGui::PopID();
    }
    ImGui::EndTable();
    gui::height_handle("##entries height", state.entries_height);
    ImGui::TextDisabled("~ provisional relationship to another row   ! conflicting claims   Right-click a header to show or hide columns.");
}

void draw_inspector(PanelState& state, Scanner& scanner, const std::vector<Entry>& entries,
                    const std::deque<coord::VisitRecord>& visits, const std::string& schedule_hash, double now) {
    ImGui::SeparatorText("Selected entry");
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.key == state.selected_key; });
    if (it == entries.end()) {
        ImGui::TextDisabled(state.selected_key.empty() ? "No entry selected." : "The selected entry is no longer retained by its detector.");
    } else {
        const auto& e = *it;
        ImGui::Text("%s | %s", e.source.c_str(), e.identity.c_str());
        ImGui::TextWrapped("Evidence: %s", e.evidence.c_str());
        ImGui::Text("Frequency %.3f MHz | count %llu | last seen %.1f s ago | %s", e.frequency_hz / 1e6,
                    static_cast<unsigned long long>(e.count), now - e.last_seen_s, coord::freshness_name(e.freshness));
        // Source interval: the exact visit capture the latest evidence came from.
        const coord::VisitRecord* source_visit = nullptr;
        if (e.capture_seq)
            for (const auto& v : visits) if (v.capture_seq == *e.capture_seq) source_visit = &v;
        if (source_visit)
            ImGui::Text("Source interval: capture #%llu, visit %llu (%s, centre %.3f MHz, %.3f s, %s, %s), cycle %llu",
                        static_cast<unsigned long long>(source_visit->capture_seq), static_cast<unsigned long long>(source_visit->visit_seq),
                        source_visit->label.c_str(), source_visit->center_hz / 1e6, source_visit->end_s - source_visit->start_s,
                        source_visit->valid ? "valid" : "NOT valid", source_visit->analysed ? "analysed" : "not analysed",
                        static_cast<unsigned long long>(source_visit->cycle));
        else if (e.capture_seq)
            ImGui::Text("Source interval: capture #%llu (older than the retained visit history)", static_cast<unsigned long long>(*e.capture_seq));
        else
            ImGui::TextDisabled("Source interval: not reported by this detector.");
        if (e.event_device_time_s) ImGui::Text("Evidence device time: %.6f s", *e.event_device_time_s);
        for (const auto& c : e.conflicts) gui::note(("Conflict: " + c).c_str(), true);
        if (!e.relations.empty() && ImGui::TreeNodeEx("Provisional relationships", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& r : e.relations) ImGui::BulletText("%s", r.c_str());
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Coverage timeline for this frequency (newest first)")) {
            int shown = 0;
            for (auto v = visits.rbegin(); v != visits.rend() && shown < 12; ++v) {
                if (std::abs(e.frequency_hz - v->center_hz) > v->usable_half_span_hz) continue;
                ++shown;
                const bool here = e.capture_seq && v->capture_seq == *e.capture_seq;
                ImGui::BulletText("%.1f s ago | %s | %s%s%s", now - v->start_s, v->label.c_str(),
                                  v->in_progress ? "receiving" : v->valid ? "valid" : "NOT valid",
                                  v->in_progress ? "" : v->analysed ? ", analysed" : ", not analysed", here ? "  <- latest evidence" : "");
            }
            if (!shown) ImGui::TextDisabled("No retained visit covered this frequency.");
            ImGui::TreePop();
        }
        if (!e.fields.empty() && ImGui::TreeNode("Decoded / diagnostic fields")) {
            if (ImGui::BeginTable("Entry fields", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
                for (const auto& [k, v] : e.fields) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextWrapped("%s", k.c_str());
                    ImGui::TableNextColumn(); ImGui::TextWrapped("%s", v.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        if (!e.raw_json.empty() && ImGui::TreeNode("Raw detector output")) {
            ImGui::TextWrapped("%s", e.raw_json.c_str());
            ImGui::TreePop();
        }
        const bool retained = e.capture_seq && scanner.retained_capture(*e.capture_seq).has_value();
        ImGui::Text("IQ for this entry's capture: %s", retained ? "retained" : e.capture_seq ? "not retained" : "no capture reference");
        const bool busy = state.export_job.valid();
        ImGui::BeginDisabled(busy || state.export_dir.empty());
        if (ImGui::Button("Export evidence")) {
            const auto capture = e.capture_seq ? scanner.retained_capture(*e.capture_seq) : std::nullopt;
            state.export_message = "Exporting...";
            state.export_job = std::async(std::launch::async, [dir = state.export_dir, e, visits, schedule_hash, capture, now] {
                return evidence::export_entry(dir, e, visits, schedule_hash, capture, now);
            });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Writes evidence.json%s under %s", retained ? " + IQ around the evidence (+/-50 ms)" : "", state.export_dir.c_str());
    }
    if (state.export_job.valid() && state.export_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { state.export_message = "Saved: " + state.export_job.get(); }
        catch (const std::exception& ex) { state.export_message = std::string("Export failed: ") + ex.what(); }
    }
    if (!state.export_message.empty()) ImGui::TextWrapped("%s", state.export_message.c_str());
    // Retention is opt-in because it holds whole captures in memory.
    bool retain = scanner.iq_retention() > 0;
    if (ImGui::Checkbox("Retain recent Wi-Fi/drone visit IQ for evidence export", &retain))
        scanner.set_iq_retention(retain ? size_t(std::max(1, state.retention_captures)) : 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("captures", &state.retention_captures)) {
        state.retention_captures = std::clamp(state.retention_captures, 1, 64);
        if (retain) scanner.set_iq_retention(size_t(state.retention_captures));
    }
    const auto status = scanner.status();
    const double capture_mb = status.wifi_actual_rate_hz.value_or(20e6) * 0.52 * 8 / 1e6;
    ImGui::TextDisabled("About %.0f MB per retained coordinated capture; %zu retained now. Evidence that arrives after its capture was evicted exports metadata only.",
                        capture_mb, scanner.retained_capture_seqs().size());
}
}  // namespace

void draw_entry_views(PanelState& state, Scanner& scanner, const std::vector<Entry>& entries,
                      const std::deque<coord::VisitRecord>& visits, const std::string& schedule_hash, double now) {
    draw_entries(state, entries, now);
    draw_inspector(state, scanner, entries, visits, schedule_hash, now);
}


void draw_section(PanelState& state, Scanner& scanner, drone::Addon& drones, ml::Inference& model, bool connected,
                  shortrange::Monitor* bluetooth) {
    ImGui::SeparatorText("Coordinated detection: Sub-GHz -> 2.4 GHz -> 5 GHz -> repeat");
    const std::string band = scanner.active_band();
    const bool coordinated = band == BAND_COORDINATED;
    const auto st = scanner.coordinated_status();
    const double now = Scanner::now_steady_s();
    if (!coordinated) {
        if (ImGui::Button("Start coordinated detection")) {
            state.previous_band = band;
            scanner.set_active_band(BAND_COORDINATED);
            state.started_unified = !drones.unified;
            if (state.started_unified) drones.start_unified();
        }
        ImGui::SameLine();
        ImGui::TextWrapped("One receiver visits each band in turn; every applicable module analyses each visit. The current mode (%s) resumes on Stop.",
                           gui::band_label(band).c_str());
    } else {
        if (ImGui::Button("Stop coordinated detection")) {
            scanner.set_active_band(state.previous_band == BAND_COORDINATED ? std::string(BAND_WIFI_2G4) : state.previous_band);
            if (state.started_unified && drones.unified) drones.stop_unified();
            state.started_unified = false;
        }
        ImGui::SameLine();
        ImGui::Text("Receiver: %s | schedule %s", connected ? "connected" : "not connected - connect in Settings", st.config_hash.c_str());
    }
    if (!st.conflict.empty()) {
        gui::note(st.conflict.c_str(), true);
        if (scanner.wifi_fixed_channel() && ImGui::Button("Clear fixed Wi-Fi channel")) scanner.set_wifi_fixed_channel(std::nullopt);
        if (scanner.lora_lock_freq() && ImGui::Button("Clear LoRa frequency lock")) scanner.set_lora_lock_freq(std::nullopt);
    }
    if (!st.config_error.empty()) gui::note(("Schedule not run: " + st.config_error).c_str(), true);

    const auto schedule = scanner.effective_coordinated_schedule();
    if (coordinated) {
        draw_strip(st, schedule, now);
        draw_coverage(st, now);
        if (st.overflows || st.empty_captures)
            gui::note(("Receiver overflows in " + std::to_string(st.overflows) + " visit(s), empty captures " +
                       std::to_string(st.empty_captures) + ": those visits are not valid coverage.").c_str(), true);
    } else {
        const auto e = coord::estimate(schedule);
        ImGui::Text("Configured: cycle %.2f s (%.3f s valid observation).", e.cycle_s, e.valid_s);
    }
    draw_editor(state, scanner, device_profile(scanner.device_type()).max_sample_rate_hz);

    // Coverage-aware freshness needs a retention limit longer than a full
    // sweep; three sweeps (at least a minute) before an entry becomes history.
    double longest_sweep = 0;
    for (const auto& se : coord::estimate(schedule).slots) longest_sweep = std::max(longest_sweep, se.sweep_s);
    const double expire_s = std::max(60.0, 3 * longest_sweep);
    static drone::RidJson rid;
    static double rid_time = -1;
    if (rid.is_null() || ImGui::GetTime() - rid_time > .25) { rid = drones.remote_id.snapshot(); rid_time = ImGui::GetTime(); }
    std::optional<shortrange::Snapshot> ble;
    if (bluetooth) ble = bluetooth->snapshot();
    const auto entries = build_entries(drones.native.snapshot(), drones.live.snapshot(), rid, model.snapshot(),
                                       scanner.coordinated_visits(), now, expire_s, ble ? &*ble : nullptr);
    ImGui::SeparatorText("All detection entries");
    const auto visits = scanner.coordinated_visits();
    draw_entry_views(state, scanner, entries, visits, st.config_hash, now);

    ImGui::SeparatorText("Module health");
    const auto native = drones.native.snapshot();
    const auto ml = model.snapshot();
    ImGui::Text("DJI native: %s | messages %llu | queue %zu | dropped samples %llu | discontinuities %llu",
                native.status.c_str(), static_cast<unsigned long long>(drones.native_chunks.load()), native.queue_depth,
                static_cast<unsigned long long>(native.dropped), static_cast<unsigned long long>(native.discontinuities));
    ImGui::Text("Model: %s | processed %llu | dropped %llu | stale %llu | invalid %llu", ml.state.c_str(),
                static_cast<unsigned long long>(ml.processed), static_cast<unsigned long long>(ml.dropped),
                static_cast<unsigned long long>(ml.stale), static_cast<unsigned long long>(ml.invalid));
    ImGui::Text("Wi-Fi Remote ID: %s | DJI fallback: %s | Bluetooth Remote ID: %s", drones.remote_id.enabled() ? "enabled" : "OFF",
                drones.live.running() ? "running" : "OFF",
                !ble ? "not connected" : ble->enabled ? "decoding (Bluetooth & Zigbee page)" : "OFF - start it on the Bluetooth & Zigbee page");
    gui::note("Sub-GHz: energy scan + LoRa only. The drone model is not supported there (it needs 20 MSps 2.4/5 GHz views); no sub-GHz drone decision is made.", true);
    gui::note("LoRa is decoded from each sub-GHz visit on derived 500/250/125 kHz channels; packets longer than a visit (slow SF11-12) can be cut.");
    gui::note("Classifier running is not classifier qualified: no presence model is qualified, so no 'no drone' or confirmed-drone decision is shown.");
}

}  // namespace rfmon::coordinated
#endif
