#ifdef RFMON_ENABLE_DJI_DRONEID
#include "drones_page.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <set>

#include "drones/band_bar.hpp"
#include "drones/crystal_match.hpp"
#include "drones/drone_tracks_panel.hpp"
#include "drones/evidence_remote_id.hpp"
#include "drones/universal_claim.hpp"
#include "drones/panel.hpp"
#include "gui/app_shell.hpp"
#include "imgui.h"
#include "shortrange/panel.hpp"

namespace rfmon::drones_page {
namespace dc = rfmon::drone_claims;

// ---------------------------------------------------------------- pure helpers

std::vector<std::string> evidence_kinds(const Detection& row) {
    std::vector<std::string> out;
    if (row.timing) out.push_back("Link");
    if (row.packet) out.push_back("DroneID");
    if (!row.remote_id.empty()) out.push_back("Remote ID");
    if (row.classifier_hint || (row.universal && (row.universal->accepted || row.universal->classifier_only)))
        out.push_back("Classifier");
    return out;
}

std::map<int, std::size_t> tier_counts(const std::map<std::string, Detection>& rows) {
    std::map<int, std::size_t> out;
    for (const auto& [id, row] : rows) ++out[int(dc::verdict(row).tier)];
    return out;
}

std::vector<const Detection*> filter_rows(const std::map<std::string, Detection>& rows, int tier_filter,
                                          Clock::time_point now) {
    struct Item { const Detection* row; int tier; Clock::time_point seen; };
    std::vector<Item> items;
    for (const auto& [id, row] : rows) {
        const int tier = int(dc::verdict(row).tier);
        if (tier_filter >= 0 && tier != tier_filter) continue;
        items.push_back({&row, tier, dc::row_observed(row, now)});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.tier != b.tier) return a.tier < b.tier;  // Tier order is strongest first
        return a.seen > b.seen;
    });
    std::vector<const Detection*> out;
    for (const auto& i : items) out.push_back(i.row);
    return out;
}

std::vector<Package> find_packages(const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<Package> out;
    std::error_code ec;
    for (fs::directory_iterator day(root, fs::directory_options::follow_directory_symlink, ec), end; !ec && day != end;
         day.increment(ec)) {
        const auto dir = day->path() / "universal_package";
        std::error_code ec2;
        if (!fs::is_directory(dir, ec2)) continue;
        for (fs::directory_iterator f(dir, ec2), fend; !ec2 && f != fend; f.increment(ec2)) {
            const auto name = f->path().filename().string();
            if (f->path().extension() != ".pt" || name.rfind("universal_v", 0) != 0) continue;
            int version = 0;
            if (std::sscanf(name.c_str(), "universal_v%d", &version) != 1) continue;
            Package p;
            p.path = f->path().string();
            p.version = version;
            p.label = f->path().stem().string();
            std::error_code ec3;
            const auto written = fs::last_write_time(f->path(), ec3);
            if (!ec3) {
                const auto wall = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                const auto tt = std::chrono::system_clock::to_time_t(wall);
                std::tm parts{};
                char date[16] = "";
                if (localtime_r(&tt, &parts) && std::strftime(date, sizeof date, "%d %b", &parts))
                    p.label += std::string("  (") + date + ")";
            }
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const Package& a, const Package& b) {
        return a.version != b.version ? a.version > b.version : a.path < b.path;
    });
    return out;
}

namespace {
struct UnionFind {
    std::vector<std::size_t> parent;
    explicit UnionFind(std::size_t n) : parent(n) { for (std::size_t i = 0; i < n; ++i) parent[i] = i; }
    std::size_t find(std::size_t i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; }
    void join(std::size_t a, std::size_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
};
std::vector<std::string> serials(const Detection& row) {
    std::vector<std::string> out;
    if (row.packet && row.packet->readable && !row.packet->serial.empty()) out.push_back(row.packet->serial);
    return out;
}
double packet_hz(const Detection& row) {
    return row.packet->frequency > 0 ? row.packet->frequency : row.packet_key.center;
}
// A readable DroneID row whose model's link type (model table) cannot show the link-only row's
// timing group, for example a Mini 2 (15 kHz OcuSync) and a Lightbridge link. "" otherwise.
std::string model_rules_out(const Detection& packet_row, const Detection& link_row) {
    if (!packet_row.packet || !packet_row.packet->readable || link_row.packet || link_row.group.empty()) return "";
    const auto* model = dc::model_link(packet_row.packet->model);
    if (!model || !model->group) return "";
    if (dc::compatible(model->group, link_row.group) || (model->also && dc::compatible(model->also, link_row.group))) return "";
    return packet_row.packet->model + " uses " + model->link + " (" + dc::group_words(model->group) +
           "); this link shows " + dc::group_words(link_row.group);
}
bool lightbridge_link_only(const Detection& row) {
    return row.timing && row.group == "32.8kHz" && !row.packet && row.remote_id.empty();
}
}  // namespace

Grouping group_rows(const std::vector<const Detection*>& rows) {
    using Strength = Relation::Strength;
    const std::size_t n = rows.size();
    UnionFind sure(n), any(n);
    Grouping g;
    const auto seconds = [](Clock::time_point a, Clock::time_point b) {
        return std::abs(std::chrono::duration<double>(a - b).count());
    };
    // Joins are applied after every pair is judged: same serial first, then likely, then possible
    // (for the fewest-drones count), and a likely or possible join that would put a "probably not"
    // pair into one drone is skipped. Without that, a row with no clock reading chained two
    // Lightbridge aircraft whose clocks differ into one drone.
    // Likely joins from time alone (Lightbridge rows without agreeing clocks) go after measured
    // ones, so a clockless row joins the drone whose clocks were established first.
    struct Pending { std::size_t i, j; Strength s; bool time_only; };
    std::vector<Pending> pending;
    std::vector<std::pair<std::size_t, std::size_t>> apart;
    const auto relate = [&](std::size_t i, std::size_t j, Strength s, const std::string& why, bool time_only = false) {
        g.related[rows[i]->id].push_back({rows[j]->id, why, s});
        g.related[rows[j]->id].push_back({rows[i]->id, why, s});
        if (s == Strength::Different) apart.emplace_back(i, j);
        else pending.push_back({i, j, s, time_only});
    };
    char b[160];
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            const auto& a = *rows[i];
            const auto& c = *rows[j];
            // Same serial number: DroneID serials, or a Remote ID serial that matches one.
            bool same_serial = false;
            for (const auto& s : serials(a))
                for (const auto& t : serials(c)) same_serial = same_serial || s == t;
            for (const auto& [x, y] : {std::pair{&a, &c}, std::pair{&c, &a}})
                for (const auto& s : serials(*x))
                    for (const auto& rid : y->remote_id)
                        same_serial = same_serial || dc::remote_id_serial_matches(rid.reported_id, s);
            if (same_serial) { relate(i, j, Strength::Certain, "same serial number"); continue; }
            // A decoded model that cannot use this link type.
            if (auto why = model_rules_out(a, c); !why.empty()) { relate(i, j, Strength::Different, why); continue; }
            if (auto why = model_rules_out(c, a); !why.empty()) { relate(i, j, Strength::Different, why); continue; }
            // The crystals: clearly different overrides the rules below; a fit adds to them.
            const auto crystal = dc::crystal_match(a, c);
            if (crystal.kind == dc::CrystalMatch::Different) { relate(i, j, Strength::Different, crystal.reason); continue; }
            // DroneID on the same channel at nearly the same time.
            if (a.packet && c.packet && std::abs(packet_hz(a) - packet_hz(c)) <= 1e6) {
                const double dt = seconds(a.packet->observed, c.packet->observed);
                if (dt <= 10) {
                    std::snprintf(b, sizeof b, "DroneID on the same channel (%.1f MHz), %.0f s apart",
                                  packet_hz(a) / 1e6, dt);
                    relate(i, j, Strength::Likely,
                           crystal.kind == dc::CrystalMatch::None ? std::string(b) : std::string(b) + "; " + crystal.reason);
                    continue;
                }
            }
            if (crystal.kind == dc::CrystalMatch::Likely) { relate(i, j, Strength::Likely, crystal.reason); continue; }
            // Two link-only Lightbridge rows go on to the time rule below even when their crystals
            // only say "possibly": the time rule already counts them as one aircraft when nothing
            // was measured, so measuring agreement must not keep them apart. Live 23 Nov 2026: the
            // Phantom's 5.8 and 2.4 GHz rows (24 and 17 readings, -4.37 and -4.61 ppm, "possibly")
            // stayed two drones while rows with no readings would have joined.
            // (Not a "possibly" from frame timing that only roughly lines up: that is capped on purpose.)
            const bool frame_capped = crystal.reason.rfind("frame timing", 0) == 0;
            if (crystal.kind == dc::CrystalMatch::Possible &&
                (frame_capped || !(lightbridge_link_only(a) && lightbridge_link_only(c)))) {
                relate(i, j, Strength::Possible, crystal.reason);
                continue;
            }
            // Two link-only Lightbridge rows heard within a row's lifetime: counted as one aircraft
            // unless their link clocks differ (handled above). Lightbridge carries no DroneID.
            if (lightbridge_link_only(a) && lightbridge_link_only(c)) {
                const double dt = seconds(a.timing->observed, c.timing->observed);
                if (dt <= std::chrono::duration<double>(dc::lifetime).count()) {
                    std::snprintf(b, sizeof b, "both Lightbridge links, heard %.0f s apart; Lightbridge rows are "
                                  "counted as one aircraft unless their link clocks differ", dt);
                    std::string why = b;
                    const auto clocks = dc::compare_rows_link_clock(a, c);
                    if (!clocks.reason.empty()) why += "; " + clocks.reason;
                    relate(i, j, Strength::Likely, why, clocks.reason.find("agree") == std::string::npos);
                    continue;
                }
            }
            // DroneID heard near a measured link at the same time.
            for (const auto& [l, p] : {std::pair{&a, &c}, std::pair{&c, &a}}) {
                if (!l->timing || !p->packet || p->timing) continue;
                const double hz = packet_hz(*p);
                if (hz < l->link_low_hz - dc::link_join_hz || hz > l->link_high_hz + dc::link_join_hz) continue;
                if (seconds(l->timing->observed, p->packet->observed) > std::chrono::duration<double>(dc::lifetime).count()) continue;
                std::snprintf(b, sizeof b, "DroneID at %.1f MHz, within 20 MHz of this link and seen at the same time", hz / 1e6);
                relate(i, j, Strength::Possible, b);
                break;
            }
        }
    const auto allowed = [&](UnionFind& u, std::size_t i, std::size_t j) {
        const auto ri = u.find(i), rj = u.find(j);
        for (const auto& [x, y] : apart) {
            const auto rx = u.find(x), ry = u.find(y);
            if ((rx == ri && ry == rj) || (rx == rj && ry == ri)) return false;
        }
        return true;
    };
    for (const auto& p : pending) if (p.s == Strength::Certain) sure.join(p.i, p.j);
    for (const bool time_only : {false, true})
        for (const auto& p : pending)
            if (p.s == Strength::Likely && p.time_only == time_only && allowed(sure, p.i, p.j)) sure.join(p.i, p.j);
    // The fewest-drones count starts from the drone groups and adds possible links.
    for (std::size_t i = 0; i < n; ++i) any.join(i, sure.find(i));
    for (const auto& p : pending)
        if (p.s == Strength::Possible && allowed(any, p.i, p.j)) any.join(p.i, p.j);
    std::map<std::size_t, int> number;
    std::set<std::size_t> fewest;
    for (std::size_t i = 0; i < n; ++i) {
        const auto root = sure.find(i);
        if (!number.count(root)) number[root] = int(number.size()) + 1;
        g.drone[rows[i]->id] = number[root];
        fewest.insert(any.find(i));
    }
    g.drones = number.size();
    g.fewest = fewest.size();
    return g;
}

