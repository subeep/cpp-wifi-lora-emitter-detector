// Coordinated Drones-page section, offscreen (upgradedplan.md P2): renders the
// production draw_section() against a production Scanner running coordinated
// mode on a fake radio, and checks the combined-entry builder keeps evidence
// levels and coverage-aware freshness distinct. No receiver is used.
#include "coordinated_panel.hpp"
#include "gui/app_shell.hpp"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace rfmon;
namespace rfmon {
struct ScannerTestAccess {
    static void set_device_factory(Scanner& s, decltype(Scanner::device_factory_) f) { s.device_factory_ = std::move(f); }
};
}  // namespace rfmon

namespace {
void require(bool value, const std::string& why) {
    if (!value) throw std::runtime_error(why);
    std::cout << "PASS " << why << '\n';
}
class QuietRadio : public CaptureDevice {
public:
    CaptureResult capture_detailed(double center, double rate, double duration, double settle, bool retune) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(settle + duration));
        CaptureResult r;
        r.sample_rate_hz = rate;
        r.samples.resize(size_t(rate * duration));
        std::normal_distribution<float> g(0.f, 1e-3f);
        for (auto& x : r.samples) x = {g(rng_), g(rng_)};
        r.timing.device_time_valid = true;
        r.timing.device_time_ns = (ns_ += 1000000000);
        r.timing.requested_samples = r.samples.size();
        r.timing.actual_rf_hz = center;
        r.timing.retuned = retune;
        return r;
    }
    void set_gain(std::optional<double>) override {}
private:
    std::mt19937 rng_{3};
    int64_t ns_ = 0;
};

std::vector<rfmon::evidence::Entry> g_entries;
std::deque<coord::VisitRecord> g_visits;

