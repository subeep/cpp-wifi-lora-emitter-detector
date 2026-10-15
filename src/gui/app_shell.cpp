#include "app_shell.hpp"
#include <algorithm>
#include <filesystem>

namespace rfmon::gui {
std::string band_label(const std::string& band) {
    if(band=="wifi_2g4")return "Wi-Fi 2.4 GHz";
    if(band=="wifi_5g")return "Wi-Fi 5 GHz";
    if(band=="sub_ghz_ism")return "LoRa / Sub-GHz ISM";
    return band;
}
const char* page_name(Page p) {
    switch(p) {
    case Page::Overview:return "Overview"; case Page::Wifi24:return "Wi-Fi 2.4 GHz";
    case Page::Wifi5:return "Wi-Fi 5 GHz"; case Page::Lora:return "LoRa";
    case Page::Drones:return "Drones"; case Page::Security:return "Security";
    case Page::Captures:return "Captures"; default:return "Settings";
    }
}
const char* page_purpose(Page p) {
    switch(p) {
    case Page::Overview:return "Acquisition, observation health and evidence at a glance.";
    case Page::Wifi24:case Page::Wifi5:return "Sampled RF observations, decoded packets and claimed network identities.";
    case Page::Lora:return "RF observations, PHY hypotheses and provisional emitter clusters.";
    case Page::Drones:return "Reported broadcasts and telemetry. Identity is unauthenticated.";
    case Page::Security:return "Suspected activity and the coverage needed to interpret it.";
    case Page::Captures:return "Existing capture and offline review workflows. Results stay separate from live evidence.";
    default:return "Explicit acquisition controls, display preferences and implemented capabilities.";
    }
}
void configure_font() {
    // Startup only. Use a standard local font where available; no download or dependency.
    auto& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    const char* path="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    if(std::filesystem::exists(path) && io.Fonts->AddFontFromFileTTF(path,16))return;
    ImFontConfig font;font.SizePixels=16;io.Fonts->AddFontDefault(&font);
}
void apply_theme(float scale, bool compact) {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowPadding={16,16}; style.FramePadding={10,compact?4.f:7.f};
    style.ItemSpacing={10,10};style.ItemInnerSpacing={8,6};style.CellPadding={8,compact?3.f:6.f};
    style.WindowRounding=8;style.ChildRounding=6;style.FrameRounding=5;style.PopupRounding=7;
    style.TabRounding=5;style.ScrollbarSize=13;style.WindowBorderSize=0;style.ChildBorderSize=1;
    auto& c=style.Colors;
    c[ImGuiCol_Text]={.90f,.93f,.96f,1}; c[ImGuiCol_TextDisabled]={.62f,.69f,.76f,1};
    c[ImGuiCol_WindowBg]={.065f,.085f,.115f,1};c[ImGuiCol_ChildBg]={.085f,.11f,.15f,1};
    c[ImGuiCol_PopupBg]={.095f,.125f,.17f,1};c[ImGuiCol_Border]={.20f,.26f,.33f,.7f};
    c[ImGuiCol_FrameBg]={.13f,.18f,.24f,1};c[ImGuiCol_FrameBgHovered]={.18f,.26f,.34f,1};
    c[ImGuiCol_Button]={.13f,.23f,.31f,1};c[ImGuiCol_ButtonHovered]={.16f,.34f,.44f,1};
    c[ImGuiCol_ButtonActive]={.17f,.43f,.54f,1};c[ImGuiCol_Header]={.13f,.27f,.35f,1};
    c[ImGuiCol_HeaderHovered]={.17f,.35f,.44f,1};c[ImGuiCol_HeaderActive]={.17f,.41f,.52f,1};
    c[ImGuiCol_CheckMark]={.34f,.79f,.90f,1}; c[ImGuiCol_SliderGrab]=c[ImGuiCol_CheckMark];
    c[ImGuiCol_TableHeaderBg]={.11f,.17f,.23f,1};c[ImGuiCol_TableRowBgAlt]={1,1,1,.025f};
    style.ScaleAllSizes(std::clamp(scale,1.f,2.f));
    style.FontScaleMain=std::clamp(scale,1.f,2.f);
    ImGui::GetStyle()=style;
}
bool section(const char* label,bool primary) {
    return ImGui::CollapsingHeader(label,primary?ImGuiTreeNodeFlags_DefaultOpen:0);
}
void note(const char* text,bool warning) {
    ImGui::PushStyleColor(ImGuiCol_Text,warning?ImVec4(.98f,.73f,.38f,1):ImVec4(.67f,.76f,.84f,1));
    ImGui::TextWrapped("%s",text);ImGui::PopStyleColor();
}
void draw_workspace(State& s,const Context& c,bool drones,const std::function<void(Page)>& body) {
    const float scale=s.scale;
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("RF Monitor workspace",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|
        ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("RF MONITOR");ImGui::SameLine();
    ImGui::TextDisabled("/ %s",page_name(s.page));
    ImGui::SameLine();if(ImGui::Button("Monitoring setup")) {s.setup_open=!s.setup_open;}
    ImGui::SameLine();if(ImGui::Button("Security")) {s.page=Page::Security;s.security_first=true;}
    if(s.legacy) {ImGui::SameLine();note("Legacy presentation");}
    ImGui::TextWrapped("Receiver: %s | %s | Active band: %s | Step: %s | Cycles: %d",
        c.radio.c_str(),!c.started?"disconnected":!c.connected?"starting / not connected":c.stalled?"RX STALLED":"connected",
        band_label(c.band).c_str(),c.step.empty()?"not reported":c.step.c_str(),c.cycles);
    if(!c.error.empty())note(c.error.c_str(),true);
    if(c.overflow)note("Capture overflow reported: continuity and observation coverage are degraded.",true);
    if(c.stalled)note("RX stalled: a connected receiver is not evidence of usable samples.",true);
    if(c.laboratory)note("Nonstandard laboratory codecs are active.",true);
    ImGui::Separator();
    const float footer=ImGui::GetTextLineHeightWithSpacing()*2;
    const bool narrow=ImGui::GetContentRegionAvail().x<1000*scale;
    if(!s.legacy && narrow) {
        if(ImGui::BeginCombo("Workspace",page_name(s.page))) {
            for(int i=0;i<8;++i){auto p=static_cast<Page>(i);if(p==Page::Drones&&!drones)continue;
                if(ImGui::Selectable(page_name(p),p==s.page)){s.page=p;if(p==Page::Security)s.security_first=true;}}
            ImGui::EndCombo();
        }
    }
    if(!s.legacy && !narrow) {
        ImGui::BeginChild("Navigation",{170*scale,-footer},ImGuiChildFlags_Borders);
        note("WORKSPACES");
        for(int i=0;i<8;++i) {
            auto p=static_cast<Page>(i);if(p==Page::Drones&&!drones)continue;
            ImGui::PushID(i);if(ImGui::Selectable(page_name(p),p==s.page,0,{0,32*scale})) {
                s.page=p;if(p==Page::Security)s.security_first=true;
            }ImGui::PopID();
        }
        ImGui::Separator();note("Viewing pages does not change acquisition.");
        ImGui::EndChild();ImGui::SameLine();
    }
    ImGui::BeginChild("Page content",{0,-footer},ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::SeparatorText(page_name(s.page));note(page_purpose(s.page));
    body(s.page);
    ImGui::EndChild();ImGui::Separator();
    ImGui::TextWrapped("Recording: %s%s | Open incidents (store): %zu | Run: %s",
        c.recording.empty()?"off":c.recording.c_str(),c.recording_pending?" (configuration changes apply on reconnect)":"",
        c.incidents,c.run.empty()?"not reported":c.run.c_str());
    ImGui::End();
}
void draw_capabilities(bool drones,bool cyclo) {
    note("Capabilities describe the current implementation. Integrity checks do not authenticate a sender.");
    const struct {const char* name;const char* scope;} items[]={
        {"Acquisition","One selected radio/band at a time. B210 profile up to 56 MSps; X310 software profile up to 20 MSps. Requested rates are not actual hardware readbacks."},
        {"Wi-Fi channels","2.4 GHz: 1-13. 5 GHz: 36/40/44/48/149/153/157/161/165. No DFS/6 GHz scan plan. Sweep gaps remain."},
        {"Wi-Fi PHY","1 Mbps DSSS / Barker-11 and legacy 20 MHz OFDM at 6/9/12/18/24/36/48/54 Mbps. No HT/VHT/HE/EHT payload decoding. Encrypted payload remains opaque."},
        {"Wi-Fi retention","1,000 packet rows; 400 normal / 4,000 extended security burst budget per capture. BSSID is a claimed network/interface; RF clusters are heuristic."},
        {"LoRa","SF7-12 explicit-header payloads; SF5/6 detection only. BW125/250/500, CR4/5-4/8. 200 hypothesis rows; no unique-aircraft/device claim."},
        {"LoRa capture / replay","Default 500 kS/s packet listener, 1-30 seconds (default 6), bounded to 32 million samples. Lock skips wideband energy scan. LoRaWAN structure only: no MIC, decryption or replay verification."},
        {"Security","Suspected disconnect flood, historical beacon replay, experimental connection/EAPOL anomalies. No MIC/key-installation, encrypted PN replay, jamming or LoRa attack detection."},
        {"Security retention","256 published recent frames / 4,096 internal; 1,000 stored incidents / 200 published. Opt-in default 64 MiB rotating event recording. Missing evidence is not a benign verdict."},
        {"Identity history","Approximately 1,000 retained readings per entry, with compaction slack to 1,100; not every reading forever. No physical-device attribution from a BSSID or RF cluster."},
        {"Excluded features","New capture indexing, evidence-bundle export, live tracking/maps, new replay adapters and exact-model ML are outside this interface revamp."}
    };
    for(const auto& item:items)if(section(item.name))note(item.scope);
    if(section("Cyclostationary"))note(cyclo?"Experimental opt-in. Four independent windows up to 65,536 samples each, total 262,144; two-second deadline. No independently validated named family. Offline-only evidence remains labelled offline.":"Not compiled in this build.");
    if(section("Drones"))note(drones?"DJI and Wi-Fi Remote ID are independent, default-off add-ons. DJI: four distributed 20 ms windows, 2M samples/window; queue 4, 128 rows. Offline DJI: 40 ms / 2M samples. Remote ID: Beacon / supported NAN v0-2, queue 128, 64 sources, 32 message slots/source, per-message stale after 10 s. No Bluetooth or signature verification. Fresh live qualification is pending.":"Drones add-ons are not compiled in this build.");
}
}

namespace rfmon::gui {
void draw_overview(State& ui,const Context& context,const Overview& data,bool drones,const std::function<void()>& draw_map) {
    const int columns=ImGui::GetContentRegionAvail().x>850*ui.scale?4:2;
    if(ImGui::BeginTable("Overview summaries",columns,ImGuiTableFlags_SizingStretchSame)) {
        auto card=[&](const char* name,const std::string& value,const char* detail,Page page){
            ImGui::TableNextColumn();ImGui::BeginChild(name,{0,150*ui.scale},ImGuiChildFlags_Borders);
            ImGui::TextDisabled("%s",name);ImGui::TextWrapped("%s",value.c_str());note(detail);
            if(ImGui::SmallButton("Inspect")){ui.page=page;if(page==Page::Security)ui.security_first=true;}
            ImGui::EndChild();};
        card("Acquisition",band_label(context.band),context.connected?"Connected; health above":"Disconnected / starting",Page::Settings);
        card("Coverage",std::to_string(data.captures)+" Wi-Fi captures","Current run; gaps matter",Page::Security);
        card("Security",std::to_string(context.incidents)+" open suspected incidents","Store scope; impact unverified",Page::Security);
        card("Observations",std::to_string(data.groups)+" retained RF groups","Not a physical-device count",band_label(context.band).find("2.4")!=std::string::npos?Page::Wifi24:band_label(context.band).find("5")!=std::string::npos?Page::Wifi5:Page::Lora);
        ImGui::EndTable();
    }
    if(!context.connected)note("No active receiver observations. Open Monitoring setup and explicitly connect.");
    if(data.queue_loss)note("Coverage degraded by security queue loss; zero incidents is not an all-clear verdict.",true);
    ImGui::SeparatorText("Current acquisition frequency map");draw_map();
    if(section("Receiver and processing details")) {
        ImGui::Text("Processing lane: %s | Wait: %.3f s | High-water jobs: %zu",data.lane_enabled?"enabled":"disabled",data.lane_wait,data.lane_peak);
        ImGui::Text("Security queue: %zu | Peak: %zu",data.queue_depth,data.queue_peak);
        note("Actual sample rate / gain readback: not uniformly reported. Inspect retained capture metadata where available.");
    }
    if(drones){ImGui::SeparatorText("Drone broadcast add-ons");ImGui::Text("DJI: %s | Wi-Fi Remote ID: %s",data.dji_enabled?"enabled":"off",data.rid_enabled?"enabled":"off");if(ImGui::Button("Inspect Drones"))ui.page=Page::Drones;}
}
}
