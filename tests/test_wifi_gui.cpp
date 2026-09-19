// Offscreen GUI smoke/interaction test. Renders the actual production Wi-Fi
// tables with controlled fixtures, without starting Scanner or accessing a radio.
#include "wifi_gui.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <ctime>
#include <fstream>
#include <iostream>
#include <vector>
using namespace rfmon;
int main(int argc, char** argv) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (!eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API)) {
        std::cerr << "Cannot initialize offscreen EGL\n"; return 1;
    }
    EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
                     EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count;
    if (!eglChooseConfig(display,attrs,&config,1,&count) || count<1) return 1;
    EGLint size[] = {EGL_WIDTH,1600,EGL_HEIGHT,1000,EGL_NONE};
    auto surface=eglCreatePbufferSurface(display,config,size);
    auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
    if (!eglMakeCurrent(display,surface,surface,context)) return 1;
    ImGui::CreateContext(); ImGui::StyleColorsDark();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2(1600,1000); io.DeltaTime=1.0f/60;
    ImGui_ImplOpenGL3_Init("#version 130");
    std::vector<wifi_master::WifiMasterRow> rows;
    wifi_master::WifiMasterRow row;
    row.device_key="3c:52:a1:0b:bf:d7"; row.key_is_mac=true;
    row.vendor="Example registered organization"; row.vendor_source="IEEE MA-L /24 3C52A1 (test fixture)";
    row.first_seen_ts=std::time(nullptr)-86400; row.last_seen_ts=std::time(nullptr);
    row.identity_ts=row.last_seen_ts; row.ssid_seen_ts=row.identity_ts; row.wps_seen_ts=row.identity_ts;
    row.ssid_source="Probe response"; row.wps_source="Probe response";
    row.identity_phy="OFDM"; row.last_phy="OFDM"; row.identity_count=12; row.monitored_channel_hz=2412e6; row.last_channel_hz=2412e6;
    wifi::BeaconInfo b; b.bssid=row.device_key; b.ssid="Lab network (test fixture)"; b.ssid_present=true;
    b.fcs_valid=true; b.frame_source="Probe response"; b.channel=9; b.channel_source="DS Parameter Set";
    b.capability=0x11; b.beacon_interval_tu=100; b.security="RSN: PSK, SAE (WPA3-Personal)";
    b.ciphers="CCMP-128 (group: CCMP-128)"; b.pmf="Capable";
    b.standards="HT (802.11n), VHT (802.11ac)"; b.wps_present=true;
    b.wps_manufacturer="Example Corp"; b.wps_model_name="Model A"; b.wps_model_number="42"; b.wps_device_name="Lab AP";
    row.identity=b; rows.push_back(row);
    row.device_key="02:00:00:00:00:02"; row.vendor="Locally administered"; row.vendor_source="No vendor inference";
    row.identity->ssid=""; row.identity->wps_model_name=""; row.identity->wps_manufacturer="";
    row.identity->wps_device_name=""; row.identity->wps_model_number=""; row.identity->wps_present=false;
    row.identity->security="Open (no privacy advertised)"; rows.push_back(row);
    row=wifi_master::WifiMasterRow{}; row.device_key="WIFI-FP-0001"; row.reading_count=10;
    row.first_seen_ts=row.last_seen_ts=std::time(nullptr); row.last_channel_hz=2437e6;
    row.latest.phy="OFDM"; row.latest.cfo_ppm=5; rows.push_back(row);
    std::vector<WifiPacketRow> packets(2);
    packets[0].time="12:00:00"; packets[0].freq_mhz=2412; packets[0].channel=1;
    packets[0].identity=b; packets[0].master_key=b.bssid; packets[0].modulation="DSSS";
    packets[0].fp_gate_reason="EVM above ceiling";
    packets[1].time="12:00:01"; packets[1].channel=6; packets[1].freq_mhz=2437;
    packets[1].modulation="OFDM"; packets[1].master_key=b.bssid;
    packets[1].identity=b; packets[1].decode_status="Decoded OFDM beacon/probe response";
    packets[1].ofdm_rate_mbps=6; packets[1].psdu_length=454; packets[1].fcs_valid=true;
    packets[0].decode_status="Decoded DSSS beacon/probe response"; packets[0].fcs_valid=true;
    auto save=[&](const char* suffix) {
        if (argc<2) return;
        std::vector<unsigned char> pixels(1600*1000*3); glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,1600,1000,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
        std::ofstream out(std::string(argv[1])+suffix,std::ios::binary);
        out << "P6\n1600 1000\n255\n";
        for (int y=999;y>=0;--y) out.write(reinterpret_cast<const char*>(pixels.data()+y*1600*3),1600*3);
    };
    int failures=0;
    for (int i=0;i<10;++i) {
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Wi-Fi identity GUI verification",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("Wi-Fi packets - test fixtures, not live captures");
        draw_wifi_packet_table(packets,200);
        ImGui::TextUnformatted("Wi-Fi Identities & RF Clusters - click an identity for details");
        draw_wifi_master_table(rows,600);
        // Query actual row geometry to exercise the production Selectable.
        if (i==2) {
            for (auto* window : ImGui::GetCurrentContext()->Windows) {
                if (std::string(window->Name).find("wifi_master_identity_v2")!=std::string::npos) {
                    io.AddMousePosEvent(window->Pos.x+60,window->Pos.y+32);
                    io.AddMouseButtonEvent(0,true);
                    break;
                }
            }
        }
        if (i==3) io.AddMouseButtonEvent(0,false);
        if (i==6 && !ImGui::IsPopupOpen("Wi-Fi identity details")) {
            std::cerr << "Identity detail popup did not open\n"; ++failures;
        }
        ImGui::End(); ImGui::Render();
        if (ImGui::GetDrawData()->TotalVtxCount<=0) ++failures;
        glViewport(0,0,1600,1000); glClearColor(.1f,.1f,.1f,1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
        if (i==1) save("-table.ppm"); if (i==7) save("-details.ppm");
        if (glGetError()!=GL_NO_ERROR) ++failures;
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext();
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context); eglDestroySurface(display,surface); eglTerminate(display);
    std::cout << "GUI render and identity interaction: " << (failures ? "FAIL" : "PASS") << '\n';
    return failures ? 1 : 0;
}