void entry_tests() {
    const double now = Scanner::now_steady_s();
    const auto at = [](double s) {
        return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s)));
    };
    drone::Snapshot native, legacy;
    drone::Observation o;
    o.protocol = "DJI DroneID";
    o.evidence = "CRC-valid packet";
    o.serial = "SYNTHETIC123";
    o.model = "Mini 2";
    o.confirmed = true;
    o.source = "LIVE / run r1 / session 1 / capture 7 / sample 0";
    o.timestamp = 12.25;
    o.frequency = 2444.5e6;
    o.acquired_at = at(now - 30);
    o.count = 3;
    native.observations.push_back(o);
    o.model = "Mini 3";  // same claimed serial, different claimed model: a conflict to surface
    o.source = "LIVE / run r1 / session 1 / capture 3 / sample 0";
    o.acquired_at = at(now - 40);
    legacy.observations.push_back(o);
    drone::RidJson rid = {{"tracks", drone::RidJson::array({{{"transport", "Beacon"}, {"protocol_version", 2}, {"transmitter", "aa:bb:cc:dd:ee:ff"},
        {"packs", 5}, {"messages", drone::RidJson::array({{{"type", "Basic ID"}, {"reported_id", "SYNTHETIC123"}, {"ua_type_name", "Helicopter / multirotor"},
        {"age_s", 2.0}, {"source", {{"channel_hz", 2437e6}, {"capture_seq", 9}, {"device_time_ns", 20000000000}}}}})}}})}};
    ml::Snapshot model;
    ml::Result r;
    r.candidate = "DJI_Mini_2";
    r.center = 5181.5e6;
    r.completed = at(now - 1);
    r.capture = 7;  // the model analysed the same capture the DJI packet came from
    model.history.push_back(r);
    std::deque<coord::VisitRecord> visits;
    coord::VisitRecord v;
    v.center_hz = 2443.5e6;
    v.usable_half_span_hz = coord::usable_half_span(20e6);
    v.start_s = now - 20;
    v.end_s = now - 19.4;
    v.valid = v.analysed = true;
    visits.push_back(v);  // covered the DJI entry after it was seen: not reacquired
    v.center_hz = 2430e6;  // covers 2437 MHz, not 2444.5 MHz (+/-8.8 MHz usable)
    v.start_s = now - 0.2;
    v.in_progress = true;
    visits.push_back(v);  // covering the Remote ID entry right now
    const auto entries = coordinated::build_entries(native, legacy, rid, model, visits, now, 60);
    require(entries.size() == 4, "One row per detector observation; no merged drone claim");
    require(entries[0].source == "Model (diagnostic)" && entries[1].source == "Wi-Fi Remote ID" && entries[2].source == "DJI native" &&
                entries[3].source == "DJI fallback",
            "Rows sorted newest first");
    require(entries[0].identity.find("not accepted") != std::string::npos &&
                entries[1].identity.find("unauthenticated") != std::string::npos &&
                entries[2].identity.find("unauthenticated") != std::string::npos,
            "Identity claims keep their evidence status");
    require(entries[1].freshness == coord::Freshness::ReceivingNow, "Remote ID entry on the frequency being observed is receiving now");
    require(entries[2].freshness == coord::Freshness::NotReacquired, "DJI entry covered by a later valid visit is not reacquired");
    require(entries[0].freshness == coord::Freshness::RecentBandUnobserved, "Model entry with no later 5 GHz visit stays recent");
    require(std::abs(entries[1].frequency_hz - 2437e6) < 1 && entries[1].count == 5, "Remote ID frequency/count from frame provenance");
    require(entries[2].capture_seq == 7u && entries[3].capture_seq == 3u && entries[1].capture_seq == 9u && entries[0].capture_seq == 7u,
            "Each row keeps the capture its latest evidence came from");
    require(entries[2].event_device_time_s == 12.25 && entries[1].event_device_time_s && std::abs(*entries[1].event_device_time_s - 20) < 1e-9,
            "Evidence device times kept for IQ span export");
    std::set<std::string> keys;
    for (const auto& e : entries) keys.insert(e.key);
    require(keys.size() == entries.size(), "Row keys are unique (stable inspector selection)");
    auto has = [](const std::vector<std::string>& v, const std::string& text) {
        return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
    };
    require(has(entries[2].relations, "#2 Wi-Fi Remote ID: same reported identifier") &&
                has(entries[2].relations, "#4 DJI fallback: same reported identifier") &&
                has(entries[1].relations, "#3 DJI native: same reported identifier"),
            "Same reported identifier relates rows across detectors, stated as unauthenticated claims");
    require(has(entries[0].relations, "#3 DJI native: evidence from the same capture #7") &&
                has(entries[2].relations, "#1 Model (diagnostic): evidence from the same capture #7"),
            "Model and DJI evidence from the same capture are related as not independent");
    require(has(entries[2].conflicts, "reported model differs: 'Mini 2'") && has(entries[3].conflicts, "'Mini 3'") &&
                entries[1].conflicts.empty(),
            "Conflicting reported models stay visible on both rows");
    require(entries[0].relations.size() == 1, "No relationship from frequency, product name or timing alone");
    g_entries = entries;
    g_visits = visits;
    g_visits.front().capture_seq = 7;  // the DJI evidence's own visit
    require(rfmon::evidence::capture_from_source("LIVE / run x / session 2 / capture 4711 / sample 64") == 4711u &&
                !rfmon::evidence::capture_from_source("OFFLINE / file / sample 0"),
            "Capture reference parsed from the detector source string");
}