std::string droneid_offset_text(const Detection& row) {
    const auto s = dc::summarize_offsets(row.droneid_offsets);
    if (!s) return row.packet ? "not measured (packet not on a listed DroneID channel)" : "";
    char b[320];
    int n = std::snprintf(b, sizeof b, "%+.3f ppm (latest %+.2f kHz at %.1f MHz)", s->all.mean_ppm,
                          s->latest_hz / 1e3, s->latest_channel_hz / 1e6);
    if (s->all.count > 1)
        n += std::snprintf(b + n, sizeof b - n, "; %zu packets, spread %.3f ppm", s->all.count, s->all.spread_ppm);
    else
        n += std::snprintf(b + n, sizeof b - n, "; 1 packet");
    if (s->band_difference_ppm)
        std::snprintf(b + n, sizeof b - n, "\n2.4 GHz %+.3f ppm (%zu) · 5 GHz %+.3f ppm (%zu); difference %+.3f ppm",
                      s->low.mean_ppm, s->low.count, s->high.mean_ppm, s->high.count, *s->band_difference_ppm);
    return b;
}

std::string link_grid_text(const Detection& row) {
    if (!row.timing) return "";
    const auto g = rfmon::symbol_timing::link_grid(*row.timing);
    if (!g) return "";
    char b[160];
    std::snprintf(b, sizeof b, "%.0f Hz modulo %.1f kHz (coherence %.2f, %zu blocks)", g->hz, g->spacing_hz / 1e3,
                  g->coherence, g->blocks);
    return b;
}

std::string link_clock_text(const Detection& row) {
    char b[220];
    if (const auto f = dc::frame_fit(row)) {
        std::snprintf(b, sizeof b, "%+.2f +- %.2f ppm from frame timing over %zu captures and %.0f s (%.1f us rms; "
                      "against this receiver's clock)", f->ppm, f->sigma_ppm, f->connected, f->span_s, f->rms_s * 1e6);
        return b;
    }
    const auto m = dc::link_clock_mean(row.link_clocks);
    if (!m) return "";
    std::snprintf(b, sizeof b, "%+.2f +- %.2f ppm from %zu capture%s (%s symbol timing, against this receiver's clock)",
                  m->ppm, m->sigma_ppm, m->captures, m->captures == 1 ? "" : "s", dc::link_kind(row.link_clocks));
    return b;
}

std::string bands_text(const std::vector<const Detection*>& rows) {
    std::set<int> bands;  // MHz of the band names below, sorted
    for (const auto* row : rows) {
        double hz = row->timing ? (row->link_low_hz + row->link_high_hz) / 2 : 0;
        if (row->packet) hz = packet_hz(*row);
        if (!(hz > 0) && !row->remote_id.empty()) hz = row->remote_id.front().frequency;
        if (!(hz > 0)) {
            if (row->band.rfind("2.4", 0) == 0) bands.insert(2400);
            else if (row->band.rfind("5", 0) == 0) bands.insert(5000);
            continue;
        }
        bands.insert(hz < 3e9 ? 2400 : hz < 5.47e9 ? 5200 : hz < 5.725e9 ? 5500 : 5800);
    }
    std::string out;
    for (int b : bands) {
        if (!out.empty()) out += " + ";
        out += b == 2400 ? "2.4" : b == 5200 ? "5.2" : b == 5500 ? "5.5" : b == 5800 ? "5.8" : "5";
    }
    return out.empty() ? "" : out + " GHz";
}

std::vector<DroneLine> drone_lines(const std::vector<const Detection*>& ordered, const Grouping& g, Clock::time_point now) {
    std::vector<DroneLine> lines;
    std::map<int, std::size_t> at;
    for (const auto* row : ordered) {
        const auto d = g.drone.find(row->id);
        const int drone = d == g.drone.end() ? 0 : d->second;
        std::size_t i;
        if (drone && at.count(drone)) i = at[drone];
        else {
            i = lines.size();
            lines.push_back({drone, {}, {}, {}, {}});
            if (drone) at[drone] = i;
        }
        auto& line = lines[i];
        line.rows.push_back(row);
        const auto seen = dc::row_observed(*row, now), first = dc::row_first_observed(*row, now);
        line.last = line.rows.size() == 1 ? seen : std::max(line.last, seen);
        line.first = line.rows.size() == 1 ? first : std::min(line.first, first);
    }
    for (auto& line : lines) line.bands = bands_text(line.rows);
    return lines;
}

std::string row_name(const Detection& row, const Detection* lead, const Grouping& g) {
    const auto own = dc::verdict(row);
    if (!lead || lead == &row) return dc::model_and_link(own);
    const auto best = dc::verdict(*lead);
    if (int(best.tier) >= int(own.tier)) return dc::model_and_link(own);
    const char* what = row.packet ? "DroneID" : row.timing ? "link" : !row.remote_id.empty() ? "Remote ID" : "row";
    const auto d = g.drone.find(row.id);
    return dc::model_and_link(best) + " (" + what + (d == g.drone.end() ? "" : " of D" + std::to_string(d->second)) + ")";
}

bool alert_for_known_drone(const dc::TrackEvent& event, const std::map<std::string, Detection>& rows,
                           const Grouping& g, Clock::time_point now) {
    if (event.event != "opened") return false;
    const auto id = row_for_track(rows, event.track);
    const auto d = g.drone.find(id);
    if (id.empty() || d == g.drone.end()) return false;
    for (const auto& [other, drone] : g.drone) {
        if (drone != d->second || other == id) continue;
        const auto row = rows.find(other);
        if (row != rows.end() && dc::row_first_observed(row->second, now) < event.track.first_seen) return true;
    }
    return false;
}

std::string alert_key(const dc::TrackEvent& event) {
    return std::to_string(event.track.id) + "|" + event.event + "|" + event.alert;
}

std::string row_for_track(const std::map<std::string, Detection>& rows, const dc::DroneTrack& track) {
    for (const auto& [id, row] : rows) {
        if (id == track.key || (!row.link_id.empty() && row.link_id == track.key)) return id;
        if (row.packet && !row.packet->serial.empty() && row.packet->serial == track.serial) return id;
        for (const auto& rid : row.remote_id)
            if (!rid.reported_id.empty() && track.remote_ids.count(rid.reported_id)) return id;
    }
    return "";
}

namespace {
// ---------------------------------------------------------------- drawing helpers

const ImVec4 kOk{.30f, .75f, .54f, 1}, kWarn{.95f, .68f, .30f, 1}, kBad{.93f, .42f, .37f, 1},
    kMuted{.55f, .62f, .69f, 1}, kAccent{.31f, .70f, .87f, 1};

float ui_scale() { return std::max(1.f, ImGui::GetFontSize() / 16.f); }

ImVec4 tier_colour(dc::Tier t) {
    switch (t) {
        case dc::Tier::Confirmed: return {.30f, .75f, .54f, 1};
        case dc::Tier::Likely: return {.31f, .70f, .87f, 1};
        case dc::Tier::Family: return {.56f, .61f, .96f, 1};
        case dc::Tier::Reported: return {.90f, .70f, .31f, 1};
        case dc::Tier::Classifier: return {.75f, .54f, .88f, 1};
        default: return {.58f, .63f, .67f, 1};
    }
}
const char* tier_help(dc::Tier t) {
    switch (t) {
        case dc::Tier::Confirmed: return "A readable DJI DroneID packet names the model. Self-reported by the aircraft, not authenticated.";
        case dc::Tier::Likely: return "DroneID is present but encrypted. Only models seen to encrypt it, whose link shows this timing group, remain.";
        case dc::Tier::Family: return "Only the video link was measured. Its timing group gives the family and the models measured in it.";
        case dc::Tier::Reported: return "Only a Remote ID broadcast: an ID and aircraft type, no model.";
        case dc::Tier::Classifier: return "Only the universal classifier saw it. Weakest level; in development and hidden by default.";
        default: return "DroneID was read, but its product code is not in the table.";
    }
}

// A small framed label drawn at the cursor. Returns true while hovered.
bool badge(const char* text, ImVec4 colour, float fill = .16f) {
    const ImVec2 pad{7, 2};
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size{ts.x + 2 * pad.x, ts.y + 2 * pad.y};
    ImGui::Dummy(size);
    auto* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = colour, edge = colour;
    bg.w = fill;
    edge.w = .55f;
    dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, ImGui::GetColorU32(bg), 4);
    dl->AddRect(p, {p.x + size.x, p.y + size.y}, ImGui::GetColorU32(edge), 4);
    dl->AddText({p.x + pad.x, p.y + pad.y}, ImGui::GetColorU32(colour), text);
    return ImGui::IsItemHovered();
}
void tier_badge(dc::Tier t) {
    if (badge(dc::tier_name(t), tier_colour(t))) ImGui::SetTooltip("%s", tier_help(t));
}
void dot(ImVec4 colour) {
    const float r = ImGui::GetTextLineHeight() * .22f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({r * 2 + 2, ImGui::GetTextLineHeight()});
    ImGui::GetWindowDrawList()->AddCircleFilled({p.x + r + 1, p.y + ImGui::GetTextLineHeight() * .5f}, r,
                                                ImGui::GetColorU32(colour));
}
// SameLine when `width` more pixels fit on this line; otherwise start a new line.
void next_on_line(float width, float gap = 0) {
    const float spacing = gap > 0 ? gap : ImGui::GetStyle().ItemSpacing.x;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + spacing + width <= right) ImGui::SameLine(0, spacing);
}
float text_w(const char* s) { return ImGui::CalcTextSize(s).x; }
float button_w(const char* s) { return text_w(s) + ImGui::GetStyle().FramePadding.x * 2; }

