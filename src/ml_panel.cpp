#include "ml_panel.hpp"
#include "imgui.h"
#include <cstdio>
#include <filesystem>
#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif
namespace rfmon::ml {
void draw_panel(Inference& inference,bool connected,const std::string& band) {
    static char model[2048]{},python[2048]{};
    static bool initialized=false;
    if(!initialized){
        std::snprintf(model,sizeof(model),"%s/data/drone_ml/2026-10-14/expanded_training_01/residual_model/model.pt",PROJECT_ROOT_DIR);
        std::snprintf(python,sizeof(python),"%s/data/drone_ml/runtime-2026-10-14/bin/python",PROJECT_ROOT_DIR);
        initialized=true;
    }
    auto s=inference.snapshot();
    ImGui::TextUnformatted("EXPERIMENTAL / Aircraft-model recording-context classifier");
    ImGui::TextWrapped("Scores are uncalibrated known-class softmax values, not the probability that a drone is present. Unknown-drone rejection and live identity are not validated.");
    ImGui::BeginDisabled(s.enabled);
    ImGui::InputText("Model checkpoint",model,sizeof(model));
    ImGui::InputText("Python runtime",python,sizeof(python));
    ImGui::EndDisabled();
    if(!s.enabled) {
        if(ImGui::Button("Start Inference")) {
            Options o;o.model=model;o.python=python;
            o.script=std::string(PROJECT_ROOT_DIR)+"/tools/drone_ml/live_worker.py";
            inference.start(o);
        }
    }else if(ImGui::Button("Stop Inference"))inference.stop();
    ImGui::SameLine();if(ImGui::Button("Clear inference history"))inference.clear_history();
    ImGui::Text("Worker: %s",s.state.c_str());
    if(!s.error.empty())ImGui::TextWrapped("Worker error: %s",s.error.c_str());
    ImGui::Text("Existing receiver: %s | Selected band: %s",connected?"connected":"disconnected",band.c_str());
    ImGui::TextWrapped("Uses existing Wi-Fi IQ captures only. Start does not connect or retune the receiver. Requires a contiguous 8 ms interval; other bands or short/gapped captures produce no prediction.");
    ImGui::SeparatorText("Runtime and coverage");
    ImGui::Text("Queue %zu/1 (+ one processing) | Offered %llu | Submitted %llu | Processed %llu",
        s.queued,(unsigned long long)s.offered,(unsigned long long)s.submitted,(unsigned long long)s.processed);
    ImGui::Text("Skipped busy/rate limit %llu | Invalid/short %llu | Stale results %llu",
        (unsigned long long)s.dropped,(unsigned long long)s.invalid,(unsigned long long)s.stale);
    ImGui::Text("Submitted samples %llu / observed input samples %llu (not full-band monitoring coverage)",
        (unsigned long long)s.submitted_samples,(unsigned long long)s.offered_samples);
    ImGui::Text("Maximum optional copy callback: %.3f ms | Duty limit: at most 2 submitted intervals/second",s.max_observer_ms);
    if(!s.history.empty()) {
        const auto& r=s.history.front();
        ImGui::SeparatorText("Latest observation");
        ImGui::Text("%s | %.1f seconds old",r.current?"Recent result":"Historical / stale",
            std::chrono::duration<double>(std::chrono::steady_clock::now()-r.completed).count());
        ImGui::TextWrapped("Decision: %s",r.status.c_str());
        ImGui::Text("Accepted identity: %s",r.accepted.empty()?"Unavailable":r.accepted.c_str());
        ImGui::Text("Top candidate: %s",r.candidate.empty()?"Unavailable":r.candidate.c_str());
        if(!r.reason.empty())ImGui::TextWrapped("Reason: %s",r.reason.c_str());
        for(const auto& candidate:r.top) {
            ImGui::Text("%s: %.2f%% uncalibrated score",candidate.label.c_str(),candidate.score*100);
            ImGui::ProgressBar(float(candidate.score),ImVec2(-1,0),"");
        }
        ImGui::Text("Processing %.1f ms | Queue + processing %.1f ms | Interval 8 ms",r.processing_ms,r.total_ms);
        ImGui::Text("Capture %llu | Session %llu | Centre %.6f MHz | Input %.3f MSps",
            (unsigned long long)r.capture,(unsigned long long)r.session,r.center/1e6,r.rate/1e6);
        if(r.selected_offset_available)ImGui::Text("Selected model view: %.6f MHz (offset %+.3f MHz)",
            (r.center+r.selected_offset)/1e6,r.selected_offset/1e6);
        ImGui::TextWrapped("Receiver: %s | Source: %s | Run: %s",r.device.c_str(),r.source.c_str(),r.run.c_str());
        if(ImGui::CollapsingHeader("Exact result and capture metadata")) {
            if(ImGui::Button("Copy result JSON"))ImGui::SetClipboardText(r.raw.c_str());
            ImGui::TextWrapped("%s",r.raw.c_str());
        }
    }else ImGui::TextUnformatted("No model result yet.");
    if(ImGui::CollapsingHeader("Model details")) {
        ImGui::Text("Architecture: %s",s.architecture.c_str());
        ImGui::TextWrapped("Checkpoint SHA-256: %s",s.model_hash.c_str());
        ImGui::TextUnformatted("Input profile: 8 ms / 20 MSps model view / 256-point STFT / 625 frames.");
        ImGui::TextUnformatted("Live acceptance: unavailable. Classes are training coverage, not validated identity coverage.");
        ImGui::TextUnformatted(s.classes.c_str());
    }
    if(ImGui::CollapsingHeader("Observation history (maximum 100)")) {
        if(ImGui::BeginTable("Model observations",4,ImGuiTableFlags_Borders|ImGuiTableFlags_ScrollY,ImVec2(0,220))){
            ImGui::TableSetupColumn("Capture");ImGui::TableSetupColumn("Decision");
            ImGui::TableSetupColumn("Candidate");ImGui::TableSetupColumn("Uncalibrated score");ImGui::TableHeadersRow();
            for(const auto& r:s.history){
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%llu",(unsigned long long)r.capture);
                ImGui::TableNextColumn();ImGui::TextUnformatted(r.status.c_str());
                ImGui::TableNextColumn();ImGui::TextUnformatted(r.candidate.c_str());
                ImGui::TableNextColumn();if(r.top.empty())ImGui::TextUnformatted("Unavailable");else ImGui::Text("%.2f%%",r.top.front().score*100);
            }ImGui::EndTable();
        }
    }
}
}