void export_tests() {
    namespace fs = std::filesystem;
    char tmp[] = "/tmp/rfmon-evidence-XXXXXX";
    require(mkdtemp(tmp) != nullptr, "Temporary export root");
    rfmon::evidence::Entry e;
    e.source = "DJI native";
    e.identity = "reported serial X (unauthenticated)";
    e.frequency_hz = 2444.5e6;
    e.capture_seq = 7;
    e.event_device_time_s = 5.1;  // 100,000 samples into the capture below
    e.raw_json = R"({"packet":{"serial":"X"}})";
    e.relations = {"#2 Wi-Fi Remote ID: same reported identifier"};
    Scanner::RetainedCapture c;
    c.record.capture_seq = 7;
    c.record.run_id = "run-1";
    c.record.sample_rate_hz = 1e6;
    c.record.capture_center_hz = 2443.5e6;
    c.record.clock = wifi_security::ClockDomain::UsrpDevice;
    c.record.device_time_ns = 5000000000;
    auto iq = std::make_shared<std::vector<std::complex<float>>>(200000);
    for (size_t i = 0; i < iq->size(); ++i) (*iq)[i] = {float(i), -float(i)};
    c.iq = iq;
    std::deque<coord::VisitRecord> visits;
    coord::VisitRecord v;
    v.capture_seq = 7;
    v.center_hz = 2443.5e6;
    v.usable_half_span_hz = coord::usable_half_span(20e6);
    visits.push_back(v);
    const auto dir = rfmon::evidence::export_entry(tmp, e, visits, "hash123", c, Scanner::now_steady_s());
    nlohmann::json j;
    std::ifstream(fs::path(dir) / "evidence.json") >> j;
    require(j["schema"] == "rfmon-drone-evidence" && j["schedule_hash"] == "hash123" && j["entry"]["capture_seq"] == 7 &&
                j["entry"]["raw"]["packet"]["serial"] == "X" && j["entry"]["relations"].size() == 1 && j["visits_covering_frequency"].size() == 1,
            "Evidence manifest holds entry, raw output, relations, visit and schedule identity");
    require(j["iq"]["exported_first_sample"] == 50000 && j["iq"]["exported_samples"] == 100000 && j["iq"]["capture_seq"] == 7 &&
                j["iq"]["first_exported_device_time_ns"] == 5050000000LL,
            "IQ span is +/-50 ms around the evidence time, with its device-time anchor");
    std::ifstream f(fs::path(dir) / "iq.cf32_le", std::ios::binary);
    std::vector<float> head(2);
    f.read(reinterpret_cast<char*>(head.data()), 8);
    require(fs::file_size(fs::path(dir) / "iq.cf32_le") == 100000 * 8 && head[0] == 50000.f && head[1] == -50000.f,
            "IQ file is the exact sample span, interleaved float32");
    const auto again = rfmon::evidence::export_entry(tmp, e, visits, "hash123", c, Scanner::now_steady_s());
    require(again != dir && fs::exists(fs::path(dir) / "iq.cf32_le"), "A second export never overwrites the first");
    const auto meta_only = rfmon::evidence::export_entry(tmp, e, visits, "hash123", std::nullopt, Scanner::now_steady_s());
    nlohmann::json m;
    std::ifstream(fs::path(meta_only) / "evidence.json") >> m;
    require(m["iq"].is_null() && m["iq_unavailable_reason"].get<std::string>().find("no longer retained") != std::string::npos &&
                !fs::exists(fs::path(meta_only) / "iq.cf32_le"),
            "Without a retained capture the export is metadata only and says why");
    std::error_code ec;
    fs::remove_all(tmp, ec);
}
}  // namespace