double seconds_since(Clock::time_point t, Clock::time_point now) {
    return std::max(0.0, std::chrono::duration<double>(now - t).count());
}
std::string ago(double s) {
    char b[48];
    if (s < 60) std::snprintf(b, sizeof b, "%.0f s ago", s);
    else if (s < 3600) std::snprintf(b, sizeof b, "%.0f min ago", std::floor(s / 60));
    else std::snprintf(b, sizeof b, "%.1f h ago", s / 3600);
    return b;
}
std::string wall_time(Clock::time_point t, Clock::time_point now) {
    const auto wall = std::chrono::system_clock::now() -
                      std::chrono::duration_cast<std::chrono::system_clock::duration>(now - t);
    const auto tt = std::chrono::system_clock::to_time_t(wall);
    std::tm parts{};
    char b[16] = "";
    if (localtime_r(&tt, &parts)) std::strftime(b, sizeof b, "%H:%M:%S", &parts);
    return b;
}
std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : sep) + s;
    return out;
}
std::string centre_text(const Detection& row) {
    char b[64] = "";
    if (row.timing) {
        if (row.link_low_hz == row.link_high_hz) std::snprintf(b, sizeof b, "%.1f MHz", row.link_low_hz / 1e6);
        else std::snprintf(b, sizeof b, "%.1f-%.1f MHz", row.link_low_hz / 1e6, row.link_high_hz / 1e6);
    } else if (row.universal && row.universal->classifier_only) {
        std::snprintf(b, sizeof b, "%.0f MHz slot", row.link_low_hz / 1e6);
    } else if (row.packet && row.packet->frequency > 0) {
        std::snprintf(b, sizeof b, "%.1f MHz", row.packet->frequency / 1e6);
    } else if (!row.remote_id.empty() && row.remote_id.front().frequency > 0) {
        std::snprintf(b, sizeof b, "%.1f MHz", row.remote_id.front().frequency / 1e6);
    }
    return b;
}
double centre_hz(const Detection& row) {
    if (row.timing || (row.universal && row.universal->classifier_only)) return row.link_low_hz;
    if (row.packet) return row.packet->frequency;
    if (!row.remote_id.empty()) return row.remote_id.front().frequency;
    return 0;
}
std::string identity_line(const Detection& row) {
    if (row.packet && row.packet->readable && !row.packet->serial.empty()) return "Serial " + row.packet->serial;
    if (!row.remote_id.empty() && !row.remote_id.front().reported_id.empty()) return "ID " + row.remote_id.front().reported_id;
    return dc::display_identity(row);
}
std::string identity_note(const Detection& row) {
    if (row.packet && row.packet->readable && !row.packet->serial.empty()) return "self-reported";
    if (!row.remote_id.empty() && !row.remote_id.front().reported_id.empty()) return "Remote ID, self-reported";
    return "";
}

// Styled push/pop for a filled (primary) or outlined (stop) button.
void push_button(ImVec4 colour, bool filled) {
    ImVec4 base = colour, hover = colour, active = colour;
    if (filled) { hover.w = .85f; active.w = .7f; }
    else { base.w = .0f; hover.w = .18f; active.w = .3f; }
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, filled ? ImVec4(.04f, .08f, .11f, 1) : colour);
    ImGui::PushStyleColor(ImGuiCol_Border, colour);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
}
void pop_button() { ImGui::PopStyleVar(); ImGui::PopStyleColor(5); }

// A pill-shaped toggle. Returns true when clicked.
bool chip(const char* label, bool on, ImVec4 colour = kAccent) {
    ImVec4 bg = on ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0, 0, 0, 0);
    ImVec4 border = on ? colour : ImGui::GetStyleColorVec4(ImGuiCol_Border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * ui_scale(), 3.f * ui_scale()});
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kMuted);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    return clicked;
}

// ---------------------------------------------------------------- status strip

void draw_status(coordinated::PanelState& cp, Scanner& scanner, drone::Addon& drones, ml::Inference& model,
                 const Inputs& in, const Actions& act) {
    const bool coordinated_mode = scanner.active_band() == BAND_COORDINATED;
    const auto st = scanner.coordinated_status();
    const double now_s = Scanner::now_steady_s();

    dot(in.connected ? kOk : in.started ? kWarn : kBad);
    ImGui::SameLine(0, 6);
    ImGui::Text("%s  %s", in.radio.empty() ? "Receiver" : in.radio.c_str(),
                in.connected ? "connected" : in.started ? "starting" : "not connected");

    std::string mode = coordinated_mode ? "Detecting, coordinated"
                       : in.started     ? "Receiving " + gui::band_label(scanner.active_band()) + " only"
                                        : "Stopped";
    next_on_line(text_w(mode.c_str()) + 20, 22);
    dot(coordinated_mode ? kOk : kMuted);
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(mode.c_str());
    if (!coordinated_mode && in.started && ImGui::IsItemHovered())
        ImGui::SetTooltip("Only this band is being received. Start detection visits Sub-GHz, 2.4 GHz and 5 GHz in turn.");

    if (coordinated_mode) {
        const std::string current = st.current ? st.current->band : std::string();
        for (const auto& slot : st.slots) {
            if (!slot.enabled) continue;
            const char* name = slot.band == BAND_SUB_GHZ ? "Sub-GHz" : slot.band == BAND_WIFI_2G4 ? "2.4 GHz" : "5 GHz";
            char label[64];
            const bool now_here = slot.band == current;
            const double age = slot.last_visit_end_s > 0 ? now_s - slot.last_visit_end_s : -1;
            if (now_here) std::snprintf(label, sizeof label, "%s  now", name);
            else if (age < 0) std::snprintf(label, sizeof label, "%s  not yet", name);
            else std::snprintf(label, sizeof label, "%s  %.0f s", name, age);
            next_on_line(text_w(label) + 14, 8);
            // A band not visited for three configured sweeps is late (overloaded or failing).
            double sweep = 0;
            for (const auto& se : st.estimate.slots) sweep = std::max(sweep, se.sweep_s);
            const bool late = age > std::max(60.0, 3 * sweep);
            if (badge(label, now_here ? kAccent : late ? kWarn : kMuted, now_here ? .18f : .06f))
                ImGui::SetTooltip("%s: %zu centres, last visit %s", name, slot.list_size,
                                  age < 0 ? "not yet" : ago(age).c_str());
        }
        char cycle[48];
        std::snprintf(cycle, sizeof cycle, "Cycle %llu", static_cast<unsigned long long>(st.cycle));
        next_on_line(text_w(cycle), 12);
        ImGui::TextDisabled("%s", cycle);
    }

    const char* dec = drones.unified ? "Decoders on" : "Decoders off";
    next_on_line(text_w(dec) + 20, 22);
    dot(drones.unified ? kOk : kWarn);
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(dec);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("DJI DroneID: %s | older DJI decoder: %s | Wi-Fi Remote ID: %s\nTurn them on or off in the Diagnostics tab.",
                          drones.native.running() ? "on" : "off", drones.live.running() ? "on" : "off",
                          drones.remote_id.enabled() ? "on" : "off");

    const auto ms = model.snapshot();
    const bool failed = ms.state == "Failed";
    const char* clf = failed ? "Classifier failed" : ms.ready ? "Classifier ready" : ms.enabled ? "Classifier starting" : "Classifier off";
    next_on_line(text_w(clf) + 20, 22);
    dot(failed ? kBad : ms.ready ? kOk : ms.enabled ? kWarn : kMuted);
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(clf);
    if (ImGui::IsItemHovered()) {
        std::string tip = ms.state;
        if (in.classifier_package)
            tip += " | package " + std::filesystem::path(*in.classifier_package).stem().string();
        if (!ms.model_hash.empty()) tip += " | model " + ms.model_hash.substr(0, 12);
        if (!ms.error.empty()) tip += " | " + ms.error;
        ImGui::SetTooltip("%s", tip.c_str());
    }

    if (in.recorder && in.recorder->recording()) {
        const auto rs = in.recorder->status();
        char rec[64];
        std::snprintf(rec, sizeof rec, "Recording labelled captures: %zu", rs.saved);
        next_on_line(text_w(rec) + 20, 22);
        dot(kBad);
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(rec);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Training recorder: %s (Diagnostics tab)", rs.dir.c_str());
    }

    const char* label = !in.started ? "Connect receiver" : coordinated_mode ? "Stop detection" : "Start detection";
    const float w = button_w(label);
    next_on_line(w, 22);
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    if (!in.started) {
        push_button(kAccent, true);
        if (ImGui::Button(label) && act.open_setup) act.open_setup();
        pop_button();
    } else if (coordinated_mode) {
        push_button(kBad, false);
        if (ImGui::Button(label)) coordinated::stop_detection(cp, scanner, drones);
        pop_button();
    } else {
        push_button(kAccent, true);
        if (ImGui::Button(label)) coordinated::start_detection(cp, scanner, drones);
        pop_button();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Visit Sub-GHz, 2.4 GHz and 5 GHz in turn with every drone decoder and the classifier.");
    }

    coordinated::draw_schedule_problems(scanner);
    if (in.started && !drones.unified)
        gui::note("Drone decoders are off: DroneID and Remote ID are not read and the classifier does not run. "
                  "Turn them on in the Diagnostics tab.", true);
    else if (in.started && failed)
        gui::note(("The classifier failed to start: " + (ms.error.empty() ? ms.state : ms.error) +
                   ". Rows still use link timing, DroneID and Remote ID.").c_str(), true);
    if (coordinated_mode && (st.overflows || st.empty_captures))
        gui::note(("Receiver overflows in " + std::to_string(st.overflows) + " visit(s), empty captures " +
                   std::to_string(st.empty_captures) + ": those visits are not valid coverage (Coverage tab).").c_str(), true);
    for (const auto& p : in.problems) gui::note(p.c_str(), true);
}

// ---------------------------------------------------------------- alert banner

