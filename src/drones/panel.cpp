#include "panel.hpp"
#include "imgui.h"
#include <algorithm>
#include <map>

namespace drone {
void draw_results(const Snapshot& s, bool offline) {
    ImGui::PushID(offline?"offline":"live");
    ImGui::TextWrapped("%s",s.status.c_str());
    ImGui::Text("Checksum-valid observations: %llu | Queue: %zu / 4",(unsigned long long)s.decoded,s.queue_depth);
    if(s.dropped || s.analysis_errors)ImGui::TextColored(ImVec4(1,.7f,.35f,1),"Drops: %llu samples | Worker errors: %llu",(unsigned long long)s.dropped,(unsigned long long)s.analysis_errors);
    if(ImGui::CollapsingHeader("Processing diagnostics")) {
    ImGui::Text("Decoded packets: %llu   Candidates: %llu   Rejected: %llu",
        (unsigned long long)s.decoded,(unsigned long long)s.candidates,(unsigned long long)s.rejected);
    ImGui::Text("Analysed samples: %llu   Queue: %zu / 4   Dropped samples: %llu",
        (unsigned long long)s.samples,s.queue_depth,(unsigned long long)s.dropped);
    ImGui::Text("Decode: %.1f ms   Queue age: %.1f ms   Errors: %llu",
        s.processing_ms,s.oldest_queue_ms,(unsigned long long)s.analysis_errors);
    ImGui::Text("Preprocessing %.3f ms | Current queue age %.3f ms | End-to-end %.3f ms",s.preprocessing_ms,s.queue_age_ms,s.end_to_end_ms);
    ImGui::Text("Discontinuities %llu | Partial discarded %llu | Stop discarded %llu",(unsigned long long)s.discontinuities,(unsigned long long)s.partial_discarded,(unsigned long long)s.stop_discarded);
    }
    if(s.observations.empty()) {
        ImGui::Spacing(); ImGui::TextWrapped("No checksum-valid DJI DroneID broadcast decoded. This does not establish that no drone is present.");
        ImGui::PopID(); return;
    }
    static std::string live_selected, replay_selected;
    auto& selected=offline?replay_selected:live_selected;
    if(selected.empty()) selected=s.observations.front().serial;
    const auto flags=ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY;
    if(ImGui::BeginTable("Broadcasts",5,flags,ImVec2(0,155))) {
        ImGui::TableSetupColumn("Reported serial",ImGuiTableColumnFlags_WidthFixed,210);
        ImGui::TableSetupColumn("Reported model",ImGuiTableColumnFlags_WidthFixed,180);
        ImGui::TableSetupColumn("RX centre (MHz)",ImGuiTableColumnFlags_WidthFixed,125);
        ImGui::TableSetupColumn("Packets",ImGuiTableColumnFlags_WidthFixed,75);
        ImGui::TableSetupColumn("Freshness",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
        for(const auto& o:s.observations) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::PushID(o.serial.c_str());
            if(ImGui::Selectable(o.serial.c_str(),selected==o.serial,ImGuiSelectableFlags_SpanAllColumns)) selected=o.serial;
            ImGui::PopID(); ImGui::TableNextColumn(); ImGui::TextUnformatted(o.model.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f",o.frequency/1e6);
            ImGui::TableNextColumn(); ImGui::Text("%llu",(unsigned long long)o.count);
            ImGui::TableNextColumn();
            const double age=std::chrono::duration<double>(std::chrono::steady_clock::now()-o.acquired_at).count();
            if(offline) ImGui::TextUnformatted("OFFLINE recording");
            else ImGui::Text("%s | received for processing %.1f s ago",!s.ready?"RETAINED":age>10?"STALE":"RECENT",age);
        }
        ImGui::EndTable();
    }
    auto it=std::find_if(s.observations.begin(),s.observations.end(),[&](const Observation& o){return o.serial==selected;});
    if(it!=s.observations.end()) {
        const auto& o=*it;
        ImGui::SeparatorText("Selected broadcast details");
        ImGui::TextWrapped("%s",o.evidence.c_str());
        ImGui::TextWrapped("Source: %s | Receiver epoch: %llu",o.source.c_str(),(unsigned long long)o.receiver_epoch);
        ImGui::Text("Sequence: %lld | Source-clock time: %.9f s | Relative power: %.1f dB",
            (long long)o.sequence,o.timestamp,o.power);
        ImGui::Text("Input: %.3f MSps | %s symbol format | Carrier-bin correction: %.0f Hz",o.sample_rate/1e6,o.legacy?"Legacy":"Current",o.carrier_correction_hz);
        ImGui::TextWrapped("Aircraft position: %s",o.position_status.c_str());
        if(ImGui::Button("Copy complete decoded packet JSON")) ImGui::SetClipboardText(o.packet_json.c_str());
        static ImGuiTextFilter fields;
        fields.Draw("Find telemetry field",280);
        int expansion=0;
        if(ImGui::SmallButton("Expand details"))expansion=1;
        ImGui::SameLine();if(ImGui::SmallButton("Collapse details"))expansion=-1;
        std::map<std::string,std::vector<const TelemetryField*>> groups;
        for(const auto& f:o.telemetry)if(fields.PassFilter((f.group+" "+f.name+" "+f.value+" "+f.status).c_str()))groups[f.group].push_back(&f);
        for(const auto& group:groups) {
            ImGui::PushID(group.first.c_str());
            if(expansion || fields.IsActive())ImGui::SetNextItemOpen(expansion>0 || fields.IsActive());
            const std::string label=group.first+" ("+std::to_string(group.second.size())+" fields)###telemetrygroup";
            if(ImGui::CollapsingHeader(label.c_str(),group.first=="Positions"?ImGuiTreeNodeFlags_DefaultOpen:0)) {
                if(ImGui::BeginTable("Telemetry",3,flags)) {
                    ImGui::TableSetupColumn("Field",ImGuiTableColumnFlags_WidthFixed,220);
                    ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthFixed,220);
                    ImGui::TableSetupColumn("Validity / interpretation",ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    for(const auto* f:group.second) {
                        ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextWrapped("%s",f->name.c_str());
                        ImGui::TableNextColumn();ImGui::TextWrapped("%s",f->value.c_str());
                        ImGui::TableNextColumn();ImGui::TextWrapped("%s",f->status.c_str());
                    }ImGui::EndTable();
                }
            }ImGui::PopID();
        }
        if(ImGui::CollapsingHeader("Raw decoded packet JSON")) ImGui::TextWrapped("%s",o.packet_json.c_str());
    }
    else ImGui::TextDisabled("Selected observation no longer retained; select another row.");
    ImGui::PopID();
}
void draw_remote_id(const RidJson& s) {
    ImGui::TextWrapped("Unauthenticated broadcast claims. Aircraft category is not exact-model classification. Accuracy is transmitter-reported.");
    if(!s.value("enabled",false))ImGui::TextDisabled("Decoder disabled; retained observations are historical.");
    if(s.value("dropped",uint64_t(0)) || s.value("errors",uint64_t(0)))ImGui::TextColored(ImVec4(1,.7f,.35f,1),"Queue drops: %llu | Errors: %llu",(unsigned long long)s.value("dropped",uint64_t(0)),(unsigned long long)s.value("errors",uint64_t(0)));
    if(ImGui::CollapsingHeader("Remote ID processing diagnostics")) {
        ImGui::Text("Examined management frames: %llu | Accepted packs: %llu | Not decoded: %llu",(unsigned long long)s.value("examined",uint64_t(0)),(unsigned long long)s.value("accepted",uint64_t(0)),(unsigned long long)s.value("not_decoded",uint64_t(0)));
        ImGui::Text("Queue: %zu / 128 | Queue drops: %llu | Errors: %llu | Out of order: %llu",s.value("queue_depth",size_t(0)),(unsigned long long)s.value("dropped",uint64_t(0)),(unsigned long long)s.value("errors",uint64_t(0)),(unsigned long long)s.value("out_of_order",uint64_t(0)));
        ImGui::TextWrapped("Ordinary Beacons can be not decoded; this is not a false-drone-alarm counter. Up to 64 sources / 32 message slots each. Message stale after 10 s; inactivity pruning after five minutes happens on the next supported pack.");
    }
    if(!s.contains("tracks")||s["tracks"].empty()){ImGui::TextWrapped("No supported Wi-Fi Remote ID messages decoded. Ordinary Wi-Fi traffic is not drone evidence.");return;}
    static std::string selected;static ImGuiTextFilter filter;
    if(selected.empty())selected=s["tracks"][0].at("key").get<std::string>();
    filter.Draw("Search retained Remote ID sources",280);
    ImGui::SameLine();if(ImGui::SmallButton("Clear source filter"))filter.Clear();
    ImGui::TextDisabled("Retained broadcast sources: %zu / 64; not a physical-aircraft count.",s["tracks"].size());
    if(ImGui::BeginTable("Remote ID sources",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY,ImVec2(0,155))) {
        for(const char* name:{"Reported ID (select)","Aircraft category","Source MAC","Transport","Version","Identity freshness"})ImGui::TableSetupColumn(name,ImGuiTableColumnFlags_WidthFixed,170);
        ImGui::TableSetupScrollFreeze(1,1);ImGui::TableHeadersRow();
        for(const auto& t:s["tracks"]) {
            std::string id="ID unavailable",category="Not reported";
            for(const auto& m:t.at("messages"))if(m.at("type")=="Basic ID") {id=m.value("reported_id",m.value("reported_id_hex",std::string()));category=m.at("ua_type_name").get<std::string>();break;}
            if(!filter.PassFilter((id+" "+category+" "+t.at("transmitter").get<std::string>()+" "+t.at("transport").get<std::string>()).c_str()))continue;
            ImGui::PushID(t.at("key").get_ref<const std::string&>().c_str());ImGui::TableNextRow();ImGui::TableNextColumn();
            if(ImGui::Selectable(id.c_str(),selected==t.at("key").get<std::string>()))selected=t.at("key").get<std::string>();
            ImGui::TableNextColumn();ImGui::TextWrapped("%s",category.c_str());
            ImGui::TableNextColumn();ImGui::TextUnformatted(t.at("transmitter").get_ref<const std::string&>().c_str());
            ImGui::TableNextColumn();ImGui::TextUnformatted(t.at("transport").get_ref<const std::string&>().c_str());
            ImGui::TableNextColumn();ImGui::Text("%d",t.at("protocol_version").get<int>());
            ImGui::TableNextColumn();ImGui::TextWrapped("%s",t.at("identity_status").get_ref<const std::string&>().c_str());ImGui::PopID();
        }ImGui::EndTable();
    }
    auto it=std::find_if(s["tracks"].begin(),s["tracks"].end(),[&](const RidJson& t){return t.at("key").get<std::string>()==selected;});
    if(it==s["tracks"].end()){ImGui::TextDisabled("Selected source no longer retained; select another row.");return;}
    const auto& t=*it;ImGui::PushID(selected.c_str());
    ImGui::SeparatorText("Selected Remote ID source");
    ImGui::TextWrapped("%s | %s | protocol v%d",t.at("transmitter").get_ref<const std::string&>().c_str(),t.at("transport").get_ref<const std::string&>().c_str(),t.at("protocol_version").get<int>());
            ImGui::Text("Packs: %llu | %s",(unsigned long long)t.at("packs").get<uint64_t>(),t.at("identity_status").get_ref<const std::string&>().c_str());
            for(const auto& m:t.at("messages")) {
                if(m.at("type")=="Basic ID") {
                    const std::string id=m.value("reported_id",m.value("reported_id_hex",std::string()));
                    ImGui::TextWrapped("Reported ID: %s | %s | %s | age %.1f s",id.c_str(),m.at("ua_type_name").get_ref<const std::string&>().c_str(),m.at("stale").get<bool>()?"STALE":"recently received",m.at("age_s").get<double>());
                }
                if(m.at("type")=="Location") {
                    if(m.at("latitude").is_number()&&m.at("longitude").is_number())ImGui::TextWrapped("Reported aircraft position: %.7f, %.7f deg | %s | age %.1f s",m.at("latitude").get<double>(),m.at("longitude").get<double>(),m.at("stale").get<bool>()?"STALE":"recently received",m.at("age_s").get<double>());
                    else ImGui::TextUnformatted("Reported aircraft position: unavailable");
                }
            }
            if(ImGui::Button("Copy Remote ID evidence JSON"))ImGui::SetClipboardText(t.dump(2).c_str());
            static ImGuiTextFilter field_search;
            field_search.Draw("Find message field",280);
            int expand=0;if(ImGui::SmallButton("Expand details"))expand=1;
            ImGui::SameLine();if(ImGui::SmallButton("Collapse details"))expand=-1;
            for(const auto& m:t.at("messages")){
                if(field_search.IsActive()&&!field_search.PassFilter(m.dump().c_str()))continue;
                std::string label=m.at("type").get<std::string>();
                if(m.contains("id_type_name"))label+=" / "+m.at("id_type_name").get<std::string>();
                if(m.contains("data_page"))label+=" / type "+std::to_string(m.at("auth_type").get<int>())+" page "+std::to_string(m.at("data_page").get<int>());
                label+=m.at("stale").get<bool>()?" | STALE":" | recently received";
                label+=" | "+std::to_string(int(m.at("age_s").get<double>()))+" s";
                std::string slot=m.at("type").get<std::string>()+"/"+m.value("id_type_name",std::string())+"/"+std::to_string(m.value("data_page",-1))+"/"+std::to_string(m.value("auth_type",-1));
                ImGui::PushID(slot.c_str());
                if(expand || field_search.IsActive())ImGui::SetNextItemOpen(expand>0 || field_search.IsActive());
                label+="###message";
                if(m.at("type")=="Authentication")ImGui::TextDisabled("Authentication page bytes are unverified; no signature check.");
                if(ImGui::TreeNodeEx(label.c_str(),m.at("type")=="Basic ID"?ImGuiTreeNodeFlags_DefaultOpen:0)){
                    ImGui::Text("Age: %.1f s",m.at("age_s").get<double>());
                    ImGui::TextWrapped("%s",m.at("source").at("age_basis").get_ref<const std::string&>().c_str());
                    if(m.at("type")=="System") ImGui::TextWrapped("Position belongs to the operator; it is not automatically a home/RTH location. Geodetic altitude uses WGS84 ellipsoid reference.");
                    if(m.at("type")=="Location") ImGui::TextWrapped("Coordinates: degrees. Direction: degrees true north. Speeds: m/s. Geodetic altitude: metres above WGS84 ellipsoid; barometric altitude and takeoff/ground height are separate. Message timestamp: seconds past UTC hour.");
                    if(ImGui::BeginTable("Fields",2,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable)){
                        ImGui::TableSetupColumn("Field",ImGuiTableColumnFlags_WidthFixed,300);ImGui::TableSetupColumn("Reported value");
                        for(auto it=m.begin();it!=m.end();++it){if(it.key()=="source"||it.key()=="raw_hex")continue;ImGui::TableNextRow();ImGui::TableNextColumn();std::string key=it.key();std::replace(key.begin(),key.end(),'_',' ');ImGui::TextWrapped("%s",key.c_str());ImGui::TableNextColumn();std::string value=it.value().is_null()?"Unavailable / not carried":it.value().is_string()?it.value().get<std::string>():it.value().dump();ImGui::TextWrapped("%s",value.c_str());}
                        ImGui::EndTable();
                    }
                    if(ImGui::TreeNode("Raw message and acquisition provenance")){ImGui::TextWrapped("Message: %s",m.at("raw_hex").get_ref<const std::string&>().c_str());ImGui::TextWrapped("%s",m.at("source").dump(2).c_str());ImGui::TreePop();}
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
    ImGui::PopID();
}
void draw_panel(Addon& a,const std::string& band,bool connected, const char* select_tab) {
    ImGui::SeparatorText("Drone Detection");
    ImGui::TextWrapped("Receive-only observers on the existing receiver. Reported broadcast identities are unauthenticated. Model candidates and experimental O4 evidence are not validated drone identities.");
    if(ImGui::BeginTabBar("Drone sources")) {
        if(ImGui::BeginTabItem("Drone Detection")) {
            if(a.unified) { if(ImGui::Button("Stop unified detection"))a.stop_unified(); }
            else if(ImGui::Button("Start unified detection"))a.start_unified();
            ImGui::SameLine();ImGui::Text("Receiver: %s | %s",connected?"connected":"disconnected",band.c_str());
            auto classifier=a.classifier_state?a.classifier_state():Addon::ClassifierState{};
            ImGui::TextColored(ImVec4(1,.7f,.35f,1),"%s",a.unified?"DIAGNOSTIC / DEGRADED - presence model not qualified":"Unified service stopped - independent diagnostic workers may still be active");
            ImGui::TextWrapped("Mandatory classifier: %s. %s",classifier.enabled?(classifier.ready?"running":"starting / unavailable"):"OFF / service incomplete",classifier.detail.c_str());
            ImGui::TextWrapped("No drone-absence decision is available. Existing ML uses sparse 8 ms views independently of decoder success. Inspect scores and STFTs in Model Inference. Scan gaps, overload, weak signals and unmonitored bands can miss drones.");
            ImGui::TextDisabled("Stop affects workers started here; independently started workers retain their controls and history.");
            ImGui::TextWrapped("An already running classifier is reused. Otherwise this starts the existing default checkpoint; start a custom checkpoint in Model Inference first.");
            const auto native=a.native.snapshot();
            ImGui::TextWrapped("Native backend: %s | legacy fallback: %s | Wi-Fi Remote ID: %s",native.status.c_str(),a.live.running()?"running":"OFF",a.remote_id.enabled()?"enabled":"OFF");
            if(ImGui::CollapsingHeader("Native DJI packets and O4 structure evidence",ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("DJI DroneID packet parsing is not full O1/O2/O3/O3+ control/video decoding. Native telemetry covers fewer packet formats than the retained legacy fallback. O4 is experimental; O4+ remains unqualified.");
                ImGui::Text("CRC24 packets: %llu | O4 windows: %llu | candidate windows: %llu (not drone count)",
                    (unsigned long long)native.native_packet_count,(unsigned long long)native.o4_windows,(unsigned long long)native.o4_candidates);
                ImGui::Text("Processed samples: %llu | queue: %zu | dropped samples: %llu | errors: %llu",
                    (unsigned long long)native.samples,native.queue_depth,(unsigned long long)native.dropped,(unsigned long long)native.analysis_errors);
                ImGui::Text("Native compute: %.1f ms | queue age: %.1f ms | end-to-end: %.1f ms",native.processing_ms,native.queue_age_ms,native.end_to_end_ms);
                ImGui::TextWrapped("At most 160 ms contiguous input per capture; queue holds one job. Gaps reset temporal evidence. Strongest-channel selection can miss a simultaneous weaker emitter. O4 windows need 80/160 ms supporting history; no candidates is not proof of absence.");
                if(!native.o4_latest.empty()&&ImGui::TreeNode("Latest O4 evidence (retained; not an identity)")) {
                    ImGui::TextWrapped("%s",native.o4_latest.c_str());ImGui::TreePop();
                }
                if(ImGui::TreeNode("Native packet inspector (last 64 receptions)")) {
                    ImGui::Text("Packet rows omitted at worker: %llu",(unsigned long long)native.native_rows_omitted);
                    for(size_t i=0;i<native.native_packets.size();++i) {
                        ImGui::PushID(int(i));
                        if(ImGui::TreeNode("Packet")){ImGui::TextWrapped("%s",native.native_packets[i].c_str());ImGui::TreePop();}
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            }
            if(ImGui::CollapsingHeader("DJI broadcast telemetry / legacy fallback",ImGuiTreeNodeFlags_DefaultOpen))
                draw_results(a.live.snapshot(),false);
            if(ImGui::CollapsingHeader("Wi-Fi Open Drone ID",ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("Beacon and NAN message packs, protocol versions 0 / 1 / 2. Uses complete decoded Wi-Fi frames. No Bluetooth or signature verification; unsupported PHYs and scan gaps limit coverage.");
                static RidJson rid;static double last=-1;static bool last_enabled=false;
                const bool enabled=a.remote_id.enabled();
                if(rid.is_null()||ImGui::GetTime()-last>.25||enabled!=last_enabled){rid=a.remote_id.snapshot();last=ImGui::GetTime();last_enabled=enabled;}
                draw_remote_id(rid);
            }
            if(ImGui::CollapsingHeader("Coverage and independent diagnostic controls")) {
                ImGui::Text("Native offered: %llu | not selected: %llu | unusable captures: %llu | discontinuities: %llu",
                    (unsigned long long)a.native_offered.load(),(unsigned long long)a.native_omitted.load(),
                    (unsigned long long)a.native_unhealthy.load(),(unsigned long long)native.discontinuities);
                ImGui::Text("Maximum native observer copy: %.3f ms (receiver processing lane)",a.native_max_observer_ns.load()*1e-6);
                ImGui::Text("Legacy offered: %llu | not selected: %llu | unusable captures: %llu",
                    (unsigned long long)a.offered.load(),(unsigned long long)a.omitted.load(),(unsigned long long)a.unhealthy.load());
                ImGui::TextWrapped("Legacy fallback samples up to four separate 20 ms windows (2M samples each). Counters are samples/jobs, not a probability of detecting every drone. Independent controls are available when the unified service is stopped.");
                ImGui::BeginDisabled(a.unified);
                if(a.live.running()){if(ImGui::Button("Stop legacy DJI diagnostic"))a.live.stop();}
                else if(ImGui::Button("Start legacy DJI diagnostic"))a.live.start();
                bool rid_enabled=a.remote_id.enabled();if(ImGui::Checkbox("Independent Wi-Fi Remote ID",&rid_enabled))a.remote_id.enable(rid_enabled);
                ImGui::EndDisabled();
            }
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Offline IQ review", nullptr, select_tab && std::string(select_tab)=="Offline IQ review" ? ImGuiTabItemFlags_SetSelected : 0)) {
            static char path[1024]=""; static double rate=50e6,frequency=2444.5e6,offset=0;
            ImGui::TextWrapped("Raw little-endian complex-float32 IQ only. Enter the recording's actual metadata. Reads at most 40 ms / 2M samples; source files are not modified. Results remain separate from live broadcasts.");
            ImGui::TextUnformatted("Raw IQ file path");
            ImGui::SetNextItemWidth(-1); ImGui::InputText("##IQ path",path,sizeof(path));
            ImGui::SetNextItemWidth(180); ImGui::InputDouble("Sample rate (Hz)",&rate,0,0,"%.0f");
            ImGui::SetNextItemWidth(180); ImGui::InputDouble("Centre frequency (Hz)",&frequency,0,0,"%.0f");
            ImGui::SetNextItemWidth(180); ImGui::InputDouble("Offset (seconds)",&offset,0,0,"%.6f");
            ImGui::BeginDisabled(a.replay_busy); if(ImGui::Button("Decode interval")) a.load(path,rate,frequency,offset); ImGui::EndDisabled();
            ImGui::TextWrapped("%s",a.replay_status().c_str());
            draw_results(a.replay.snapshot(),true);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
}