int main() {
    entry_tests();
    export_tests();
    auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    require(eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "Offscreen EGL available");
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    require(eglChooseConfig(display, attrs, &config, 1, &count) && count, "EGL config");
    EGLint dims[] = {EGL_WIDTH, 1600, EGL_HEIGHT, 1000, EGL_NONE};
    auto surface = eglCreatePbufferSurface(display, config, dims);
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    require(eglMakeCurrent(display, surface, surface, context), "EGL make current");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600, 1000};
    io.DeltaTime = 1.f / 60;
    gui::configure_font();
    gui::apply_theme();
    ImGui_ImplOpenGL3_Init("#version 130");

    char tmp[] = "/tmp/rfmon-coordinated-panel-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);
    int rc = 0;
    try {
        Scanner scanner(tmp);
        ScannerTestAccess::set_device_factory(scanner, [](const DeviceProfile&, std::optional<double>) {
            return std::unique_ptr<CaptureDevice>(new QuietRadio());
        });
        auto schedule = coord::default_schedule(2e6);
        for (auto& slot : schedule.slots) slot.dwell_s = 0.05;
        schedule.slots[0].visits[0].sample_rate_hz = 1e6;
        schedule.settle_s = 0.005;
        scanner.set_wifi_sample_rate(2e6);
        scanner.set_coordinated_schedule(schedule);
        drone::Addon drones;
        ml::Inference model;
        coordinated::PanelState state;
        std::string text;
        auto frame = [&] {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({1600, 1000});
            ImGui::Begin("Drones");
            ImGui::LogToBuffer(0);
            coordinated::draw_section(state, scanner, drones, model, true);
            text = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::Render();
            if (ImGui::GetCurrentContext()->ErrorCountCurrentFrame != 0) throw std::runtime_error("ImGui state error");
            glViewport(0, 0, 1600, 1000);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        };
        scanner.set_active_band(BAND_SUB_GHZ);
        frame();
        require(text.find("Start coordinated detection") != std::string::npos && text.find("Configured: cycle") != std::string::npos,
                "Stopped: Start control and configured cycle shown");
        require(scanner.active_band() == BAND_SUB_GHZ, "Rendering alone does not change acquisition");
        scanner.set_active_band(BAND_COORDINATED);
        scanner.start();
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (scanner.coordinated_status().cycle < 2 && std::chrono::steady_clock::now() < end) {
            frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        while (!scanner.coordinated_status().current && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame();
        require(text.find("Stop coordinated detection") != std::string::npos, "Running: Stop control shown");
        require(text.find("Receiving:") != std::string::npos && text.find("-> repeat") != std::string::npos, "Schedule strip with current visit");
        require(text.find("Lane drops") != std::string::npos && text.find("Revisit per centre: configured / measured") != std::string::npos,
                "Per-band coverage table");
        require(text.find("All detection entries") != std::string::npos && text.find("not a count of physical aircraft") != std::string::npos,
                "Combined entries table with its meaning stated");
        require(text.find("drone model is not supported there") != std::string::npos, "Sub-GHz ML limitation disclosed");
        require(text.find("No entry selected.") != std::string::npos && text.find("Retain recent Wi-Fi/drone visit IQ") != std::string::npos,
                "Inspector and opt-in IQ retention control shown");
        require(scanner.iq_retention() == 0, "IQ retention is off unless explicitly enabled");
        // Inspector on a real row: provenance, relationships, conflicts, export.
        state.selected_key = g_entries[2].key;
        state.export_dir = std::string(tmp) + "/evidence";
        {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({1600, 1000});
            ImGui::Begin("Entries");
            ImGui::LogToBuffer(0);
            coordinated::draw_entry_views(state, scanner, g_entries, g_visits, "hash", Scanner::now_steady_s());
            text = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::Render();
            if (ImGui::GetCurrentContext()->ErrorCountCurrentFrame != 0) throw std::runtime_error("ImGui state error");
        }
        require(text.find("Source interval: capture #7, visit") != std::string::npos && text.find("Provisional relationships") != std::string::npos &&
                    text.find("Conflict: reported model differs") != std::string::npos && text.find("Export evidence") != std::string::npos &&
                    text.find("IQ for this entry's capture: not retained") != std::string::npos,
                "Inspector shows source visit, relationships, conflicts, IQ status and export");
        require(text.find("4 ~ !") != std::string::npos, "Table marks rows with relationships and conflicts");
        state.selected_key = "rid/gone";
        frame();
        require(text.find("no longer retained by its detector") != std::string::npos, "A vanished selection is reported, not silently replaced");
        scanner.set_wifi_fixed_channel(6);
        const auto conflict_end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (scanner.coordinated_status().conflict.empty() && std::chrono::steady_clock::now() < conflict_end)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        frame();
        require(text.find("Clear fixed Wi-Fi channel") != std::string::npos, "Conflict shown with its explicit resolution control");
        scanner.stop();
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        rc = 1;
    }
    std::filesystem::current_path(cwd);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    std::cout << (rc ? "FAILED" : "ALL PASSED") << '\n';
    return rc;
}