void draw_alert(State& state, const Inputs& in, const Grouping& g) {
    if (!in.tracks || in.tracks->alerts().empty()) return;
    const auto& event = in.tracks->alerts().front();
    const auto key = alert_key(event);
    if (key == state.dismissed_alert || !event.track.present) return;
    // A row that joined a drone already on the page is that drone's line updating, not a new drone.
    if (in.rows && alert_for_known_drone(event, *in.rows, g, in.now)) return;
    const auto colour = event.lower_severity ? kMuted : tier_colour(event.track.best.tier);
    ImVec4 bg = colour, edge = colour;
    bg.w = .10f;
    edge.w = .55f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, edge);
    if (ImGui::BeginChild("##drone alert", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        tier_badge(event.track.best.tier);
        ImGui::SameLine();
        ImGui::TextWrapped("%s%s  (%s)", event.lower_severity ? "Advisory: " : "", event.alert.c_str(),
                           wall_time(event.track.last_seen, in.now).c_str());
        const float w = button_w("Show") + button_w("Dismiss") + ImGui::GetStyle().ItemSpacing.x;
        next_on_line(w);
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
        if (ImGui::SmallButton("Show")) {
            const auto id = in.rows ? row_for_track(*in.rows, event.track) : std::string();
            if (!id.empty()) { state.selected = id; state.tier_filter = -1; }
            else state.select_tab = TabHistory;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) state.dismissed_alert = key;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------- details panel

void draw_positions(const Detection& row, Clock::time_point now) {
    struct Point { const char* role; const dc::ReportedPosition* fix; };
    std::vector<Point> pts;
    const auto add = [&](const char* role, const std::optional<dc::ReportedPosition>& fix) {
        if (fix) pts.push_back({role, &*fix});
    };
    if (row.packet) {
        add("Aircraft", row.packet->positions.aircraft);
        add("Pilot", row.packet->positions.pilot);
        add("Home", row.packet->positions.home);
    }
    for (const auto& rid : row.remote_id) {
        add("Aircraft", rid.positions.aircraft);
        add("Operator", rid.positions.operator_location);
        break;  // one broadcast is enough for the sketch
    }
    if (pts.empty()) return;
    ImGui::SeparatorText("Reported positions");
    // Local sketch in metres around the pilot / operator (or the first fix).
    const dc::ReportedPosition* origin = pts.front().fix;
    for (const auto& p : pts)
        if (std::string(p.role) == "Pilot" || std::string(p.role) == "Operator") { origin = p.fix; break; }
    const double k = 3.14159265358979 / 180, cos0 = std::cos(origin->latitude * k);
    double reach = 1;
    std::vector<ImVec2> metres;
    for (const auto& p : pts) {
        const double x = (p.fix->longitude - origin->longitude) * 111320 * cos0;
        const double y = (p.fix->latitude - origin->latitude) * 110540;
        metres.push_back({float(x), float(y)});
        reach = std::max(reach, std::hypot(x, y));
    }
    double ring = 10;
    for (double step : {10., 20., 50., 100., 200., 500., 1000., 2000., 5000., 10000., 20000.}) {
        ring = step;
        if (reach <= 2 * step) break;
    }
    const float size = 150 * ui_scale();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy({size, size});
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 c{p0.x + size / 2, p0.y + size / 2};
    const float px_per_m = float((size / 2 - 12) / (2 * ring));
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddRectFilled(p0, {p0.x + size, p0.y + size}, ImGui::GetColorU32(ImGuiCol_FrameBg), 6);
    dl->AddCircle(c, float(ring) * px_per_m, line);
    dl->AddCircle(c, float(2 * ring) * px_per_m, line);
    dl->AddLine({c.x, p0.y + 6}, {c.x, p0.y + size - 6}, line);
    dl->AddLine({p0.x + 6, c.y}, {p0.x + size - 6, c.y}, line);
    dl->AddText({c.x + 4, p0.y + 4}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "N");
    for (size_t i = 0; i < pts.size(); ++i) {
        const ImVec2 q{c.x + metres[i].x * px_per_m, c.y - metres[i].y * px_per_m};
        const bool aircraft = std::string(pts[i].role) == "Aircraft";
        dl->AddCircleFilled(q, 4.5f * ui_scale(), ImGui::GetColorU32(aircraft ? kAccent : ImVec4(.85f, .89f, .93f, 1)));
        dl->AddText({q.x + 7, q.y - 7}, ImGui::GetColorU32(ImGuiCol_TextDisabled), pts[i].role);
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("Rings %.0f m and %.0f m", ring, 2 * ring);
    for (const auto& p : pts) {
        ImGui::Text("%s  %.6f, %.6f", p.role, p.fix->latitude, p.fix->longitude);
        std::string extra = ago(seconds_since(p.fix->observed, now));
        char b[64];
        if (p.fix->height_m) { std::snprintf(b, sizeof b, ", height %.0f m", *p.fix->height_m); extra += b; }
        if (p.fix->speed_m_s) { std::snprintf(b, sizeof b, ", %.1f m/s", *p.fix->speed_m_s); extra += b; }
        ImGui::TextDisabled("  %s", extra.c_str());
    }
    ImGui::EndGroup();
    gui::note("Self-reported by the aircraft, not authenticated.");
}

// Ranked models as score bars.
void score_bars(const std::vector<dc::UniversalModelScore>& list, ImVec4 colour) {
    for (const auto& m : list) {
        ImGui::TextUnformatted(m.name.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * .55f);
        char overlay[16];
        std::snprintf(overlay, sizeof overlay, "%.2f", m.score);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
        ImGui::ProgressBar(float(std::clamp(m.score, 0.0, 1.0)), {-1, ImGui::GetTextLineHeight()}, overlay);
        ImGui::PopStyleColor();
    }
}
std::string without_dji(const std::string& s) { return s.rfind("DJI ", 0) == 0 ? s.substr(4) : s; }

// Why the classifier's ranked models were not named (display only).
void not_named_note(const dc::UniversalClaim& u) {
    char b[320];
    const auto distance = [&] {
        char d[32];
        if (u.last_distance) std::snprintf(d, sizeof d, "%.1f", *u.last_distance);
        else std::snprintf(d, sizeof d, "very far");
        return std::string(d);
    };
    if (!u.score_limit)
        std::snprintf(b, sizeof b, "Not named: the package sets no naming limits for this signal type.");
    else if (u.below_score && !u.beyond_distance)
        std::snprintf(b, sizeof b, "Not named: the top score was below the naming limit in %zu of %zu captures "
                      "(latest %.2f, limit %.2f).", u.below_score, u.unnamed, u.last_score.value_or(0), *u.score_limit);
    else if (u.beyond_distance && !u.below_score)
        std::snprintf(b, sizeof b, "Not named: in %zu of %zu captures the signal was too far from that model's training "
                      "recordings (latest distance %s, limit %.2f). Expected for a unit or receiver it was not trained on.",
                      u.beyond_distance, u.unnamed, distance().c_str(), u.distance_limit.value_or(0));
    else
        std::snprintf(b, sizeof b, "Not named: score below the limit in %zu and too far from the training recordings in %zu "
                      "of %zu captures (latest score %.2f, limit %.2f; distance %s, limit %.2f).", u.below_score,
                      u.beyond_distance, u.unnamed, u.last_score.value_or(0), *u.score_limit, distance().c_str(),
                      u.distance_limit.value_or(0));
    gui::note(b);
}

void draw_classifier_models(const Detection& row, ImVec4 colour, bool& any) {
    if (row.universal && row.universal->accepted && !row.universal->models.empty()) {
        any = true;
        const bool carries = row.universal->naming_carries;
        if (!carries) {
            ImGui::TextDisabled("Classifier's guess (not the answer)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The classifier named these, but its model names have not held on a dataset\n"
                                  "(another receiver) it was not trained on, so the answer above is the link\n"
                                  "family and its candidates. DroneID, when readable, names the model.");
        }
        score_bars(row.universal->models, carries ? colour : kMuted);
        if (!row.universal->accuracy.empty()) gui::note(row.universal->accuracy.c_str());
    } else if (row.universal && !row.universal->guesses.empty()) {
        any = true;
        ImGui::TextDisabled("Classifier's best guesses (not named)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ranked by the classifier, but no capture passed its naming limits,\n"
                              "so these are not used for the answer above.");
        score_bars(row.universal->guesses, kMuted);
        not_named_note(*row.universal);
    }
    if (row.classifier_check && row.packet) {
        const auto& u = *row.classifier_check;
        const bool named = u.accepted && !u.models.empty();
        if (!named && u.guesses.empty()) return;
        any = true;
        ImGui::TextDisabled("Classifier check (DroneID decides this row)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What the classifier says about the same captures. DroneID names this drone,\n"
                              "so the classifier is shown for comparison only.");
        const auto& list = named ? u.models : u.guesses;
        score_bars(list, named ? colour : kMuted);
        if (!named) not_named_note(u);
        const auto top = without_dji(dc::table_name(list.front().name));
        const auto said = without_dji(row.packet->model);
        const bool agrees = top == said || without_dji(list.front().name) == said;
        gui::note(((named ? "It names " : "Its top guess is ") + list.front().name +
                   (agrees ? ", which agrees with DroneID." : "; DroneID says " + row.packet->model + ".")).c_str(),
                  !agrees);
    }
}

void key_value(const char* key, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", key);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

ImVec4 drone_colour(int n) {
    static const ImVec4 palette[] = {{.47f, .78f, .93f, 1}, {.94f, .62f, .45f, 1}, {.62f, .85f, .52f, 1},
                                     {.85f, .62f, .92f, 1}, {.95f, .83f, .45f, 1}, {.55f, .70f, .98f, 1}};
    return palette[(std::max(n, 1) - 1) % 6];
}
const char* strength_words(Relation::Strength s) {
    return s == Relation::Certain ? "Same drone as" : s == Relation::Likely ? "Probably the same drone as"
           : s == Relation::Possible                                        ? "Possibly the same drone as"
                                                                            : "Probably not the same drone as";
}
std::size_t group_size(const Grouping& g, int drone) {
    std::size_t n = 0;
    for (const auto& [id, d] : g.drone) n += d == drone;
    return n;
}
// "D1", "D1 · 2 rows", with "?" when a weaker link to another drone exists.
std::string drone_label(const Grouping& g, const std::string& id) {
    const auto it = g.drone.find(id);
    if (it == g.drone.end()) return "";
    std::string label = "D" + std::to_string(it->second);
    const auto rows = group_size(g, it->second);
    if (rows > 1) label += " · " + std::to_string(rows) + " rows";
    if (const auto r = g.related.find(id); r != g.related.end())
        for (const auto& rel : r->second)
            if (rel.strength == Relation::Possible) { label += " ?"; break; }
    return label;
}
std::string drone_tooltip(const Grouping& g, const std::string& id, const std::map<std::string, Detection>& all) {
    const auto r = g.related.find(id);
    if (r == g.related.end() || r->second.empty()) return "Unique: no other row looks like this drone.";
    std::string out;
    for (const auto& rel : r->second) {
        const auto other = all.find(rel.other);
        if (other == all.end()) continue;
        if (!out.empty()) out += "\n";
        out += std::string(strength_words(rel.strength)) + " D" + std::to_string(g.drone.at(rel.other)) + " (" +
               dc::verdict(other->second).model + "): " + rel.reason;
    }
    return out;
}

void draw_same_drone(State& state, const Detection& row, const Grouping& g, const std::map<std::string, Detection>& all) {
    ImGui::SeparatorText("Same drone?");
    const auto r = g.related.find(row.id);
    if (r == g.related.end() || r->second.empty()) {
        ImGui::TextDisabled("Unique: no other row looks like this drone.");
        return;
    }
    for (const auto& rel : r->second) {
        const auto other = all.find(rel.other);
        if (other == all.end()) continue;
        ImGui::PushID(rel.other.c_str());
        const int d = g.drone.at(rel.other);
        ImGui::TextColored(drone_colour(d), "D%d", d);
        ImGui::SameLine();
        ImGui::TextWrapped("%s %s (%s): %s.", strength_words(rel.strength), dc::verdict(other->second).model.c_str(),
                           identity_line(other->second).c_str(), rel.reason.c_str());
        if (ImGui::SmallButton("Select that row")) { state.selected = rel.other; state.tier_filter = -1; }
        ImGui::PopID();
    }
    gui::note("Rows are kept apart; this only marks rows that probably describe one aircraft.");
}

void draw_details(State& state, coordinated::PanelState& cp, const Detection* row, const Inputs& in, const Actions& act,
                  const Grouping& g) {
    if (!row) {
        ImGui::TextDisabled("Select a drone to see why it got its evidence level, its models and its evidence.");
        return;
    }
    const auto v = dc::verdict(*row);
    tier_badge(v.tier);
    ImGui::TextWrapped("%s", dc::model_and_link(v).c_str());
    gui::note(v.basis.c_str());
    if (!v.detail.empty()) gui::note(v.detail.c_str());
    if (row->conflict) gui::note("Timing and reported identity conflict.", true);
    if (in.rows) draw_same_drone(state, *row, g, *in.rows);

    ImGui::SeparatorText("Models");
    bool any = false;
    draw_classifier_models(*row, tier_colour(v.tier), any);
    if (!row->candidates.empty()) {
        any = true;
        ImGui::TextWrapped("Measured in this link group: %s", row->candidates.c_str());
    }
    if (row->classifier_hint) {
        any = true;
        ImGui::TextWrapped("Classifier hint: %s (%zu of %zu link captures)", row->classifier_hint->model.c_str(),
                           row->classifier_hint->votes, row->classifier_hint->voting_captures);
    }
    if (!row->only_classifier_class.empty()) {
        any = true;
        ImGui::TextWrapped("Only classifier class available: %s", row->only_classifier_class.c_str());
    }
    if (!any) ImGui::TextDisabled("No model evidence beyond the level above.");

    ImGui::SeparatorText("Signal and identity");
    if (ImGui::BeginTable("##facts", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed, 110 * ui_scale());
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        key_value("Band", row->band + (centre_text(*row).empty() ? "" : "  " + centre_text(*row)));
        key_value("Timing group", row->group.empty() ? "not measured" : row->group);
        if (row->universal) {
            const auto& u = *row->universal;
            char b[128];
            std::snprintf(b, sizeof b, "%s; %zu captures, %zu typed, %zu named, %zu disagree",
                          u.type.empty() ? "no type" : u.type.c_str(), u.captures, u.typed, u.named, u.disagreements);
            key_value("Classifier", b);
        }
        key_value("Identity", dc::display_identity(*row));
        if (row->packet && row->packet->readable && !row->packet->serial.empty())
            key_value("Serial", row->packet->serial + " (self-reported)");
        for (const auto& rid : row->remote_id)
            key_value(("Remote ID" + (rid.transport.empty() ? "" : " (" + rid.transport + ")")).c_str(),
                      dc::remote_id_identity(rid));
        if (const auto text = droneid_offset_text(*row); !text.empty()) {
            key_value("DroneID offset", text);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Carrier offset from the DroneID channel, in parts per million (measurement only).\n"
                                  "One drone's crystal gives about the same ppm on 2.4 and 5.8 GHz; a different\n"
                                  "drone usually differs. Values include this receiver's own error, so compare\n"
                                  "them only with each other. Not yet used to group rows.");
        }
        if (const auto text = link_grid_text(*row); !text.empty()) {
            key_value("Link grid", text);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Where the link's subcarriers sit, modulo their spacing (measurement only).\n"
                                  "Steady over time for one link on one channel. Not yet converted to ppm:\n"
                                  "that needs the link's nominal channel, still to be checked.");
        }
        if (const auto text = link_clock_text(*row); !text.empty()) {
            key_value("Link clock", text);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The drone's crystal error from how fast its link's symbols arrive (measurement).\n"
                                  "The same crystal sets the carrier, so this matches the DroneID offset in ppm and\n"
                                  "reads alike on every band. One Lightbridge capture is good to about 1 ppm, an\n"
                                  "OcuSync capture (15 or 30 kHz) to about 0.1-0.2 ppm. A link seen in several\n"
                                  "captures is followed by its frame timing on the receiver clock: about 0.05 ppm.\n"
                                  "Frame timing also tells two drones apart when their crystals read alike.\n"
                                  "Used to tell rows apart or join them.");
        }
        ImGui::EndTable();
    }

    draw_positions(*row, in.now);

    ImGui::SeparatorText("Evidence");
    if (row->timing)
        ImGui::BulletText("Link timing, last %s", ago(seconds_since(row->timing->observed, in.now)).c_str());
    if (row->packet)
        ImGui::BulletText("DroneID %s, last %s",
                          row->packet->encrypted ? "(encrypted)" : row->packet->readable ? "(readable)" : "(unreadable)",
                          ago(seconds_since(row->packet->observed, in.now)).c_str());
    for (const auto& rid : row->remote_id)
        ImGui::BulletText("Remote ID%s, last %s", rid.transport.empty() ? "" : (" over " + rid.transport).c_str(),
                          ago(seconds_since(rid.observed, in.now)).c_str());
    if (row->universal && row->universal->captures)
        ImGui::BulletText("Classifier, %zu captures", row->universal->captures);
    else if (row->universal && row->universal->classifier_only)
        ImGui::BulletText("Classifier only, last %s", ago(seconds_since(row->universal->last_observed, in.now)).c_str());

    if (ImGui::Button("Show evidence")) {
        const double hz = centre_hz(*row);
        if (hz > 0) std::snprintf(cp.entries_filter, sizeof cp.entries_filter, "%.0f", std::floor(hz / 1e6));
        else cp.entries_filter[0] = 0;
        state.select_tab = TabEvidence;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the Evidence tab, filtered to this frequency");
    ImGui::SameLine();
    if (ImGui::Button("Copy details")) {
        std::string text = std::string(dc::tier_name(v.tier)) + ": " + dc::model_and_link(v) + "\n" + v.basis + "\n";
        if (!v.detail.empty()) text += v.detail + "\n";
        text += "Band: " + row->band + " " + centre_text(*row) + "\nIdentity: " + dc::display_identity(*row) +
                "\nEvidence: " + dc::evidence_sources(*row) + "\n";
        if (const auto offset = droneid_offset_text(*row); !offset.empty()) text += "DroneID offset: " + offset + "\n";
        if (const auto grid = link_grid_text(*row); !grid.empty()) text += "Link grid: " + grid + "\n";
        if (const auto clock = link_clock_text(*row); !clock.empty()) text += "Link clock: " + clock + "\n";
        ImGui::SetClipboardText(text.c_str());
    }
    const auto kinds = evidence_kinds(*row);
    if (std::find(kinds.begin(), kinds.end(), "Classifier") != kinds.end() && act.open_model_inference) {
        ImGui::SameLine();
        if (ImGui::Button("Open in Model Inference")) act.open_model_inference();
    }
}

// ---------------------------------------------------------------- Drones now

void draw_empty(const char* title, const char* text) {
    ImGui::Dummy({0, 18 * ui_scale()});
    const float w = ImGui::GetContentRegionAvail().x;
    const float tw = text_w(title);
    if (w > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - tw) / 2);
    ImGui::TextUnformatted(title);
    ImGui::PushTextWrapPos(0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

// One row's line. `lead` is its drone's strongest row (names the drone); `member` indents it
// under its drone's line.
void draw_row_line(State& state, const Detection& row, const Detection* lead, const Inputs& in, const Grouping& g,
                   bool member) {
    const float row_h = ImGui::GetTextLineHeightWithSpacing() * 2;
    ImGui::PushID(row.id.c_str());
    ImGui::TableNextRow(0, row_h);
    ImGui::TableNextColumn();
    const ImVec2 start = ImGui::GetCursorPos();
    const bool selected = state.selected == row.id;
    if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                          {0, row_h}))
        state.selected = row.id;
    ImGui::SetCursorPos(start);
    if (member) ImGui::Indent(14 * ui_scale());
    const auto v = dc::verdict(row);
    tier_badge(v.tier);
    if (member) ImGui::Unindent(14 * ui_scale());
    ImGui::TableNextColumn();
    if (const auto d = g.drone.find(row.id); d != g.drone.end()) {
        ImGui::TextColored(drone_colour(d->second), "%s", member ? ("D" + std::to_string(d->second)).c_str()
                                                                 : drone_label(g, row.id).c_str());
        if (ImGui::IsItemHovered() && in.rows) ImGui::SetTooltip("%s", drone_tooltip(g, row.id, *in.rows).c_str());
    }
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", row_name(row, lead, g).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", v.basis.c_str());
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", identity_line(row).c_str());
    const auto note = identity_note(row);
    if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(row.band.c_str());
    const auto centre = centre_text(row);
    if (!centre.empty()) ImGui::TextDisabled("%s", centre.c_str());
    ImGui::TableNextColumn();
    const double age = seconds_since(dc::row_observed(row, in.now), in.now);
    if (age <= 5) ImGui::TextColored(kOk, "%s", ago(age).c_str());
    else ImGui::TextUnformatted(ago(age).c_str());
    ImGui::TextDisabled("since %s", wall_time(dc::row_first_observed(row, in.now), in.now).c_str());
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", join(evidence_kinds(row), " · ").c_str());
    if (row.conflict) ImGui::TextColored(kWarn, "conflicting claims");
    ImGui::PopID();
}

