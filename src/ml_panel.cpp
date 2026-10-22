#include "ml_panel.hpp"
#include "imgui.h"
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <GL/gl.h>
#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif
namespace rfmon::ml {
namespace {
GLuint preview_texture=0;
std::shared_ptr<const StftPreview> uploaded_preview;
ImU32 stft_colour(float t) {
    // Dark blue -> cyan -> yellow, with monotonically increasing brightness.
    const float stops[4][3]={{.025f,.035f,.12f},{.12f,.22f,.52f},{.04f,.72f,.67f},{1.f,.93f,.35f}};
    const float v=std::clamp(t,0.f,1.f)*3;
    const int i=std::min(2,int(v));const float a=v-i;
    return ImGui::ColorConvertFloat4ToU32(ImVec4(stops[i][0]*(1-a)+stops[i+1][0]*a,
        stops[i][1]*(1-a)+stops[i+1][1]*a,stops[i][2]*(1-a)+stops[i+1][2]*a,1));
}
}
void release_preview_texture() {
    if(preview_texture)glDeleteTextures(1,&preview_texture);
    preview_texture=0;uploaded_preview.reset();
}
void draw_stft_preview(const Result& result) {
    ImGui::SeparatorText("Live model STFT");
    if(!result.stft) {
        ImGui::TextWrapped("No STFT available for this observation (invalid/constant IQ, or a worker without preview support).");
        return;
    }
    const auto& p=*result.stft;
    ImGui::Text("%s | Capture %llu | %.1f seconds old",result.current?"Latest processed interval":"Historical / stale",
        (unsigned long long)result.capture,std::chrono::duration<double>(std::chrono::steady_clock::now()-result.completed).count());
    ImGui::Text("Interval %zu/%zu | Starts %.3f ms into capture",result.interval_index+1,result.interval_count,
        result.rate>0?result.offset_samples/result.rate*1000:0);
    if(uploaded_preview!=result.stft) {
        std::vector<ImU32> colours(p.pixels.size());
        for(int f=0;f<p.rows;++f)for(int t=0;t<p.cols;++t)
            colours[(p.rows-1-f)*p.cols+t]=stft_colour(p.pixels[f*p.cols+t]/255.f);
        GLint binding=0,alignment=0,row_length=0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
        glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);glGetIntegerv(GL_UNPACK_ROW_LENGTH,&row_length);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        if(!preview_texture)glGenTextures(1,&preview_texture);
        glBindTexture(GL_TEXTURE_2D,preview_texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,p.cols,p.rows,0,GL_RGBA,GL_UNSIGNED_BYTE,colours.data());
        glBindTexture(GL_TEXTURE_2D,GLuint(binding));
        glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);glPixelStorei(GL_UNPACK_ROW_LENGTH,row_length);
        uploaded_preview=result.stft;
    }
    const double center=result.center+result.selected_offset;
    const double bin_hz=p.rate_hz/p.nfft;
    const double lower_edge=center-p.rate_hz/2-bin_hz/2;
    const float width=std::max(140.f,ImGui::GetContentRegionAvail().x-112.f);
    const float height=260.f;
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(origin.x+96,origin.y));
    ImGui::Image(ImTextureID(preview_texture),ImVec2(width,height));
    auto* draw=ImGui::GetWindowDrawList();
    const ImU32 ink=ImGui::GetColorU32(ImGuiCol_Text);
    for(int tick=0;tick<=4;++tick) {
        const float fraction=tick/4.f;
        char label[64];std::snprintf(label,sizeof(label),"%.3f",(lower_edge+p.rate_hz*(1-fraction))/1e6);
        draw->AddText(ImVec2(origin.x,origin.y+fraction*(height-14)),ink,label);
        const float x=origin.x+96+fraction*width;
        std::snprintf(label,sizeof(label),"%.1f",fraction*p.duration_seconds*1000);
        draw->AddText(ImVec2(x-(tick==4?20:0),origin.y+height+4),ink,label);
    }
    if(ImGui::IsItemHovered()) {
        const auto mouse=ImGui::GetMousePos();
        const int t=std::clamp(int((mouse.x-origin.x-96)/width*p.cols),0,p.cols-1);
        const int f=p.rows-1-std::clamp(int((mouse.y-origin.y)/height*p.rows),0,p.rows-1);
        const double db=p.db_min+(p.db_max-p.db_min)*p.pixels[f*p.cols+t]/255.;
        ImGui::SetTooltip("Time %.4f ms | Frequency %.6f MHz\nRelative power %.2f dB (display-quantized)\nFrequency bin %d | Time frame %d",
            (t*p.hop+p.nfft*.5)/p.rate_hz*1000,(center+(f-p.rows/2)*bin_hz)/1e6,db,f,t);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+height+26));
    ImGui::TextUnformatted("Frequency (MHz) vertical | Time within interval (ms) horizontal");
    const auto bar=ImGui::GetCursorScreenPos();
    for(int i=0;i<128;++i)draw->AddRectFilled(ImVec2(bar.x+i*width/128,bar.y),
        ImVec2(bar.x+(i+1)*width/128,bar.y+12),stft_colour(i/127.f));
    ImGui::Dummy(ImVec2(width,14));
    ImGui::Text("Colour range %.1f to %.1f dB (per-interval scale)",p.db_min,p.db_max);
    ImGui::TextWrapped("256 frequency bins x 625 time frames; 8 ms, 20 MSps model view. Source RMS-normalized log power, before checkpoint normalization; not dBm. Full grid displayed with 8-bit colour precision. Updates on processed intervals; separated views are not continuous coverage.");
}
void draw_panel(Inference& inference,bool connected,const std::string& band) {
    static char model[2048]{},python[2048]{};
    static char audit_root[2048]{},audit_notes[1024]{};
    static bool record_audit=false;
    static bool dispersed_intervals=true;
    static int audit_context=0;
    static const char* contexts[]={"unlabelled","background_observation","controller_only","mini2_linked_idle","mini2_linked_video"};
    static bool initialized=false;
    if(!initialized){
        std::snprintf(model,sizeof(model),"%s/data/drone_ml/2026-10-14/expanded_training_01/residual_model/model.pt",PROJECT_ROOT_DIR);
        std::snprintf(python,sizeof(python),"%s/data/drone_ml/runtime-2026-10-14/bin/python",PROJECT_ROOT_DIR);
        std::snprintf(audit_root,sizeof(audit_root),"%s/data/drone_ml/live_audits",PROJECT_ROOT_DIR);
        initialized=true;
    }
    auto s=inference.snapshot();
    ImGui::TextUnformatted("EXPERIMENTAL / Aircraft-model recording-context classifier");
    ImGui::TextWrapped("Scores are uncalibrated known-class softmax values, not the probability that a drone is present. Unknown-drone rejection and live identity are not validated.");
    ImGui::BeginDisabled(s.enabled);
    ImGui::InputText("Model checkpoint",model,sizeof(model));
    ImGui::InputText("Python runtime",python,sizeof(python));
    ImGui::Checkbox("Sample across each capture (up to 3 separate intervals)",&dispersed_intervals);
    ImGui::TextWrapped("First/middle/end sampling improves temporal coverage; each model input remains 8 ms. Views share a capture and are not independent votes. The existing IQ-copy budget may reduce the interval count at high rates.");
    ImGui::Checkbox("Record exact inference inputs for review",&record_audit);
    if(record_audit) {
        ImGui::InputText("Audit output directory",audit_root,sizeof(audit_root));
        ImGui::Combo("Observed session context",&audit_context,contexts,5);
        ImGui::InputText("Session notes (location, mode, distance)",audit_notes,sizeof(audit_notes));
        ImGui::TextWrapped("Each start creates a separate session, limited to 64 intervals / 256 MiB of IQ. Context is an operator observation, not a verified training label. Stop inference before changing context. Recording is optional and uses only already-submitted intervals.");
    }
    ImGui::EndDisabled();
    if(!s.enabled) {
        if(ImGui::Button("Start Inference")) {
            Options o;o.model=model;o.python=python;
            o.dispersed_intervals=dispersed_intervals;
            o.script=std::string(PROJECT_ROOT_DIR)+"/tools/drone_ml/live_worker.py";
            if(record_audit){o.audit_root=audit_root;o.audit_context=contexts[audit_context];o.audit_notes=audit_notes;}
            inference.start(o);
        }
    }else if(ImGui::Button("Stop Inference"))inference.stop();
    ImGui::SameLine();if(ImGui::Button("Clear inference history"))inference.clear_history();
    ImGui::Text("Worker: %s",s.state.c_str());
    if(!s.error.empty())ImGui::TextWrapped("Worker error: %s",s.error.c_str());
    ImGui::Text("Audit recorder: %s | Saved intervals: %llu",s.audit_status.c_str(),(unsigned long long)s.audit_saved);
    if(!s.audit_path.empty())ImGui::TextWrapped("Audit session: %s",s.audit_path.c_str());
    if(!s.audit_error.empty())ImGui::TextWrapped("Audit recording stopped: %s (inference can continue)",s.audit_error.c_str());
    ImGui::Text("Existing receiver: %s | Selected band: %s",connected?"connected":"disconnected",band.c_str());
    ImGui::TextWrapped("Uses existing Wi-Fi IQ captures only. Start does not connect or retune the receiver. Requires a contiguous 8 ms interval; other bands or short/gapped captures produce no prediction.");
    if(!s.history.empty())draw_stft_preview(s.history.front());
    else {
        ImGui::SeparatorText("Live model STFT");
        ImGui::TextWrapped("Waiting for a processed IQ interval. Connect the receiver and Start Inference to display the model's STFT.");
    }
    ImGui::SeparatorText("Runtime and coverage");
    ImGui::Text("Queue %zu/1 (+ one processing) | Offered %llu | Submitted %llu | Processed %llu",
        s.queued,(unsigned long long)s.offered,(unsigned long long)s.submitted,(unsigned long long)s.processed);
    ImGui::Text("Skipped busy/rate limit %llu | Invalid/short %llu | Stale results %llu",
        (unsigned long long)s.dropped,(unsigned long long)s.invalid,(unsigned long long)s.stale);
    ImGui::Text("Submitted samples %llu / observed input samples %llu (not full-band monitoring coverage)",
        (unsigned long long)s.submitted_samples,(unsigned long long)s.offered_samples);
    ImGui::Text("Maximum optional copy callback: %.3f ms | Capture jobs submitted: %llu",s.max_observer_ms,(unsigned long long)s.submitted_jobs);
    ImGui::TextWrapped("Duty limit: at most 2 capture jobs/second, up to 3 intervals/job (6 intervals/second). Queue holds one job plus one processing. Busy jobs are skipped.");
    if(!s.history.empty()) {
        const auto& r=s.history.front();
        ImGui::SeparatorText("Latest observation");
        ImGui::Text("%s | %.1f seconds old",r.current?"Recent result":"Historical / stale",
            std::chrono::duration<double>(std::chrono::steady_clock::now()-r.completed).count());
        ImGui::TextWrapped("%s",observation_decision(r));
        ImGui::TextWrapped("A qualified drone-presence detector is not available. This observation cannot establish no drone, an unknown drone, or a known drone model.");
        if(ImGui::CollapsingHeader("Experimental model ranking (not detections)")) {
            ImGui::TextWrapped("Engine status: %s",r.status.c_str());
            if(!r.reason.empty())ImGui::TextWrapped("Reason: %s",r.reason.c_str());
            ImGui::Text("Top candidate: %s",r.candidate.empty()?"Unavailable":r.candidate.c_str());
            for(const auto& candidate:r.top) {
                ImGui::Text("%s: %.4f relative class score",candidate.label.c_str(),candidate.score);
            }
        }
        ImGui::Text("Processing %.1f ms | Queue + processing %.1f ms | Interval 8 ms",r.processing_ms,r.total_ms);
        ImGui::Text("View %zu/%zu | Start %.3f ms into capture (sample %llu)",r.interval_index+1,r.interval_count,
            r.rate>0?r.offset_samples/r.rate*1000:0,(unsigned long long)r.offset_samples);
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
        ImGui::TextUnformatted("Presence detector: unavailable. Identity acceptance: unavailable.");
        ImGui::TextUnformatted("Classes are training coverage, not validated identity coverage.");
        ImGui::TextUnformatted(s.classes.c_str());
    }
    if(ImGui::CollapsingHeader("Diagnostic candidate history - not detections (maximum 100)")) {
        if(ImGui::BeginTable("Model observations",4,ImGuiTableFlags_Borders|ImGuiTableFlags_ScrollY,ImVec2(0,220))){
            ImGui::TableSetupColumn("Capture");ImGui::TableSetupColumn("Decision");
            ImGui::TableSetupColumn("Unverified candidate");ImGui::TableSetupColumn("Relative class score");ImGui::TableHeadersRow();
            for(const auto& r:s.history){
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%llu",(unsigned long long)r.capture);
                ImGui::TableNextColumn();ImGui::TextUnformatted(observation_decision(r));
                ImGui::TableNextColumn();ImGui::TextUnformatted(r.candidate.c_str());
                ImGui::TableNextColumn();if(r.top.empty())ImGui::TextUnformatted("Unavailable");else ImGui::Text("%.4f",r.top.front().score);
            }ImGui::EndTable();
        }
    }
}
}
