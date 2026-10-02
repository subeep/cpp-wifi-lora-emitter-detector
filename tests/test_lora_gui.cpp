// Renders real LoRa widgets offscreen, without a Scanner or radio.
#include "lora_gui.hpp"
#include "lora_capture.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <fstream>
#include <iostream>
using namespace rfmon;
int main(int argc, char** argv) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (!eglInitialize(display,nullptr,nullptr) || !eglBindAPI(EGL_OPENGL_API)) return 1;
    EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
                     EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count;
    if (!eglChooseConfig(display,attrs,&config,1,&count) || count<1) return 1;
    EGLint size[] = {EGL_WIDTH,1800,EGL_HEIGHT,650,EGL_NONE};
    auto surface=eglCreatePbufferSurface(display,config,size);
    auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
    if (!eglMakeCurrent(display,surface,surface,context)) return 1;
    ImGui::CreateContext(); ImGui::StyleColorsDark();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2(1800,650); io.DeltaTime=1.0f/60;
    ImGui_ImplOpenGL3_Init("#version 130");
    std::vector<LoraPacketRow> rows;
    for (int i=0;i<5;++i) {
        LoraPacketRow r; r.time="12:00:00"; r.freq_mhz=866.9; r.sf=7; r.bandwidth_khz=125;
        if (i==0) { r.status="Detected only"; r.detail="Preamble only; header unresolved."; }
        else { r.decoder="LoRa explicit PHY"; r.sync_word=0x34; r.ldro=false; r.ldro_ambiguous=i!=4; r.cr=1; r.payload_len=3;
               set_lora_integrity(r,i!=1,i!=2,i==4);
               if (r.payload_complete) { r.payload_hex="01 02 03"; r.payload_repr="..."; } }
        rows.push_back(r);
    }
    if (argc>2) {
        auto capture=load_lora_capture(argv[2]);
        auto actual=analyze_lora_capture(capture.iq,capture.sample_rate_hz,capture.requested_center_hz);
        bool valid=false;
        for(auto& row:actual) { row.time="Hardware replay"; if(row.crc==LoraPacketRow::Crc::Valid) valid=true; rows.push_back(std::move(row)); }
        if(!valid) return 1;
    }
    LoraPacketRow wan;
    wan.time="Synthetic MAC";wan.status="Payload / no CRC";wan.header_valid=true;wan.payload_complete=true;
    wan.crc=LoraPacketRow::Crc::Absent;wan.decoder="LoRa explicit PHY";
    wan.lorawan_candidate="Unconfirmed downlink candidate";
    wan.lorawan_detail="Synthetic display fixture. DevAddr=01020304 FCnt16=4660; MIC not verified.";
    wan.inverted_iq=true;wan.preamble_peak_ratio=.93;wan.sfd_peak_ratio=.92;wan.cfo_hz=-123.5;
    wan.drift_hz_per_symbol=.25;wan.capture_offset_s=.125;wan.fec_disagreements=0;
    rows.push_back(wan);
    for (int frame=0;frame<3;++frame) {
        if (argc>3 && frame>0) {
            for(auto* window : ImGui::GetCurrentContext()->Windows)
                if(window->ScrollMax.x>0) ImGui::SetScrollX(window, window->ScrollMax.x);
        }
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("LoRa interpretation - synthetic display fixtures");
        ImGui::TextWrapped("Each row is an SF/BW hypothesis. CRC does not establish protocol identity or real-radio interoperability.");
        lora_security::Snapshot security;security.captures=3;security.excluded=1;security.input_loss=2;
        security.health="Incomplete coverage; security input excluded";security.sampled_s=6;security.analysed_s=4;security.usable_s=2;
        security.latest.capture_seq=3;security.latest.center_hz=866900000;security.latest.rate_hz=500000;security.latest.radio_session=1;
        lora_security::ProfileCoverage profile;profile.center_hz=866900000;profile.rate_hz=500000;
        profile.gain_db=20;profile.sampled_s=6;profile.usable_s=2;security.coverage["synthetic-display"]=profile;
        ImGui::SetNextItemOpen(true,ImGuiCond_Always);draw_lora_security_panel(security);
        draw_lora_packet_table(rows,180);
        ImGui::SetNextItemOpen(true,ImGuiCond_Always); draw_lora_replay_panel();
        ImGui::End(); ImGui::Render();
        if (ImGui::GetDrawData()->TotalVtxCount<=0) return 1;
        glViewport(0,0,1800,650); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
        if (glGetError()!=GL_NO_ERROR) return 1;
    }
    if (argc>1) {
        std::vector<unsigned char> pixels(1800*650*3); glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,1800,650,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
        std::ofstream out(argv[1],std::ios::binary); out << "P6\n1800 650\n255\n";
        for (int y=649;y>=0;--y) out.write(reinterpret_cast<const char*>(pixels.data()+y*1800*3),1800*3);
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext();
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context); eglDestroySurface(display,surface); eglTerminate(display);
    std::cout << "PASS: LoRa security coverage panel, integrity table and replay controls rendered offscreen\n";
}