// A drone's line: its lead row's level and answer, every band and the evidence of all its rows.
// Selecting it selects the lead row; the arrow opens the member rows beneath.
void draw_drone_line(State& state, const DroneLine& line, const Inputs& in, const Grouping& g) {
    const auto& lead = *line.rows.front();
    const float row_h = ImGui::GetTextLineHeightWithSpacing() * 2;
    const bool open = state.expanded.count(lead.id) > 0;
    ImGui::PushID(("drone " + lead.id).c_str());
    ImGui::TableNextRow(0, row_h);
    ImGui::TableNextColumn();
    const ImVec2 start = ImGui::GetCursorPos();
    bool selected = false;
    for (const auto* r : line.rows) selected = selected || state.selected == r->id;
    if (ImGui::Selectable("##drone", selected && !open, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                          {0, row_h}))
        state.selected = lead.id;
    ImGui::SetCursorPos(start);
    const auto v = dc::verdict(lead);
    tier_badge(v.tier);
    ImGui::TableNextColumn();
    ImGui::TextColored(drone_colour(line.drone), "D%d", line.drone);
    if (ImGui::IsItemHovered() && in.rows) ImGui::SetTooltip("%s", drone_tooltip(g, lead.id, *in.rows).c_str());
    if (ImGui::ArrowButton("##open", open ? ImGuiDir_Down : ImGuiDir_Right)) {
        if (open) state.expanded.erase(lead.id);
        else state.expanded.insert(lead.id);
    }
    ImGui::SameLine(0, 4 * ui_scale());
    ImGui::TextDisabled("%zu rows", line.rows.size());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rows that probably describe this drone. Open to see each row.");
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", dc::model_and_link(v).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", v.basis.c_str());
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", identity_line(lead).c_str());
    const auto note = identity_note(lead);
    if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", line.bands.c_str());
    ImGui::TableNextColumn();
    const double age = seconds_since(line.last, in.now);
    if (age <= 5) ImGui::TextColored(kOk, "%s", ago(age).c_str());
    else ImGui::TextUnformatted(ago(age).c_str());
    ImGui::TextDisabled("since %s", wall_time(line.first, in.now).c_str());
    ImGui::TableNextColumn();
    std::vector<std::string> kinds;
    bool conflict = false;
    for (const auto* r : line.rows) {
        for (const auto& k : evidence_kinds(*r))
            if (std::find(kinds.begin(), kinds.end(), k) == kinds.end()) kinds.push_back(k);
        conflict = conflict || r->conflict;
    }
    // evidence_kinds' fixed order across rows.
    std::vector<std::string> ordered;
    for (const char* k : {"Link", "DroneID", "Remote ID", "Classifier"})
        if (std::find(kinds.begin(), kinds.end(), k) != kinds.end()) ordered.push_back(k);
    ImGui::TextWrapped("%s", join(ordered, " · ").c_str());
    if (conflict) ImGui::TextColored(kWarn, "conflicting claims");
    ImGui::PopID();
    if (open)
        for (const auto* r : line.rows) draw_row_line(state, *r, &lead, in, g, true);
}

