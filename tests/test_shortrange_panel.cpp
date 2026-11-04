// Headless render of the "Bluetooth & Zigbee" page and the Drones page's
// Bluetooth Remote ID section from a synthetic capture (no radio, no GPU):
// checks the side-by-side lists, inspectors, coverage and mode warnings.
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"
#include "shortrange/panel.hpp"
#include "shortrange_synth.hpp"

using namespace rfmon::shortrange;

static int failures = 0;
static void check(bool ok, const std::string& name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++failures;
}

static std::string render(const std::function<void()>& body) {
    std::string text;
    for (int i = 0; i < 3; ++i) {  // later frames settle layout and selection
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::Begin("Bluetooth & Zigbee");
        ImGui::LogToBuffer();
        body();
        text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        if (ImGui::GetCurrentContext()->ErrorCountCurrentFrame) throw std::runtime_error("ImGui state error");
    }
    return text;
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600, 1000};
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    Monitor m([](const uint8_t* msg) { return std::string("{\"type\":") + std::to_string(msg[0] >> 4) + "}"; });
    std::mt19937 rng(3);
    const auto wide = shortrange_synth::capture(rng);
    m.process(wide.data(), wide.size(), shortrange_synth::kRate, shortrange_synth::kCentre, "TEST / synthetic capture");
    PanelState st;
    st.export_dir = "/tmp/rfmon-shortrange-panel-test";

    auto text = render([&] { draw_page(st, m, "wifi_2g4", true); });
    check(text.find("Start Bluetooth & Zigbee decoding") != std::string::npos, "start control shown while stopped");
    check(text.find("Bluetooth LE: 1 probable devices from 1 addresses (1 named)") != std::string::npos, "BLE list count");
    check(text.find("Zigbee / IEEE 802.15.4: 2 addresses") != std::string::npos, "802.15.4 list count");
    check(text.find("RFMON-TEST") != std::string::npos && text.find("C1:22:33:44:55:66") != std::string::npos, "BLE row name and address");
    check(text.find("PAN 0x1A62 / 0x0000") != std::string::npos && text.find("PAN coordinator") != std::string::npos, "Zigbee coordinator row");
    check(text.find("BLE channel 37 (2402 MHz)") != std::string::npos, "2.4 GHz band-mode coverage note");

    st.selected_ble = m.snapshot().ble_grouped.at(0).key;
    st.selected_zigbee = "PAN 0x1A62 / 0x0000";
    text = render([&] { draw_page(st, m, "wifi_2g4", true); });
    check(text.find("Find My") != std::string::npos && text.find("Advertising data (latest)") != std::string::npos &&
              text.find("Complete local name") == std::string::npos,  // label comes from the assigned-number table
          "BLE inspector fields");
    check(text.find("Device Name") != std::string::npos && text.find("Appearance") != std::string::npos, "BLE inspector decoded AD fields");
    check(text.find("Extended PAN ID") != std::string::npos && text.find("11:22:33:44:55:66:77:88") != std::string::npos,
          "Zigbee inspector beacon fields");

    text = render([&] { draw_page(st, m, "wifi_5g", true); });
    check(text.find("does not capture 2.4 GHz") != std::string::npos, "5 GHz mode warns lists are history");
    m.enable(true);
    text = render([&] { draw_page(st, m, "coordinated", false); });
    check(text.find("Stop decoding") != std::string::npos && text.find("Receiver not connected") != std::string::npos &&
              text.find("BLE channel 37 (2402 MHz)") == std::string::npos, "running, disconnected, coordinated");

    text = render([&] { draw_remote_id_section(m.snapshot()); });
    check(text.find("No Bluetooth Remote ID received") != std::string::npos, "Drones section without Remote ID");
    std::vector<uint8_t> msg(25, 0);
    msg[0] = 0x02;
    msg[1] = 0x12;
    std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40, 0x50, 0xC6, 30, 0x16, 0xFA, 0xFF, 0x0D, 0x01};
    payload.insert(payload.end(), msg.begin(), msg.end());
    std::vector<uint8_t> pdu = {0x42, uint8_t(payload.size())};
    pdu.insert(pdu.end(), payload.begin(), payload.end());
    std::vector<std::complex<float>> cap(200000);
    std::normal_distribution<float> noise(0.f, 1.f);
    for (auto& v : cap) v = {noise(rng) * 0.01f, noise(rng) * 0.01f};
    shortrange_synth::place(cap, 20000, ble::modulate(pdu, 37), 1e6, 0.2);
    m.process(cap.data(), cap.size(), 20e6, 2401e6, "TEST / 2401 MHz visit");
    text = render([&] { ImGui::SetNextItemOpen(true); draw_remote_id_section(m.snapshot()); });
    check(text.find("C6:50:40:30:20:10") != std::string::npos && text.find("Basic ID / id type 1: {\"type\":0}") != std::string::npos,
          "Drones section lists Bluetooth Remote ID");

    ImGui::DestroyContext();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