void draw_table(State& state, const std::vector<const Detection*>& rows, const Inputs& in, const Grouping& g) {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##drones now", 7, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 96 * ui_scale());
    ImGui::TableSetupColumn("Drone", ImGuiTableColumnFlags_WidthFixed, 92 * ui_scale());
    ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("Identity", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Band", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, 104 * ui_scale());
    ImGui::TableSetupColumn("Evidence", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableHeadersRow();
    for (const auto& line : drone_lines(rows, g, in.now)) {
        if (state.per_drone && line.drone && line.rows.size() > 1) draw_drone_line(state, line, in, g);
        else
            for (const auto* r : line.rows) draw_row_line(state, *r, line.rows.front(), in, g, false);
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- band bar

ImU32 colour_u32(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::GetColorU32(c);
}
std::string shortened(const std::string& s, std::size_t n) { return s.size() <= n ? s : s.substr(0, n - 3) + "..."; }
std::string mark_text(const band_bar::Mark& m) {
    char b[64];
    if (m.kind == band_bar::Kind::Link) std::snprintf(b, sizeof b, "%.0f-%.0f MHz", m.low_hz / 1e6, m.high_hz / 1e6);
    else std::snprintf(b, sizeof b, "%.1f MHz", m.centre() / 1e6);
    return b;
}
// "D1 Mini 2 OS2 · 2429.5": drone number, model with its OcuSync generation, and where the
// label's mark is. A Family row shows its generation(s) alone: "D2 DJI OS2/O3/O4 · link ~5795".
// `lead`: the drone's strongest row, whose answer names every row of the drone.
std::string band_label(const Detection& row, const band_bar::Mark& m, const Grouping& g, const Detection* lead = nullptr) {
    std::string out;
    if (const auto d = g.drone.find(row.id); d != g.drone.end()) out = "D" + std::to_string(d->second) + " ";
    auto v = dc::verdict(row);
    if (lead && lead != &row) {
        const auto best = dc::verdict(*lead);
        if (int(best.tier) < int(v.tier)) v = best;
    }
    // "DJI Lightbridge family" reads "DJI Lightbridge" in a label.
    const std::string family = " family";
    if (v.model.size() > family.size() && v.model.compare(v.model.size() - family.size(), family.size(), family) == 0)
        v.model.resize(v.model.size() - family.size());
    if (v.tier == dc::Tier::Family && !v.link_short.empty()) out += "DJI " + v.link_short;
    else out += shortened(v.model, v.link_short.empty() ? 18 : 14) + (v.link_short.empty() ? "" : " " + v.link_short);
    char b[32];
    if (m.kind == band_bar::Kind::Link) std::snprintf(b, sizeof b, " · link ~%.0f", m.centre() / 1e6);
    else std::snprintf(b, sizeof b, " · %.1f", m.centre() / 1e6);
    return out + b;
}
std::string band_tooltip(const Detection& row, const std::vector<band_bar::Mark>& marks, const Grouping& g) {
    const auto v = dc::verdict(row);
    std::string out = drone_label(g, row.id);
    out += (out.empty() ? "" : " · ") + std::string(dc::tier_name(v.tier)) + " · " + dc::model_and_link(v) + "\n" + identity_line(row);
    for (const auto& m : marks) {
        out += "\n" + std::string(band_bar::kind_name(m.kind)) + " " + mark_text(m) + ", " + ago(m.age_s);
        if (m.kind == band_bar::Kind::Link) out += "\n  (the captures it was heard in; the link is somewhere inside)";
    }
    const auto offset = droneid_offset_text(row);
    if (!offset.empty()) out += "\nDroneID offset: " + offset;
    const auto clock = link_clock_text(row);
    if (!clock.empty()) out += "\nLink clock: " + clock;
    return out + "\nClick to select this row.";
}

void draw_band_bar(State& state, Scanner& scanner, const std::vector<const Detection*>& rows, const Inputs& in,
                   const Grouping& g, bool coordinated_mode) {
    namespace bb = band_bar;
    const float scale = ui_scale();
    const auto segs = bb::segments();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool stacked = width < 760 * scale;
    const auto placed = bb::place(segs, width, 16 * scale, stacked);
    const float line_h = ImGui::GetTextLineHeight();
    const float pad = 3 * scale;
    const int max_label_lines = 3;
    const float label_h = line_h + 2 * pad + 2 * scale;
    const float bar_h = 20 * scale;

    ImGui::TextDisabled("Bands: where the receiver looks and where each drone was heard%s",
                        coordinated_mode ? "" : " (detection not running)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shaded: the usable span of each visit, brighter when more recent (green: receiving now,\n"
                          "orange: the last visit there failed). Faint: scheduled, not visited yet. Dark ends: outside\n"
                          "the band. Small ticks: documented DroneID channels.\n"
                          "Marks, coloured by drone number: a line for DroneID (exact channel), a dot for Remote ID,\n"
                          "a square for classifier-only, a box for a link (the captures it was heard in; the link is\n"
                          "somewhere inside). Hover a mark or label for details; click to select the row.");

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto visits = bb::visited(scanner.coordinated_visits(), Scanner::now_steady_s());
    const auto plan = bb::configured(scanner.effective_coordinated_schedule());
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool can_hover = ImGui::IsWindowHovered();
    const auto over = [&](ImVec2 a, ImVec2 b) { return can_hover && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y; };
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    struct RowMarks { const Detection* row; std::vector<bb::Mark> marks; ImVec4 colour; };
    std::vector<RowMarks> all;
    for (const auto* row : rows) {
        const auto d = g.drone.find(row->id);
        all.push_back({row, bb::marks(*row, in.now), d == g.drone.end() ? kMuted : drone_colour(d->second)});
    }
    // Labels, one per row and segment, above the row's most exact mark there. Placed first: the
    // label area is as tall as the lines they need (1 to 3).
    struct SegmentLabels {
        std::vector<bb::Label> labels;
        std::vector<const RowMarks*> rows;
        std::vector<std::string> texts;
    };
    std::vector<SegmentLabels> seg_labels(segs.size());
    int label_lines = 1;
    // One full label per drone and segment, on its strongest row's mark there; its other rows'
    // marks in the segment get the short drone tag ("D2").
    std::map<int, const Detection*> leads;
    for (const auto& line : drone_lines(rows, g, in.now))
        if (line.drone) leads[line.drone] = line.rows.front();
    for (std::size_t si = 0; si < segs.size(); ++si) {
        auto& sl = seg_labels[si];
        std::set<int> labelled;
        for (const auto& rm : all) {
            const auto* m = bb::anchor(rm.marks, segs, int(si));
            if (!m) continue;
            const auto d = g.drone.find(rm.row->id);
            const int drone = d == g.drone.end() ? 0 : d->second;
            if (drone && !labelled.insert(drone).second) sl.texts.push_back("D" + std::to_string(drone));
            else sl.texts.push_back(band_label(*rm.row, *m, g, drone ? leads[drone] : nullptr));
            sl.labels.push_back({origin.x + bb::x_of(segs[si], placed[si], m->centre()),
                                 ImGui::CalcTextSize(sl.texts.back().c_str()).x + 2 * pad});
            sl.rows.push_back(&rm);
        }
        bb::stack(sl.labels, origin.x + placed[si].x0, origin.x + placed[si].x1, 4 * scale, max_label_lines);
        for (const auto& l : sl.labels) label_lines = std::max(label_lines, l.line + 1);
    }
    const float labels_h = float(label_lines) * label_h;
    const float block_h = labels_h + bar_h + line_h + 8 * scale;

    const Detection* hovered = nullptr;
    const std::vector<bb::Mark>* hovered_marks = nullptr;
    int hidden = 0;
    for (std::size_t si = 0; si < segs.size(); ++si) {
        const auto& seg = segs[si];
        const auto& p = placed[si];
        const float top = origin.y + float(p.line) * block_h;
        const float x0 = origin.x + p.x0, x1 = origin.x + p.x1;
        const float bar_top = top + labels_h, bar_bot = bar_top + bar_h;
        const auto x = [&](double hz) { return origin.x + bb::x_of(seg, p, hz); };
        const auto span_rect = [&](const bb::Span& s, ImU32 c) {
            if (s.high_hz < seg.low_hz || s.low_hz > seg.high_hz) return;
            dl->AddRectFilled({x(s.low_hz), bar_top}, {x(s.high_hz), bar_bot}, c);
        };
        dl->AddRectFilled({x0, bar_top}, {x1, bar_bot}, ImGui::GetColorU32(ImGuiCol_FrameBg));
        for (const auto& s : plan) span_rect(s, colour_u32(kAccent, .12f));
        for (const auto& s : visits)
            span_rect(s, s.receiving ? colour_u32(kOk, .6f) : s.valid ? colour_u32(kAccent, .55f * bb::shade(s))
                                                                       : colour_u32(kWarn, .4f));
        for (const auto& s : visits)
            if (s.receiving && s.high_hz >= seg.low_hz && s.low_hz <= seg.high_hz)
                dl->AddRect({x(s.low_hz), bar_top}, {x(s.high_hz), bar_bot}, colour_u32(kOk, 1), 0, 0, 1.5f * scale);
        dl->AddRectFilled({x0, bar_top}, {x(seg.band_low_hz), bar_bot}, IM_COL32(0, 0, 0, 90));
        dl->AddRectFilled({x(seg.band_high_hz), bar_top}, {x1, bar_bot}, IM_COL32(0, 0, 0, 90));
        dl->AddRect({x0, bar_top}, {x1, bar_bot}, ImGui::GetColorU32(ImGuiCol_Border));
        for (const double* list : {std::begin(drone::droneid::channels_24), std::begin(drone::droneid::channels_58)}) {
            const double* end = list == std::begin(drone::droneid::channels_24) ? std::end(drone::droneid::channels_24)
                                                                                 : std::end(drone::droneid::channels_58);
            for (const double* ch = list; ch != end; ++ch)
                if (bb::segment_of(segs, *ch) == int(si))
                    dl->AddLine({x(*ch), bar_bot - 5 * scale}, {x(*ch), bar_bot}, colour_u32(text, .7f), 1.f * scale);
        }

        // Marks: links first (widest), then the exact kinds on top.
        for (const bool links : {true, false})
            for (const auto& rm : all)
                for (const auto& m : rm.marks) {
                    if ((m.kind == bb::Kind::Link) != links) continue;
                    if (m.high_hz < seg.low_hz || m.low_hz > seg.high_hz) continue;
                    const bool selected = state.selected == rm.row->id;
                    const float a = m.age_s > 30 ? .45f : 1.f;
                    const ImU32 c = colour_u32(rm.colour, a);
                    const float cx = x(m.centre()), mid = (bar_top + bar_bot) / 2, w = (selected ? 2.5f : 1.3f) * scale;
                    ImVec2 lo, hi;
                    if (m.kind == bb::Kind::Link) {
                        lo = {x(m.low_hz), bar_top + bar_h * .22f};
                        hi = {x(m.high_hz), bar_bot - bar_h * .22f};
                        dl->AddRectFilled(lo, hi, colour_u32(rm.colour, .28f * a));
                        dl->AddRect(lo, hi, c, 0, 0, w);
                    } else if (m.kind == bb::Kind::DroneId) {
                        dl->AddLine({cx, bar_top - 2 * scale}, {cx, bar_bot + 2 * scale}, c, (selected ? 3.5f : 2.2f) * scale);
                        lo = {cx - 4 * scale, bar_top};
                        hi = {cx + 4 * scale, bar_bot};
                    } else if (m.kind == bb::Kind::RemoteId) {
                        dl->AddCircleFilled({cx, mid}, 4 * scale, c);
                        if (selected) dl->AddCircle({cx, mid}, 6 * scale, colour_u32(text, 1), 0, 1.5f * scale);
                        lo = {cx - 5 * scale, bar_top};
                        hi = {cx + 5 * scale, bar_bot};
                    } else {
                        dl->AddRect({cx - 4 * scale, mid - 4 * scale}, {cx + 4 * scale, mid + 4 * scale}, c, 0, 0, w);
                        lo = {cx - 5 * scale, bar_top};
                        hi = {cx + 5 * scale, bar_bot};
                    }
                    if (over(lo, hi) && (!hovered || !links)) { hovered = rm.row; hovered_marks = &rm.marks; }
                }

        const auto& sl = seg_labels[si];
        for (std::size_t i = 0; i < sl.labels.size(); ++i) {
            const auto& l = sl.labels[i];
            if (l.line < 0) { ++hidden; continue; }
            const auto& rm = *sl.rows[i];
            const bool selected = state.selected == rm.row->id;
            const float y = top + labels_h - float(l.line + 1) * label_h;
            const ImVec2 lo{l.x, y}, hi{l.x + l.width, y + line_h + 2 * pad};
            const float mx = l.want_x;
            dl->AddLine({std::clamp(mx, lo.x + 2, hi.x - 2), hi.y}, {mx, bar_top}, colour_u32(rm.colour, .8f), 1.f * scale);
            dl->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_PopupBg));
            dl->AddRect(lo, hi, colour_u32(rm.colour, 1), 3 * scale, 0, (selected ? 2.f : 1.f) * scale);
            dl->AddText({lo.x + pad, lo.y + pad}, colour_u32(text, 1), sl.texts[i].c_str());
            if (over(lo, hi)) { hovered = rm.row; hovered_marks = &rm.marks; }
        }

        // Axis: band edges and the segment name.
        char b[32];
        const float ty = bar_bot + 3 * scale;
        std::snprintf(b, sizeof b, "%.0f", seg.band_low_hz / 1e6);
        dl->AddText({x(seg.band_low_hz), ty}, ImGui::GetColorU32(ImGuiCol_TextDisabled), b);
        std::snprintf(b, sizeof b, "%g", seg.band_high_hz / 1e6);
        dl->AddText({x(seg.band_high_hz) - ImGui::CalcTextSize(b).x, ty}, ImGui::GetColorU32(ImGuiCol_TextDisabled), b);
        dl->AddText({(x0 + x1 - ImGui::CalcTextSize(seg.name).x) / 2, ty}, colour_u32(text, .9f), seg.name);
    }
    int lines = 0;
    for (const auto& p : placed) lines = std::max(lines, p.line + 1);
    ImGui::Dummy({width, float(lines) * block_h});
    if (hovered) {
        ImGui::SetTooltip("%s", band_tooltip(*hovered, *hovered_marks, g).c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) state.selected = hovered->id;
    }
    if (hidden) ImGui::TextDisabled("%d label%s did not fit: hover the marks.", hidden, hidden == 1 ? "" : "s");
}

void draw_now(State& state, coordinated::PanelState& cp, Scanner& scanner, drone::Addon& drones, const Inputs& in,
              const Actions& act, const Grouping& grouping) {
    static const std::map<std::string, Detection> no_rows;
    const auto& all = in.rows ? *in.rows : no_rows;
    const bool coordinated_mode = scanner.active_band() == BAND_COORDINATED;

    ImGui::SeparatorText("Drones now");
    if (!all.empty()) {
        if (grouping.fewest < grouping.drones)
            ImGui::TextDisabled("%zu rows · likely %zu drone%s (perhaps %zu)", all.size(), grouping.drones,
                                grouping.drones == 1 ? "" : "s", grouping.fewest);
        else
            ImGui::TextDisabled("%zu rows · likely %zu drone%s", all.size(), grouping.drones, grouping.drones == 1 ? "" : "s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rows with the same Drone number (D1, D2, ...) probably describe one aircraft:\n"
                              "same serial number, DroneID on the same channel within 10 s, the same crystal\n"
                              "(DroneID ppm, link grid or link clock), or Lightbridge rows heard together.\n"
                              "\"?\" marks a weaker link (DroneID near a measured link).");
        ImGui::SameLine(0, 16 * ui_scale());
        ImGui::Checkbox("One line per drone", &state.per_drone);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: a drone's rows share one line (open it with the arrow). Off: every row on its own line.");
    }
    const auto counts = tier_counts(all);
    char label[64];
    std::snprintf(label, sizeof label, "All  %zu###tier all", all.size());
    if (chip(label, state.tier_filter < 0)) state.tier_filter = -1;
    for (const auto t : {dc::Tier::Confirmed, dc::Tier::Likely, dc::Tier::Family, dc::Tier::Reported,
                         dc::Tier::Unknown, dc::Tier::Classifier}) {
        const auto it = counts.find(int(t));
        const std::size_t n = it == counts.end() ? 0 : it->second;
        if ((t == dc::Tier::Unknown || t == dc::Tier::Classifier) && !n && state.tier_filter != int(t)) continue;
        std::snprintf(label, sizeof label, "%s  %zu###tier %d", dc::tier_name(t), n, int(t));
        next_on_line(button_w(label), 6);
        if (chip(label, state.tier_filter == int(t), tier_colour(t)))
            state.tier_filter = state.tier_filter == int(t) ? -1 : int(t);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tier_help(t));
    }
    if (in.classifier_only_rows) {
        const char* dev = "Classifier-only (development)";
        next_on_line(button_w(dev), 14);
        if (chip(dev, *in.classifier_only_rows, tier_colour(dc::Tier::Classifier)))
            *in.classifier_only_rows = !*in.classifier_only_rows;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show rows only the classifier supports. Kept off until the no-drone background "
                              "shows at most 1 false row per hour.");
    }

    const auto rows = filter_rows(all, state.tier_filter, in.now);
    // The details panel follows the table: keep the selection while its row is listed.
    if (std::none_of(rows.begin(), rows.end(), [&](const Detection* r) { return r->id == state.selected; }))
        state.selected = rows.empty() ? std::string() : rows.front()->id;
    const Detection* selected = nullptr;
    if (auto it = all.find(state.selected); it != all.end()) selected = &it->second;

    const float scale = ui_scale();
    if (state.table_height <= 0) state.table_height = 300 * scale;
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool side = avail > 1050 * scale;
    const float panel_w = side ? std::min(430 * scale, avail * .38f) : 0;
    const bool show_details = in.started && !all.empty();

    ImGui::BeginChild("##now table", {show_details && side ? avail - panel_w - ImGui::GetStyle().ItemSpacing.x : 0,
                                      state.table_height},
                      ImGuiChildFlags_Borders);
    if (!in.started) {
        draw_empty("Receiver not connected", "Connect the receiver to start detecting. Drones from earlier in this session are in History.");
        const float w = button_w("Connect receiver");
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetContentRegionAvail().x - w) / 2));
        push_button(kAccent, true);
        if (ImGui::Button("Connect receiver") && act.open_setup) act.open_setup();
        pop_button();
    } else if (all.empty() && !coordinated_mode) {
        draw_empty("No drones in recent evidence",
                   "Detection is not running: only the current band is received. Start detection to visit "
                   "Sub-GHz, 2.4 GHz and 5 GHz in turn with every drone decoder and the classifier.");
        const float w = button_w("Start detection");
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetContentRegionAvail().x - w) / 2));
        push_button(kAccent, true);
        if (ImGui::Button("Start detection")) coordinated::start_detection(cp, scanner, drones);
        pop_button();
    } else if (all.empty()) {
        draw_empty("No drones in recent evidence",
                   "Every band is being visited in turn. Not seeing a drone is not proof that none is there: "
                   "the receiver hears one band at a time, and weak or unmonitored signals can be missed.");
    } else if (rows.empty()) {
        draw_empty("No drones at this evidence level", "Choose All to see every drone.");
    } else {
        draw_table(state, rows, in, grouping);
    }
    ImGui::EndChild();
    if (show_details && side) {
        ImGui::SameLine();
        ImGui::BeginChild("##details", {0, state.table_height}, ImGuiChildFlags_Borders);
        draw_details(state, cp, selected, in, act, grouping);
        ImGui::EndChild();
    }
    gui::height_handle("##now height", state.table_height);
    if (show_details && !side) {
        ImGui::BeginChild("##details stacked", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        draw_details(state, cp, selected, in, act, grouping);
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("About these results")) {
        ImGui::BulletText("DroneID and Remote ID are broadcast by the aircraft itself. They are not authenticated.");
        ImGui::BulletText("The level is the strongest evidence for a row: Confirmed, Likely, Family, Reported, Unknown,\n"
                          "Classifier. Hover a level for what it means.");
        ImGui::BulletText("A row is an observation, not a count of aircraft. Two rows can be the same drone.");
        ImGui::BulletText("Not seeing a drone is not proof that none is there: the receiver visits one band at a time,\n"
                          "and weak or unmonitored signals can be missed. Sub-GHz is not used for drone decisions.");
        ImGui::BulletText("Classifier-only rows are in development. Show them with the dashed filter above the table.");
    }
}

// ---------------------------------------------------------------- tabs

void draw_package_picker(State& state, ml::Inference& model, const Inputs& in, const Actions& act) {
    if (!in.classifier_package) return;
    if (state.packages_scanned < 0 || ImGui::GetTime() - state.packages_scanned > 5) {
        state.packages = find_packages(in.packages_root);
        state.packages_scanned = ImGui::GetTime();
    }
    const auto label_of = [&](const std::string& path) {
        for (const auto& p : state.packages)
            if (p.path == path) return p.label + (path == in.default_package ? "  default" : "");
        return std::filesystem::path(path).filename().string() + "  (not found)";
    };
    ImGui::SetNextItemWidth(std::min(340 * ui_scale(), ImGui::GetContentRegionAvail().x * .6f));
    if (ImGui::BeginCombo("Classifier package", label_of(*in.classifier_package).c_str())) {
        for (const auto& p : state.packages) {
            const bool current = p.path == *in.classifier_package;
            if (ImGui::Selectable(label_of(p.path).c_str(), current) && !current) {
                *in.classifier_package = p.path;
                if (act.reload_classifier) act.reload_classifier();
            }
            if (current) ImGui::SetItemDefaultFocus();
        }
        if (state.packages.empty()) ImGui::TextDisabled("No packages under %s", in.packages_root.c_str());
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The trained package the universal classifier loads (weights and naming limits).\n"
                          "Changing it reloads a running classifier; rows from the previous package age out.");
    const auto ms = model.snapshot();
    ImGui::SameLine();
    if (ms.enabled)
        ImGui::TextDisabled("%s%s", ms.state.c_str(), ms.model_hash.empty() ? "" : (", model " + ms.model_hash.substr(0, 12)).c_str());
    else
        ImGui::TextDisabled("classifier off; used when it starts");
}

void draw_recorder(State& state, const Inputs& in, const Actions& act) {
    if (!in.recorder) return;
    ImGui::SeparatorText("Training recorder");
    const auto rs = in.recorder->status();
    gui::note("Saves the captures of drones whose model a readable DroneID names, labelled with that model "
              "(self-reported), to retrain the classifier on this receiver. About 41 MB per capture; "
              "encrypted DroneID and link-only drones are not saved.");
    if (!state.recorder_dir[0]) std::snprintf(state.recorder_dir, sizeof state.recorder_dir, "%s", in.recorder_default_dir.c_str());
    ImGui::BeginDisabled(rs.recording);
    ImGui::SetNextItemWidth(std::min(520 * ui_scale(), ImGui::GetContentRegionAvail().x * .7f));
    ImGui::InputText("Folder##recorder", state.recorder_dir, sizeof state.recorder_dir);
    ImGui::EndDisabled();
    if (!rs.recording) {
        push_button(kAccent, true);
        if (ImGui::Button("Start recording") && act.start_recording) act.start_recording(state.recorder_dir);
        pop_button();
    } else {
        push_button(kBad, false);
        if (ImGui::Button("Stop recording") && act.stop_recording) act.stop_recording();
        pop_button();
    }
    if (!rs.dir.empty()) {
        std::string models;
        for (const auto& [m, n] : rs.per_model) models += (models.empty() ? "" : ", ") + m + " " + std::to_string(n);
        ImGui::TextWrapped("%s %s: %zu captures (%.1f GB)%s%s", rs.recording ? "Recording to" : "Last session",
                           rs.dir.c_str(), rs.saved, double(rs.bytes) / (1 << 30), models.empty() ? "" : ": ",
                           models.c_str());
        ImGui::TextDisabled("Waiting to write %zu | evicted before saving %zu | queue full %zu | claimed by two models %zu | "
                            "other sample rate %zu | free %.0f GB",
                            rs.pending, rs.missed, rs.dropped, rs.ambiguous, rs.skipped_rate, double(rs.free_bytes) / (1 << 30));
    }
    if (!rs.error.empty()) gui::note(rs.error.c_str(), true);
}

void draw_diagnostics(State& state, Scanner& scanner, drone::Addon& drones, ml::Inference& model, shortrange::Monitor* bluetooth,
                      const Inputs& in, const Actions& act) {
    draw_recorder(state, in, act);
    ImGui::SeparatorText("Modules");
    bool on = drones.unified;
    if (ImGui::Checkbox("Drone decoders and classifier (on by default)", &on)) {
        if (on) drones.start_unified();
        else drones.stop_unified();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("DJI DroneID (native and the older decoder), Wi-Fi Remote ID and the universal classifier.\n"
                          "Off: rows come from link timing and Bluetooth Remote ID only.");
    draw_package_picker(state, model, in, act);
    coordinated::draw_module_health(drones, model, bluetooth);
    for (const auto& line : in.diagnostics) ImGui::TextWrapped("%s", line.c_str());
    for (const auto& p : in.problems) gui::note(p.c_str(), true);
    (void)scanner;
    ImGui::SeparatorText("Decoders");
    drone::draw_decoder_sections(drones);
    if (bluetooth && ImGui::CollapsingHeader("Bluetooth Remote ID"))
        shortrange::draw_remote_id_section(bluetooth->snapshot());
}

}  // namespace

void draw(State& state, coordinated::PanelState& cp, Scanner& scanner, drone::Addon& drones, ml::Inference& model,
          shortrange::Monitor* bluetooth, const Inputs& in, const Actions& act) {
    if (in.select_offline) state.select_tab = TabOffline;
    draw_status(cp, scanner, drones, model, in, act);
    ImGui::Spacing();
    static const std::map<std::string, Detection> no_rows;
    const auto every_row = filter_rows(in.rows ? *in.rows : no_rows, -1, in.now);
    const auto grouping = group_rows(every_row);
    draw_alert(state, in, grouping);
    draw_band_bar(state, scanner, every_row, in, grouping, scanner.active_band() == BAND_COORDINATED);
    draw_now(state, cp, scanner, drones, in, act, grouping);

    ImGui::Spacing();
    if (ImGui::BeginTabBar("##drone tabs")) {
        const auto flags = [&](Tab t) { return state.select_tab == t ? ImGuiTabItemFlags_SetSelected : 0; };
        if (ImGui::BeginTabItem("History", nullptr, flags(TabHistory))) {
            if (state.history_height <= 0) state.history_height = 300 * ui_scale();
            if (in.tracks) dc::draw_drone_history(*in.tracks, in.now, state.history_height);
            gui::height_handle("##history height", state.history_height);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Evidence", nullptr, flags(TabEvidence))) {
            coordinated::draw_evidence_view(cp, scanner, drones, bluetooth);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Coverage", nullptr, flags(TabCoverage))) {
            coordinated::draw_coverage_view(cp, scanner);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics", nullptr, flags(TabDiagnostics))) {
            draw_diagnostics(state, scanner, drones, model, bluetooth, in, act);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Offline review", nullptr, flags(TabOffline))) {
            drone::draw_offline_review(drones);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    state.select_tab = -1;
}

}  // namespace rfmon::drones_page
#endif
